/**
 * @file cc_mvp.c
 *
 * Minimum-viable C JIT for arm-rpi3: parses
 *     int main() { return <int-literal>; }
 * and emits ARM32 machine code that, when called as a function,
 * returns the literal.  Stepping stone toward the full xinu-rpi4
 * C-subset JIT (cc/cc.c, ~2 500 lines of AArch64).
 *
 * Why so minimal: Pi 3 already has the AIPL runtime (apps/abcl_*) for
 * dynamic actor behaviour; the JIT here is for the *other* xinu-rpi4
 * niche — quick "compile + run this snippet" smoke-tests over HTTP.
 * Future commits will grow the parser (binops, locals, function calls)
 * and the codegen (call ABI, frame setup, branches).
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>

/* ARM32 instruction encodings used by the MVP.
 *   mov r0, #imm   — 0xE3A0_0000 | (imm & 0xFF), valid for imm in 0..255.
 *   movw r0, #imm  — 0xE300_0000 | (imm & 0xFFF) | ((imm & 0xF000) << 4),
 *                    valid for any 16-bit imm.  Used for larger constants.
 *   movt r0, #imm  — 0xE340_0000 | (imm & 0xFFF) | ((imm & 0xF000) << 4),
 *                    sets the top half of r0 without clearing the low half.
 *   bx lr          — 0xE12F_FF1E.
 */
static unsigned int enc_mov_imm8(int imm) { return 0xE3A00000u | (imm & 0xFF); }
static unsigned int enc_movw(int imm)     { return 0xE3000000u | ((imm & 0xF000) << 4) | (imm & 0xFFF); }
static unsigned int enc_movt(int imm)     { return 0xE3400000u | ((imm & 0xF000) << 4) | (imm & 0xFFF); }
static unsigned int enc_bx_lr(void)       { return 0xE12FFF1Eu; }

/* Strip leading whitespace + skip C-style /* ... */ /* and // ... */
/* line comments.  Bumps *cur in place. */
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

/* Match a literal keyword/punctuation, returning 1 if matched (cursor advanced)
 * or 0 if not (cursor unchanged). */
static int match(const char **cur, const char *lit)
{
    skip_ws(cur);
    const char *p = *cur;
    int i;
    for (i = 0; lit[i]; i++) {
        if (p[i] != lit[i]) return 0;
    }
    *cur = p + i;
    return 1;
}

/* Parse a decimal integer literal (positive or negative), returning the value
 * and advancing the cursor.  Sets *ok to 1 on success, 0 if no digits found. */
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

/* Compile-and-call the trivial program "int main() { return N; }".
 * On success, fills *retval with the called function's return value
 * (i.e. N) and *codesize with the number of machine bytes emitted, and
 * returns 0.  On parse error returns -1.  On allocation error returns -2.
 *
 * The emitted code buffer is heap-allocated and intentionally NOT freed —
 * caller owns it.  Pi 3 Xinu boots with the MMU off so all of DRAM is
 * RWX; no permission flip is needed before jumping to the buffer. */
int cc_mvp_compile_and_run(const char *src, long *retval, int *codesize)
{
    const char *p = src;

    if (!match(&p, "int"))    return -1;
    if (!match(&p, "main"))   return -1;
    if (!match(&p, "("))      return -1;
    if (!match(&p, ")"))      return -1;
    if (!match(&p, "{"))      return -1;
    if (!match(&p, "return")) return -1;

    int ok = 0;
    int n  = parse_int(&p, &ok);
    if (!ok) return -1;

    if (!match(&p, ";")) return -1;
    if (!match(&p, "}")) return -1;

    /* Emit:
     *   movw r0, #(n & 0xFFFF)
     *   movt r0, #((n >> 16) & 0xFFFF)   (only if needed)
     *   bx lr
     * 3 instructions = 12 bytes max.  4 KB allocation gives us headroom
     * for the parser growth that's coming. */
    unsigned int *code = (unsigned int *)memget(4096);
    if (NULL == code) return -2;

    int i = 0;
    if (n >= 0 && n <= 255) {
        code[i++] = enc_mov_imm8(n);
    } else {
        code[i++] = enc_movw(n & 0xFFFF);
        if (((unsigned int)n) >> 16) {
            code[i++] = enc_movt(((unsigned int)n >> 16) & 0xFFFF);
        }
    }
    code[i++] = enc_bx_lr();
    *codesize = i * 4;

    /* On ARMv7 we must flush the data cache and invalidate the I-cache
     * before executing newly-written instructions.  Pi 3 Xinu runs with
     * caches OFF (per system/platforms/arm-rpi3/platformVars comments),
     * so this is a no-op in this build — but leave the room for it. */

    long (*entry)(void) = (long (*)(void))code;
    *retval = entry();
    return 0;
}
