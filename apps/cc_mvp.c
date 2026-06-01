/**
 * @file cc_mvp.c
 *
 * C JIT for arm-rpi3, MVP-stage 4: locals + multi-statement + if/else.
 *
 * Grammar:
 *   program     = "int" "main" "(" ")" "{" stmt* "}"
 *   stmt        = decl_stmt | assign_stmt | if_stmt | while_stmt
 *               | return_stmt | expr_stmt | block
 *   decl_stmt   = "int" NAME "=" expr ";"
 *   assign_stmt = NAME "=" expr ";"
 *   if_stmt     = "if" "(" expr ")" stmt ("else" stmt)?
 *   while_stmt  = "while" "(" expr ")" stmt
 *   return_stmt = "return" expr ";"
 *   expr_stmt   = expr ";"
 *   block       = "{" stmt* "}"
 *   expr        = relational ( ("==" | "!=") relational )*
 *   relational  = additive   ( ("<=" | ">=" | "<" | ">") additive )*
 *   additive    = primary    ( ("+" | "-") primary )*
 *   primary     = INT_LITERAL
 *               | NAME "(" args? ")"           // function call
 *               | NAME                          // local-var load
 *   args        = expr ( "," expr )*           // 0..4 args
 *   NAME        = [_A-Za-z][_A-Za-z0-9]*
 *
 * Compiles to ARM32 (Cortex-A53 AArch32 mode), emits via memget()
 * buffer, calls in place.  AAPCS32 calling convention: args in
 * r0..r3, return in r0, ip (r12) for the call target so we can BLX
 * an arbitrary absolute address.
 *
 * Stack frame:
 *   push {fp,lr}       ; save caller's fp and link reg
 *   mov  fp, sp        ; fp points at saved-fp slot
 *   sub  sp, sp, #N*4  ; reserve N local-int slots (N rounded up to 2
 *                        for 8-byte SP alignment per AAPCS32)
 *   ... body ...
 *   mov  sp, fp        ; drop locals + intermediate pushes
 *   pop  {fp,pc}       ; return through pc, restoring fp
 *
 * Local int at slot i (0-indexed) lives at [fp, #-(i+1)*4].
 * Intermediate-value pushes (binop, function arg marshalling) happen
 * on the stack BELOW the locals, so they don't clobber them.
 *
 * Function lookup: a small hand-rolled table near the top.  Add
 * entries when you want a Pi 3 kernel function callable from JIT
 * source.  This is the bridge that lets aipl2c-generated C (Mac
 * side) reach Pi 3 runtime primitives (actor system, kprintf, …).
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>

#define SYSERR_PTR ((void *)(-1))

/* ===== builtin symbol table ===========================================
 * Each row exports one Pi 3 function (or thin wrapper) under a name the
 * JIT'd C can call.  Keep the wrappers tiny — argument marshalling and
 * format-string baking are easier here than inside the codegen. */

static int cc_b_print_int(int n)
{
    kprintf("[jit] print_int = %d\r\n", n);
    return n;
}

static int cc_b_actor_count(void)
{
    extern int abcl_n_objects(void);
    return abcl_n_objects();
}

static int cc_b_actor_age(int id)
{
    extern long abcl_object_age_ms(int);
    return (int)abcl_object_age_ms(id);
}

static int cc_b_now_ms(void)
{
    extern unsigned long clkticks;
    return (int)(clkticks * 10);
}

struct sym { const char *name; void *fn; };
static const struct sym cc_syms[] = {
    { "print_int",   (void *)&cc_b_print_int   },
    { "actor_count", (void *)&cc_b_actor_count },
    { "actor_age",   (void *)&cc_b_actor_age   },
    { "now_ms",      (void *)&cc_b_now_ms      },
    { NULL,          NULL }
};

static void *lookup_sym(const char *name, int namelen)
{
    int i;
    for (i = 0; cc_syms[i].name; i++) {
        int j = 0;
        const char *p = cc_syms[i].name;
        while (j < namelen && p[j] && p[j] == name[j]) j++;
        if (j == namelen && p[j] == '\0') return cc_syms[i].fn;
    }
    return NULL;
}

/* ===== local-variable symbol table ====================================
 * Function-level scope only — no nested-block shadowing.  Reset at the
 * start of every cc_mvp_compile_and_run so locals from a previous JIT
 * don't leak in. */
#define MAX_LOCALS 16
static struct { char name[16]; int slot; } locals[MAX_LOCALS];
static int n_locals;

static void reset_locals(void) { n_locals = 0; }

/* Return existing slot for `name` if it's already declared, else -1.
 * Used by NAME-lookup in expressions and by assignment. */
static int find_local(const char *name, int namelen)
{
    int i;
    for (i = 0; i < n_locals; i++) {
        int j = 0;
        while (j < namelen && locals[i].name[j] && locals[i].name[j] == name[j]) j++;
        if (j == namelen && locals[i].name[j] == '\0') return i;
    }
    return -1;
}

/* Allocate a fresh slot for `name`.  Returns slot index, or -1 if the
 * symbol table is full or the name is too long. */
static int alloc_local(const char *name, int namelen)
{
    if (n_locals >= MAX_LOCALS) return -1;
    if (namelen >= 16) return -1;
    int i;
    for (i = 0; i < namelen; i++) locals[n_locals].name[i] = name[i];
    locals[n_locals].name[namelen] = '\0';
    locals[n_locals].slot = n_locals;
    return n_locals++;
}

/* ===== ARM32 encoders ============================================== */
static unsigned int enc_mov_imm8(int rd, int imm)
{
    return 0xE3A00000u | ((rd & 0xF) << 12) | (imm & 0xFF);
}
static unsigned int enc_movw(int rd, int imm)
{
    return 0xE3000000u | ((rd & 0xF) << 12) | ((imm & 0xF000) << 4) | (imm & 0xFFF);
}
static unsigned int enc_movt(int rd, int imm)
{
    return 0xE3400000u | ((rd & 0xF) << 12) | ((imm & 0xF000) << 4) | (imm & 0xFFF);
}
static unsigned int enc_bx_lr(void)        { return 0xE12FFF1Eu; }
static unsigned int enc_blx_ip(void)       { return 0xE12FFF3Cu; }  /* blx r12 */
static unsigned int enc_str_push_r0(void)  { return 0xE52D0004u; }
static unsigned int enc_ldr_pop(int rd)
{
    /* ldr Rd, [sp], #4 — post-increment by 4. */
    return 0xE49D0004u | ((rd & 0xF) << 12);
}
static unsigned int enc_add_r0_r1_r0(void) { return 0xE0810000u; }
static unsigned int enc_sub_r0_r1_r0(void) { return 0xE0410000u; }
static unsigned int enc_push_lr_fp(void)   { return 0xE92D4800u; }  /* push {fp,lr} */
static unsigned int enc_pop_fp_pc(void)    { return 0xE8BD8800u; }  /* pop  {fp,pc} */
static unsigned int enc_mov_fp_sp(void)    { return 0xE1A0B00Du; }  /* mov fp, sp */
static unsigned int enc_mov_sp_fp(void)    { return 0xE1A0D00Bu; }  /* mov sp, fp */
static unsigned int enc_sub_sp_imm(int imm)
{
    /* sub sp, sp, #imm — imm encoded as imm8 (no rotation; caller
     * guarantees imm < 256, which fits our MAX_LOCALS*4 = 64 cap). */
    return 0xE24DD000u | (imm & 0xFF);
}
static unsigned int enc_ldr_r0_fp_neg(int off)
{
    /* ldr r0, [fp, #-off]  — off is positive byte offset (12-bit). */
    return 0xE51B0000u | (off & 0xFFF);
}
static unsigned int enc_str_r0_fp_neg(int off)
{
    /* str r0, [fp, #-off] */
    return 0xE50B0000u | (off & 0xFFF);
}
static unsigned int enc_cmp_r0_imm0(void) { return 0xE3500000u; }   /* cmp r0, #0 */
static unsigned int enc_cmp_r1_r0(void)   { return 0xE1510000u; }   /* cmp r1, r0 */
static unsigned int enc_beq_placeholder(void) { return 0x0A000000u; } /* offset=0 */
static unsigned int enc_b_placeholder(void)   { return 0xEA000000u; } /* offset=0 */

/* MOV{cond} r0, #1 — used to materialise the boolean result of a
 * comparison.  cond is the 4-bit ARM condition code (EQ=0, NE=1,
 * GE=A, LT=B, GT=C, LE=D).  Pattern: (cond<<28) | 0x03A00001. */
static unsigned int enc_mov_cond_r0_1(unsigned int cond)
{
    return (cond << 28) | 0x03A00001u;
}
#define COND_EQ 0x0u
#define COND_NE 0x1u
#define COND_GE 0xAu
#define COND_LT 0xBu
#define COND_GT 0xCu
#define COND_LE 0xDu

/* Patch a previously-emitted B/Bcc at code[br_at] so it branches to
 * code[target_at].  ARM imm24 = (target_pc - branch_pc - 8) / 4, where
 * branch_pc and target_pc are byte addresses.  In word-index terms:
 * imm24 = target_at - br_at - 2. */
static void patch_branch(unsigned int *code, int br_at, int target_at)
{
    int off = target_at - br_at - 2;          /* word offset */
    code[br_at] = (code[br_at] & 0xFF000000u) | ((unsigned int)off & 0x00FFFFFFu);
}

/* Emit a constant into register `rd` (0..15) using the shortest
 * available encoding.  Returns words written (1 or 2). */
static int emit_const(unsigned int *code, int at, int rd, int n)
{
    if (n >= 0 && n <= 255) {
        code[at] = enc_mov_imm8(rd, n);
        return 1;
    }
    code[at] = enc_movw(rd, n & 0xFFFF);
    if (((unsigned int)n) >> 16) {
        code[at + 1] = enc_movt(rd, ((unsigned int)n >> 16) & 0xFFFF);
        return 2;
    }
    return 1;
}

/* ===== lexer ====================================================== */
static void skip_ws(const char **cur)
{
    const char *p = *cur;
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; continue; }
        if (*p == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        break;
    }
    *cur = p;
}

/* Match literal string, advancing past it on success.  For keywords
 * (return, if, else, int) the trailing char MUST NOT be an ident
 * continuation — otherwise "intx" would tokenise as "int" + "x".
 * Punctuators ("(", "{", ";", ...) don't need this guard. */
static int match_kw(const char **cur, const char *lit)
{
    skip_ws(cur);
    const char *p = *cur;
    int i;
    for (i = 0; lit[i]; i++)
        if (p[i] != lit[i]) return 0;
    /* keyword must be followed by non-ident char */
    char next = p[i];
    if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
        (next >= '0' && next <= '9') || next == '_') return 0;
    *cur = p + i;
    return 1;
}

static int match(const char **cur, const char *lit)
{
    skip_ws(cur);
    const char *p = *cur;
    int i;
    for (i = 0; lit[i]; i++)
        if (p[i] != lit[i]) return 0;
    *cur = p + i;
    return 1;
}

static char peek(const char **cur) { skip_ws(cur); return **cur; }

static int parse_int(const char **cur, int *ok)
{
    skip_ws(cur);
    const char *p = *cur;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') { *ok = 0; return 0; }
    int n = 0;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
    *cur = p;
    *ok = 1;
    return neg ? -n : n;
}

static int is_ident_start(char c) { return (c=='_' || (c>='A'&&c<='Z') || (c>='a'&&c<='z')); }
static int is_ident_cont(char c)  { return is_ident_start(c) || (c>='0'&&c<='9'); }

/* If next token is an identifier, advance *cur past it and return
 * (1, name_start, name_len).  0 if no identifier.  Does NOT validate
 * that the identifier isn't a reserved keyword — caller's responsibility. */
static int parse_ident(const char **cur, const char **out_start, int *out_len)
{
    skip_ws(cur);
    const char *p = *cur;
    if (!is_ident_start(*p)) return 0;
    const char *s = p;
    while (is_ident_cont(*p)) p++;
    *out_start = s;
    *out_len   = (int)(p - s);
    *cur = p;
    return 1;
}

/* ===== codegen / parser ============================================ */
static int compile_expr(unsigned int *code, int at, const char **cur);
static int compile_stmt(unsigned int *code, int at, const char **cur);

/* Emit code that puts a primary into r0.  Returns words written, or -1
 * on parse error.  Primary forms:
 *   INT_LITERAL                      — mov r0, #imm
 *   NAME ( args? )                   — call builtin
 *   NAME                             — load local var
 */
static int compile_primary(unsigned int *code, int at, const char **cur)
{
    int start = at;
    skip_ws(cur);

    /* identifier? (call or var load) */
    const char *save = *cur;
    const char *name; int nlen;
    if (parse_ident(cur, &name, &nlen)) {
        skip_ws(cur);
        if (**cur == '(') {
            /* call */
            (*cur)++;
            int n_args = 0;
            skip_ws(cur);
            if (**cur != ')') {
                int w = compile_expr(code, at, cur);
                if (w < 0) return -1;
                at += w;
                code[at++] = enc_str_push_r0();
                n_args = 1;
                while (1) {
                    skip_ws(cur);
                    if (**cur != ',') break;
                    (*cur)++;
                    if (n_args >= 4) return -1;
                    w = compile_expr(code, at, cur);
                    if (w < 0) return -1;
                    at += w;
                    code[at++] = enc_str_push_r0();
                    n_args++;
                }
            }
            skip_ws(cur);
            if (**cur != ')') return -1;
            (*cur)++;
            int i;
            for (i = n_args - 1; i >= 0; i--)
                code[at++] = enc_ldr_pop(i);
            void *fn = lookup_sym(name, nlen);
            if (NULL == fn) return -1;
            unsigned int addr = (unsigned int)(unsigned long)fn;
            code[at++] = enc_movw(12, addr & 0xFFFF);
            code[at++] = enc_movt(12, (addr >> 16) & 0xFFFF);
            code[at++] = enc_blx_ip();
            return at - start;
        }
        /* var load — must be a declared local */
        int slot = find_local(name, nlen);
        if (slot < 0) return -1;
        code[at++] = enc_ldr_r0_fp_neg((slot + 1) * 4);
        return at - start;
    }

    /* int literal */
    *cur = save;
    int ok = 0;
    int n  = parse_int(cur, &ok);
    if (!ok) return -1;
    return emit_const(code, at, 0, n);
}

/* additive = primary ( ("+" | "-") primary )* */
static int compile_additive(unsigned int *code, int at, const char **cur)
{
    int start = at;
    int w = compile_primary(code, at, cur);
    if (w < 0) return -1;
    at += w;
    while (1) {
        char op = peek(cur);
        if (op != '+' && op != '-') break;
        (*cur)++;
        code[at++] = enc_str_push_r0();
        w = compile_primary(code, at, cur);
        if (w < 0) return -1;
        at += w;
        code[at++] = enc_ldr_pop(1);
        code[at++] = (op == '+') ? enc_add_r0_r1_r0() : enc_sub_r0_r1_r0();
    }
    return at - start;
}

/* Emit `cmp r1, r0 ; mov r0, #0 ; mov{cond} r0, #1` so r0 becomes
 * 1 if (left CMP right) is true under `cond`, else 0.  Left is in r1
 * (just popped), right is in r0 (just computed). */
static int emit_cmp_set(unsigned int *code, int at, unsigned int cond)
{
    code[at++] = enc_cmp_r1_r0();
    code[at++] = enc_mov_imm8(0, 0);
    code[at++] = enc_mov_cond_r0_1(cond);
    return 3;
}

/* relational = additive ( ("<=" | ">=" | "<" | ">") additive )*
 *
 * Multi-char operators must be checked BEFORE the single-char ones
 * or "<=" would mis-lex as "<" + "=". */
static int compile_relational(unsigned int *code, int at, const char **cur)
{
    int start = at;
    int w = compile_additive(code, at, cur);
    if (w < 0) return -1;
    at += w;
    while (1) {
        skip_ws(cur);
        const char *p = *cur;
        unsigned int cond;
        int oplen;
        if (p[0] == '<' && p[1] == '=') { cond = COND_LE; oplen = 2; }
        else if (p[0] == '>' && p[1] == '=') { cond = COND_GE; oplen = 2; }
        else if (p[0] == '<') { cond = COND_LT; oplen = 1; }
        else if (p[0] == '>') { cond = COND_GT; oplen = 1; }
        else break;
        *cur = p + oplen;
        code[at++] = enc_str_push_r0();
        w = compile_additive(code, at, cur);
        if (w < 0) return -1;
        at += w;
        code[at++] = enc_ldr_pop(1);
        at += emit_cmp_set(code, at, cond);
    }
    return at - start;
}

/* expr = relational ( ("==" | "!=") relational )* */
static int compile_expr(unsigned int *code, int at, const char **cur)
{
    int start = at;
    int w = compile_relational(code, at, cur);
    if (w < 0) return -1;
    at += w;
    while (1) {
        skip_ws(cur);
        const char *p = *cur;
        unsigned int cond;
        if      (p[0] == '=' && p[1] == '=') cond = COND_EQ;
        else if (p[0] == '!' && p[1] == '=') cond = COND_NE;
        else break;
        *cur = p + 2;
        code[at++] = enc_str_push_r0();
        w = compile_relational(code, at, cur);
        if (w < 0) return -1;
        at += w;
        code[at++] = enc_ldr_pop(1);
        at += emit_cmp_set(code, at, cond);
    }
    return at - start;
}

/* Compile a block "{" stmt* "}".  Returns words written, or -1. */
static int compile_block(unsigned int *code, int at, const char **cur)
{
    int start = at;
    if (!match(cur, "{")) return -1;
    while (1) {
        skip_ws(cur);
        if (**cur == '}') break;
        if (**cur == '\0') return -1;          /* unterminated block */
        int w = compile_stmt(code, at, cur);
        if (w < 0) return -1;
        at += w;
    }
    if (!match(cur, "}")) return -1;
    return at - start;
}

/* if (cond) stmt [else stmt] */
static int compile_if(unsigned int *code, int at, const char **cur)
{
    int start = at;
    if (!match(cur, "(")) return -1;
    int w = compile_expr(code, at, cur);
    if (w < 0) return -1;
    at += w;
    if (!match(cur, ")")) return -1;

    /* cmp r0, #0 ; beq L_else */
    code[at++] = enc_cmp_r0_imm0();
    int beq_at = at;
    code[at++] = enc_beq_placeholder();

    /* then-branch */
    w = compile_stmt(code, at, cur);
    if (w < 0) return -1;
    at += w;

    /* else clause? */
    skip_ws(cur);
    if (match_kw(cur, "else")) {
        /* End of then-branch jumps over else */
        int b_at = at;
        code[at++] = enc_b_placeholder();
        /* Patch BEQ to point HERE (start of else) */
        patch_branch(code, beq_at, at);
        /* Compile else-stmt */
        w = compile_stmt(code, at, cur);
        if (w < 0) return -1;
        at += w;
        /* Patch the over-else B to point HERE (after else) */
        patch_branch(code, b_at, at);
    } else {
        /* No else: BEQ jumps to end of if */
        patch_branch(code, beq_at, at);
    }
    return at - start;
}

/* Compile one statement.  Forms:
 *   "int" NAME "=" expr ";"          decl
 *   NAME "=" expr ";"                assign
 *   "if" "(" expr ")" stmt ("else" stmt)?
 *   "return" expr ";"                — sets r0 + jumps to epilogue (we
 *                                      simply emit `mov sp,fp ; pop {fp,pc}`
 *                                      inline, since cc_mvp has only one
 *                                      block-level scope so no register
 *                                      saving below needed)
 *   "{" stmt* "}"                    block
 *   expr ";"                         eval-and-discard
 *
 * Returns words written, or -1 on parse error.
 *
 * Inline epilogue for `return`: that means multiple returns each emit
 * their own 2-word epilogue.  Slightly wasteful but lets us avoid a
 * label/backpatch for the function exit. */
static int compile_stmt(unsigned int *code, int at, const char **cur)
{
    int start = at;
    skip_ws(cur);

    /* { block } */
    if (**cur == '{') {
        return compile_block(code, at, cur);
    }

    /* return */
    if (match_kw(cur, "return")) {
        int w = compile_expr(code, at, cur);
        if (w < 0) return -1;
        at += w;
        if (!match(cur, ";")) return -1;
        code[at++] = enc_mov_sp_fp();
        code[at++] = enc_pop_fp_pc();
        return at - start;
    }

    /* if */
    if (match_kw(cur, "if")) {
        return compile_if(code, at, cur);
    }

    /* while (cond) stmt
     *
     *   L_top:
     *     compile cond  -> r0
     *     cmp r0, #0
     *     beq L_end           <- placeholder, patched after body
     *     compile body
     *     b L_top             <- backward branch, offset known now
     *   L_end:
     */
    if (match_kw(cur, "while")) {
        if (!match(cur, "(")) return -1;
        int top_at = at;
        int w = compile_expr(code, at, cur);
        if (w < 0) return -1;
        at += w;
        if (!match(cur, ")")) return -1;
        code[at++] = enc_cmp_r0_imm0();
        int beq_at = at;
        code[at++] = enc_beq_placeholder();
        w = compile_stmt(code, at, cur);
        if (w < 0) return -1;
        at += w;
        /* Emit backward B to L_top, then patch the placeholder at b_at. */
        int b_at = at;
        code[at++] = enc_b_placeholder();
        patch_branch(code, b_at, top_at);
        patch_branch(code, beq_at, at);
        return at - start;
    }

    /* int decl */
    if (match_kw(cur, "int")) {
        const char *name; int nlen;
        if (!parse_ident(cur, &name, &nlen)) return -1;
        int slot = alloc_local(name, nlen);
        if (slot < 0) return -1;
        if (!match(cur, "=")) return -1;
        int w = compile_expr(code, at, cur);
        if (w < 0) return -1;
        at += w;
        if (!match(cur, ";")) return -1;
        code[at++] = enc_str_r0_fp_neg((slot + 1) * 4);
        return at - start;
    }

    /* assign or expr-stmt — both start with an identifier or literal.
     * Peek for "NAME =" specifically; everything else is a plain expr. */
    const char *save_cur = *cur;
    const char *name; int nlen;
    if (parse_ident(cur, &name, &nlen)) {
        skip_ws(cur);
        if (**cur == '=' && (*cur)[1] != '=') {
            (*cur)++;                            /* eat '=' */
            int slot = find_local(name, nlen);
            if (slot < 0) return -1;
            int w = compile_expr(code, at, cur);
            if (w < 0) return -1;
            at += w;
            if (!match(cur, ";")) return -1;
            code[at++] = enc_str_r0_fp_neg((slot + 1) * 4);
            return at - start;
        }
        /* Not "NAME =" — rewind and parse as expression-statement.
         * That re-parses the identifier, but it's only one token. */
        *cur = save_cur;
    }

    /* expr ";" — useful for side-effect calls like print_int(x); */
    int w = compile_expr(code, at, cur);
    if (w < 0) return -1;
    at += w;
    if (!match(cur, ";")) return -1;
    return at - start;
}

int cc_mvp_compile_and_run(const char *src, long *retval, int *codesize)
{
    const char *p = src;
    if (!match_kw(&p, "int"))  return -1;
    if (!match_kw(&p, "main")) return -1;
    if (!match(&p, "("))       return -1;
    if (!match(&p, ")"))       return -1;

    unsigned int *code = (unsigned int *)memget(4096);
    if (code == SYSERR_PTR || code == NULL) return -2;

    reset_locals();

    /* Prologue: save fp+lr, install frame pointer, reserve local-int
     * area.  We don't know N (number of locals) yet — the body's
     * `int x = ...;` calls allocate them as we go.  Reserve a fixed
     * MAX_LOCALS*4 = 64 byte frame; unused slots are just dead space.
     * That keeps SP alignment trivial and avoids a forward-patch. */
    int at = 0;
    code[at++] = enc_push_lr_fp();
    code[at++] = enc_mov_fp_sp();
    code[at++] = enc_sub_sp_imm(MAX_LOCALS * 4);

    /* Body is a single block.  Statements inside may include `return`,
     * which emits its own epilogue (mov sp,fp ; pop {fp,pc}).  If the
     * body falls off the end without returning we append a default
     * `return 0` epilogue below. */
    int w = compile_block(code, at, &p);
    if (w < 0) { memfree(code, 4096); return -1; }
    at += w;

    /* Default `return 0` for fall-through. */
    code[at++] = enc_mov_imm8(0, 0);
    code[at++] = enc_mov_sp_fp();
    code[at++] = enc_pop_fp_pc();

    *codesize = at * 4;

    /* Now that the I-cache is enabled (arm-rpi3 mmu.c), freshly-written JIT
     * instructions may be stale in the I-cache.  Invalidate it before
     * executing (D-cache is off so the writes are already in RAM). */
    asm volatile (
        "dsb\n"
        "mov r0, #0\n"
        "mcr p15, 0, r0, c7, c5, 0\n"   /* ICIALLU */
        "dsb\n"
        "isb\n"
        ::: "r0", "memory");

    long (*entry)(void) = (long (*)(void))code;
    *retval = entry();
    /* Execution complete — entry() returned, no live use of `code` remains.
     * Free the 4 KB JIT buffer so per-call cost is bounded.  Without this
     * every compile leaks 4 KB; load-balancer workers blow through tens
     * of MB in a stress test. */
    memfree(code, 4096);
    return 0;
}
