/**
 * @file cc_mvp.c
 *
 * C JIT for arm-rpi3, MVP-stage 3: function calls + builtin symbols.
 *
 * Grammar:
 *   program  = "int" "main" "(" ")" "{" "return" expr ";" "}"
 *   expr     = primary ( ("+" | "-") primary )*
 *   primary  = INT_LITERAL
 *            | NAME "(" args? ")"           // function call
 *   args     = expr ( "," expr )*           // 0..4 args
 *   NAME     = [_A-Za-z][_A-Za-z0-9]*
 *
 * Compiles to ARM32 (Cortex-A53 AArch32 mode), emits via memget()
 * buffer, calls in place.  AAPCS32 calling convention: args in
 * r0..r3, return in r0, ip (r12) for the call target so we can BLX
 * an arbitrary absolute address.
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
 * its length (via *out_len with *out_start the original start).
 * Returns 1 on success, 0 if no identifier present. */
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

/* Emit code that puts a primary into r0.  Returns words written, or -1
 * on parse error. */
static int compile_primary(unsigned int *code, int at, const char **cur)
{
    skip_ws(cur);
    const char *save = *cur;

    /* Try identifier-style: NAME ( args ) */
    const char *name; int nlen;
    if (parse_ident(cur, &name, &nlen)) {
        skip_ws(cur);
        if (**cur == '(') {
            (*cur)++;
            /* Parse comma-separated args.  For up to 4 args, we evaluate
             * each into r0 and push, then pop into r0..r3 (in argN..arg0
             * order).  We also save r0 from outer context by pushing
             * before any arg work — caller's r0 is on top of stack so
             * arg eval can clobber freely. */
            int n_args = 0;
            int  arg_pushes_at = at;  /* not used yet */
            (void)arg_pushes_at;

            skip_ws(cur);
            if (**cur != ')') {
                /* parse first arg */
                int w = compile_expr(code, at, cur);
                if (w < 0) return -1;
                at += w;
                code[at++] = enc_str_push_r0();        /* save arg0 */
                n_args = 1;
                while (1) {
                    skip_ws(cur);
                    if (**cur != ',') break;
                    (*cur)++;
                    if (n_args >= 4) return -1;        /* > 4 args unsupported */
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

            /* Pop args in reverse order into rN, r(N-1), ..., r0 */
            int i;
            for (i = n_args - 1; i >= 0; i--) {
                code[at++] = enc_ldr_pop(i);
            }

            /* Resolve target — must be a known builtin. */
            void *fn = lookup_sym(name, nlen);
            if (NULL == fn) return -1;
            unsigned int addr = (unsigned int)(unsigned long)fn;

            /* mov ip, #low ; movt ip, #high ; blx ip — result in r0 */
            code[at++] = enc_movw(12, addr & 0xFFFF);
            code[at++] = enc_movt(12, (addr >> 16) & 0xFFFF);
            code[at++] = enc_blx_ip();
            return at - (int)(at - (at - 0));  /* dummy: return at-orig */
        }
        /* Not a call — rewind and try int literal next. */
        *cur = save;
    }

    /* Fall back to int literal */
    int ok = 0;
    int n  = parse_int(cur, &ok);
    if (!ok) return -1;
    return emit_const(code, at, 0, n);
}

/* Wrapper that translates compile_primary's "return at" convention
 * (which I tangled above) into "return words written". */
static int compile_primary_simple(unsigned int *code, int at, const char **cur)
{
    int start = at;
    skip_ws(cur);

    /* identifier? */
    const char *save = *cur;
    const char *name; int nlen;
    if (parse_ident(cur, &name, &nlen)) {
        skip_ws(cur);
        if (**cur == '(') {
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
        *cur = save;
    }

    int ok = 0;
    int n  = parse_int(cur, &ok);
    if (!ok) return -1;
    return emit_const(code, at, 0, n);
}

static int compile_expr(unsigned int *code, int at, const char **cur)
{
    int start = at;
    int w = compile_primary_simple(code, at, cur);
    if (w < 0) return -1;
    at += w;
    while (1) {
        char op = peek(cur);
        if (op != '+' && op != '-') break;
        (*cur)++;
        code[at++] = enc_str_push_r0();
        w = compile_primary_simple(code, at, cur);
        if (w < 0) return -1;
        at += w;
        code[at++] = enc_ldr_pop(1);
        code[at++] = (op == '+') ? enc_add_r0_r1_r0() : enc_sub_r0_r1_r0();
    }
    return at - start;
}

int cc_mvp_compile_and_run(const char *src, long *retval, int *codesize)
{
    const char *p = src;
    if (!match(&p, "int"))    return -1;
    if (!match(&p, "main"))   return -1;
    if (!match(&p, "("))      return -1;
    if (!match(&p, ")"))      return -1;
    if (!match(&p, "{"))      return -1;
    if (!match(&p, "return")) return -1;

    unsigned int *code = (unsigned int *)memget(4096);
    if (code == SYSERR_PTR || code == NULL) return -2;

    /* Prologue saves fp+lr so the builtin BLX'd in the middle can
     * return to us (it overwrites lr). */
    int at = 0;
    code[at++] = enc_push_lr_fp();

    int w = compile_expr(code, at, &p);
    if (w < 0) return -1;
    at += w;

    if (!match(&p, ";")) return -1;
    if (!match(&p, "}")) return -1;

    /* Epilogue: pop fp,pc — returns through pc with original lr. */
    code[at++] = enc_pop_fp_pc();
    *codesize = at * 4;

    long (*entry)(void) = (long (*)(void))code;
    *retval = entry();
    return 0;
}
