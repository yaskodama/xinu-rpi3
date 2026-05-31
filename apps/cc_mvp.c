/**
 * @file cc_mvp.c
 *
 * C JIT for arm-rpi3, MVP-stage 2: grammar
 *   program  = "int" "main" "(" ")" "{" "return" expr ";" "}"
 *   expr     = primary ( ("+" | "-") primary )*
 *   primary  = INT_LITERAL
 *
 * Compiles to ARM32 (Cortex-A53 AArch32 mode), emits via memget()
 * buffer, calls in place.  Pi 3 boots with MMU/caches OFF so the
 * DRAM buffer is implicitly executable — no permission flip needed.
 *
 * Stack-machine codegen: every primary lands in r0; for a binop the
 * right operand is computed first into r0 and pushed (str r0,[sp,#-4]!),
 * then the left operand is computed into r0, then r1 is popped
 * (ldr r1,[sp],#4) and add/sub r0, r1, r0 produces the result.
 * (Left-associative — chained binops naturally fall out.)
 *
 * Future steps: * /, parens, locals, if/else, function calls.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>

#define SYSERR_PTR ((void *)(-1))    /* memget() returns this on failure */

/* ARM32 instruction encoders.  Each emit_*() returns the 32-bit word. */
static unsigned int enc_mov_imm8(int imm) { return 0xE3A00000u | (imm & 0xFF); }
static unsigned int enc_movw(int rd, int imm)
{
    return 0xE3000000u | ((rd & 0xF) << 12) | ((imm & 0xF000) << 4) | (imm & 0xFFF);
}
static unsigned int enc_movt(int rd, int imm)
{
    return 0xE3400000u | ((rd & 0xF) << 12) | ((imm & 0xF000) << 4) | (imm & 0xFFF);
}
static unsigned int enc_bx_lr(void)        { return 0xE12FFF1Eu; }
static unsigned int enc_str_push_r0(void)  { return 0xE52D0004u; }  /* str r0,[sp,#-4]! */
static unsigned int enc_ldr_pop_r1(void)   { return 0xE49D1004u; }  /* ldr r1,[sp],#4   */
static unsigned int enc_add_r0_r1_r0(void) { return 0xE0810000u; }  /* add r0,r1,r0 */
static unsigned int enc_sub_r0_r1_r0(void) { return 0xE0410000u; }  /* sub r0,r1,r0 */

/* Skip whitespace and both C comment styles ('// to EOL' and block). */
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

/* Match a literal keyword/punctuation; advance cursor on success. */
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

/* Peek the next non-whitespace char without advancing. */
static char peek(const char **cur)
{
    skip_ws(cur);
    return **cur;
}

/* Parse a decimal int literal.  Sets *ok=1 on success. */
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

/* Emit "mov r0, #N" using the shortest ARM32 sequence available.
 * Returns number of 32-bit words written. */
static int emit_const_r0(unsigned int *code, int at, int n)
{
    if (n >= 0 && n <= 255) {
        code[at] = enc_mov_imm8(n);
        return 1;
    }
    code[at] = enc_movw(0, n & 0xFFFF);
    if (((unsigned int)n) >> 16) {
        code[at + 1] = enc_movt(0, ((unsigned int)n >> 16) & 0xFFFF);
        return 2;
    }
    return 1;
}

/* Emit code that puts a primary (just an int literal for now) into r0.
 * Returns words emitted, or -1 on parse error. */
static int compile_primary(unsigned int *code, int at, const char **cur)
{
    int ok = 0;
    int n  = parse_int(cur, &ok);
    if (!ok) return -1;
    return emit_const_r0(code, at, n);
}

/* Emit code that puts an expression result into r0.  Returns words
 * emitted, or -1 on parse error. */
static int compile_expr(unsigned int *code, int at, const char **cur)
{
    int w = compile_primary(code, at, cur);
    if (w < 0) return -1;
    at += w;

    while (1) {
        char op = peek(cur);
        if (op != '+' && op != '-') break;
        (*cur)++;
        /* Push left operand (currently in r0), compile right primary
         * into r0, pop into r1, then add/sub r0, r1, r0. */
        code[at++] = enc_str_push_r0();
        w = compile_primary(code, at, cur);
        if (w < 0) return -1;
        at += w;
        code[at++] = enc_ldr_pop_r1();
        code[at++] = (op == '+') ? enc_add_r0_r1_r0() : enc_sub_r0_r1_r0();
    }
    return at;
}

/* Compile and run the program.  Returns 0 on success and fills *retval
 * with what main() returned and *codesize with bytes emitted.  Returns
 * -1 on parse error, -2 on allocation failure. */
int cc_mvp_compile_and_run(const char *src, long *retval, int *codesize)
{
    const char *p = src;

    if (!match(&p, "int"))    return -1;
    if (!match(&p, "main"))   return -1;
    if (!match(&p, "("))      return -1;
    if (!match(&p, ")"))      return -1;
    if (!match(&p, "{"))      return -1;
    if (!match(&p, "return")) return -1;

    /* Generous initial buffer — re-tested with multi-binop expressions. */
    unsigned int *code = (unsigned int *)memget(4096);
    if (code == SYSERR_PTR || code == NULL) return -2;

    int n_words = compile_expr(code, 0, &p);
    if (n_words < 0) return -1;

    if (!match(&p, ";")) return -1;
    if (!match(&p, "}")) return -1;

    code[n_words++] = enc_bx_lr();
    *codesize = n_words * 4;

    long (*entry)(void) = (long (*)(void))code;
    *retval = entry();
    return 0;
}
