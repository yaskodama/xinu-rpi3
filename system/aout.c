/**
 * @file aout.c
 *
 * Loader and stack VM for a.out bytecode programs produced by cc.c.
 *
 * The VM holds:
 *   - locals[]   : int32_t array sized by hdr.nlocals (fresh per call)
 *   - stack[]    : int32_t evaluation stack (256 entries)
 *   - consts[]   : raw byte buffer; string pointers passed through builtins
 *                  reference into this buffer
 *
 * Strings on the VM stack are represented as offsets into consts[].
 * Builtins (printf/puts/...) interpret string-typed arguments accordingly;
 * since the VM has no static types, this is a convention enforced by the
 * compiler (PUSH_STR vs PUSH_I32).
 */

#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <thread.h>
#include <xfs.h>
#include <aout.h>

/* Provided by apps/wm.c (only on platforms with the WM linked in). */
extern void wm_user_set_line(int idx, int x1, int y1, int x2, int y2,
                             unsigned short color);
extern void wm_user_clear_lines(void);
extern void wm_user_render_enable(int on);

/* Bhaskara approximation of sin in Q12 (max 4096). */
static int q12_sin(int deg)
{
    int t, num, den;
    deg = ((deg % 360) + 360) % 360;
    if (deg <= 180)
    {
        t   = deg;
        num = 16384 * t * (180 - t);
        den = 40500 - t * (180 - t);
        return num / den;
    }
    else
    {
        t   = deg - 180;
        num = 16384 * t * (180 - t);
        den = 40500 - t * (180 - t);
        return -(num / den);
    }
}

#define VM_STACK_DEPTH 256
#define VM_LOCALS_MAX  512

/* Read little-endian multi-byte values via memcpy of individual bytes.
 * GCC at -Os will otherwise recognize the byte-shift pattern and emit a
 * single LDRH/LDR which faults on unaligned addresses on ARMv6 (e.g. when
 * a uint16_t operand follows a 1-byte opcode at an odd offset). */
static int read_i32(const uint8_t *p)
{
    uint8_t b0, b1, b2, b3;
    memcpy(&b0, p + 0, 1);
    memcpy(&b1, p + 1, 1);
    memcpy(&b2, p + 2, 1);
    memcpy(&b3, p + 3, 1);
    return (int)((uint32_t)b0 |
                 ((uint32_t)b1 << 8) |
                 ((uint32_t)b2 << 16) |
                 ((uint32_t)b3 << 24));
}
static uint32_t read_u32(const uint8_t *p)
{
    uint8_t b0, b1, b2, b3;
    memcpy(&b0, p + 0, 1);
    memcpy(&b1, p + 1, 1);
    memcpy(&b2, p + 2, 1);
    memcpy(&b3, p + 3, 1);
    return (uint32_t)b0 |
           ((uint32_t)b1 << 8) |
           ((uint32_t)b2 << 16) |
           ((uint32_t)b3 << 24);
}
static uint16_t read_u16(const uint8_t *p)
{
    uint8_t b0, b1;
    memcpy(&b0, p + 0, 1);
    memcpy(&b1, p + 1, 1);
    return (uint16_t)((uint16_t)b0 | ((uint16_t)b1 << 8));
}

static const char *str_at(const uint8_t *consts, uint32_t const_size,
                          uint32_t off)
{
    if (off >= const_size) return "";
    return (const char *)(consts + off);
}

static void bi_printf(int32_t *stack, int *sp_io, int nargs,
                      const uint8_t *consts, uint32_t const_size)
{
    int sp = *sp_io;
    int32_t *args = &stack[sp - nargs];
    const char *fmt;
    int ai = 1;
    char numbuf[16];

    if (nargs < 1) { *sp_io = sp - nargs; if (sp - nargs >= 0) stack[sp - nargs] = 0; *sp_io = sp - nargs + 1; return; }
    fmt = str_at(consts, const_size, (uint32_t)args[0]);

    for (; *fmt; fmt++)
    {
        if (*fmt != '%') { putchar(*fmt); continue; }
        fmt++;
        switch (*fmt)
        {
        case 'd':
        {
            int v = (ai < nargs) ? args[ai++] : 0;
            sprintf(numbuf, "%d", v);
            { const char *q = numbuf; while (*q) putchar(*q++); }
            break;
        }
        case 's':
        {
            uint32_t off = (ai < nargs) ? (uint32_t)args[ai++] : 0;
            const char *q = str_at(consts, const_size, off);
            while (*q) putchar(*q++);
            break;
        }
        case 'c':
        {
            int v = (ai < nargs) ? args[ai++] : 0;
            putchar(v);
            break;
        }
        case '%': putchar('%'); break;
        case 0:   fmt--;        break;
        default:  putchar('%'); putchar(*fmt); break;
        }
    }

    sp -= nargs;
    stack[sp++] = 0;
    *sp_io = sp;
}

static int run(const uint8_t *code, uint32_t code_size,
               const uint8_t *consts, uint32_t const_size,
               uint32_t entry, uint32_t nlocals)
{
    int32_t stack[VM_STACK_DEPTH];
    int32_t locals[VM_LOCALS_MAX];
    int sp = 0;
    uint32_t pc = entry;
    int retval = 0;
    int i;
    int32_t a, b;
    uint8_t op;

    for (i = 0; i < (int)nlocals && i < VM_LOCALS_MAX; i++) locals[i] = 0;

    while (pc < code_size)
    {
        op = code[pc++];
        switch (op)
        {
        case OP_HALT: goto done;

        case OP_PUSH_I32:
            if (pc + 4 > code_size) goto fault;
            stack[sp++] = read_i32(code + pc);
            pc += 4;
            break;

        case OP_PUSH_STR:
            if (pc + 4 > code_size) goto fault;
            stack[sp++] = (int32_t)read_u32(code + pc);
            pc += 4;
            break;

        case OP_POP: if (sp <= 0) goto fault; sp--; break;
        case OP_DUP: if (sp <= 0 || sp >= VM_STACK_DEPTH) goto fault;
                     stack[sp] = stack[sp-1]; sp++; break;

        case OP_LOAD_LOC:
            if (pc + 2 > code_size) goto fault;
            { uint16_t idx = read_u16(code + pc); pc += 2;
              if (idx >= nlocals) goto fault;
              stack[sp++] = locals[idx]; }
            break;

        case OP_STORE_LOC:
            if (pc + 2 > code_size || sp <= 0) goto fault;
            { uint16_t idx = read_u16(code + pc); pc += 2;
              if (idx >= nlocals) goto fault;
              locals[idx] = stack[--sp]; }
            break;

        case OP_ADD: if (sp < 2) goto fault; sp--; stack[sp-1] = stack[sp-1] + stack[sp]; break;
        case OP_SUB: if (sp < 2) goto fault; sp--; stack[sp-1] = stack[sp-1] - stack[sp]; break;
        case OP_MUL: if (sp < 2) goto fault; sp--; stack[sp-1] = stack[sp-1] * stack[sp]; break;
        case OP_DIV: if (sp < 2) goto fault; sp--; if (stack[sp]==0) { stack[sp-1]=0; break; } stack[sp-1] = stack[sp-1] / stack[sp]; break;
        case OP_MOD: if (sp < 2) goto fault; sp--; if (stack[sp]==0) { stack[sp-1]=0; break; } stack[sp-1] = stack[sp-1] % stack[sp]; break;
        case OP_NEG: if (sp < 1) goto fault; stack[sp-1] = -stack[sp-1]; break;

        case OP_EQ: if (sp < 2) goto fault; sp--; stack[sp-1] = (stack[sp-1] == stack[sp]); break;
        case OP_NE: if (sp < 2) goto fault; sp--; stack[sp-1] = (stack[sp-1] != stack[sp]); break;
        case OP_LT: if (sp < 2) goto fault; sp--; stack[sp-1] = (stack[sp-1] <  stack[sp]); break;
        case OP_LE: if (sp < 2) goto fault; sp--; stack[sp-1] = (stack[sp-1] <= stack[sp]); break;
        case OP_GT: if (sp < 2) goto fault; sp--; stack[sp-1] = (stack[sp-1] >  stack[sp]); break;
        case OP_GE: if (sp < 2) goto fault; sp--; stack[sp-1] = (stack[sp-1] >= stack[sp]); break;
        case OP_NOT: if (sp < 1) goto fault; stack[sp-1] = !stack[sp-1]; break;
        case OP_LAND: if (sp < 2) goto fault; sp--; a = stack[sp-1]; b = stack[sp]; stack[sp-1] = (a && b); break;
        case OP_LOR:  if (sp < 2) goto fault; sp--; a = stack[sp-1]; b = stack[sp]; stack[sp-1] = (a || b); break;

        case OP_JMP:
            if (pc + 4 > code_size) goto fault;
            { int32_t off = read_i32(code + pc); pc += 4; pc = (uint32_t)((int32_t)pc + off); }
            break;

        case OP_JZ:
            if (pc + 4 > code_size || sp < 1) goto fault;
            { int32_t off = read_i32(code + pc); pc += 4; sp--;
              if (stack[sp] == 0) pc = (uint32_t)((int32_t)pc + off); }
            break;

        case OP_JNZ:
            if (pc + 4 > code_size || sp < 1) goto fault;
            { int32_t off = read_i32(code + pc); pc += 4; sp--;
              if (stack[sp] != 0) pc = (uint32_t)((int32_t)pc + off); }
            break;

        case OP_CALL_BI:
            if (pc + 2 > code_size) goto fault;
            {
                uint8_t bi    = code[pc++];
                uint8_t na    = code[pc++];
                if (sp < (int)na) goto fault;
                switch (bi)
                {
                case BI_PRINTF:
                    bi_printf(stack, &sp, na, consts, const_size);
                    break;
                case BI_PUTS:
                {
                    if (na >= 1)
                    {
                        const char *q = str_at(consts, const_size,
                                               (uint32_t)stack[sp - na]);
                        while (*q) putchar(*q++);
                        putchar('\n');
                    }
                    sp -= na; stack[sp++] = 0;
                    break;
                }
                case BI_PUTCHAR:
                {
                    int v = (na >= 1) ? stack[sp - na] : 0;
                    putchar(v);
                    sp -= na; stack[sp++] = v;
                    break;
                }
                case BI_GETCHAR:
                    sp -= na;
                    stack[sp++] = -1;       /* not implemented */
                    break;
                case BI_EXIT:
                {
                    retval = (na >= 1) ? stack[sp - na] : 0;
                    goto done;
                }
                case BI_RGB:
                {
                    int r = (na >= 1) ? stack[sp - na]     : 0;
                    int gv= (na >= 2) ? stack[sp - na + 1] : 0;
                    int bv= (na >= 3) ? stack[sp - na + 2] : 0;
                    int c = (((bv >> 3) & 0x1F) << 11) |
                            (((gv >> 2) & 0x3F) <<  5) |
                             ((r  >> 3) & 0x1F);
                    sp -= na; stack[sp++] = c;
                    break;
                }
                case BI_WM_LINE:
                {
                    int idx = (na >= 1) ? stack[sp - na    ] : 0;
                    int x1  = (na >= 2) ? stack[sp - na + 1] : 0;
                    int y1  = (na >= 3) ? stack[sp - na + 2] : 0;
                    int x2  = (na >= 4) ? stack[sp - na + 3] : 0;
                    int y2  = (na >= 5) ? stack[sp - na + 4] : 0;
                    int c   = (na >= 6) ? stack[sp - na + 5] : 0xFFFF;
                    wm_user_set_line(idx, x1, y1, x2, y2,
                                     (unsigned short)c);
                    sp -= na; stack[sp++] = 0;
                    break;
                }
                case BI_WM_RENDER:
                {
                    int on = (na >= 1) ? stack[sp - na] : 0;
                    wm_user_render_enable(on);
                    sp -= na; stack[sp++] = 0;
                    break;
                }
                case BI_WM_CLEAR:
                {
                    wm_user_clear_lines();
                    sp -= na; stack[sp++] = 0;
                    break;
                }
                case BI_SLEEP_MS:
                {
                    int ms = (na >= 1) ? stack[sp - na] : 0;
                    if (ms > 0) sleep((unsigned)ms);
                    sp -= na; stack[sp++] = 0;
                    break;
                }
                case BI_ISIN:
                {
                    int d = (na >= 1) ? stack[sp - na] : 0;
                    sp -= na; stack[sp++] = q12_sin(d);
                    break;
                }
                case BI_ICOS:
                {
                    int d = (na >= 1) ? stack[sp - na] : 0;
                    sp -= na; stack[sp++] = q12_sin(d + 90);
                    break;
                }
                case BI_SCREEN_W:
                    sp -= na; stack[sp++] = 1024;
                    break;
                case BI_SCREEN_H:
                    sp -= na; stack[sp++] = 768;
                    break;
                default:
                    goto fault;
                }
            }
            break;

        case OP_RET:
            if (sp < 1) goto fault;
            retval = stack[--sp];
            goto done;

        case OP_ENTER:
            if (pc + 2 > code_size) goto fault;
            { uint16_t n = read_u16(code + pc); pc += 2;
              for (i = 0; i < (int)n && i < VM_LOCALS_MAX; i++) locals[i] = 0; }
            break;

        default:
            goto fault;
        }
        if (sp < 0 || sp >= VM_STACK_DEPTH) goto fault;
    }
done:
    return retval;
fault:
    printf("a.out: VM fault at pc=0x%x\n", pc - 1);
    return -1;
}

int aoutRun(const char *path)
{
    int fd, n;
    struct aout_header hdr;
    uint8_t *code = NULL, *consts = NULL;
    int rc;

    fd = xfsOpen(path, XFS_O_RDONLY);
    if (fd < 0) { return SYSERR; }
    n = xfsRead(fd, &hdr, sizeof(hdr));
    if (n != (int)sizeof(hdr)) { xfsClose(fd); return SYSERR; }
    if (hdr.magic[0] != 'X' || hdr.magic[1] != 'A' ||
        hdr.magic[2] != 'O' || hdr.magic[3] != 'U' ||
        hdr.version  != AOUT_VERSION)
    { xfsClose(fd); return SYSERR; }

    if (hdr.code_size > 0)
    {
        code = (uint8_t *)memget(hdr.code_size);
        if ((void *)SYSERR == code) { xfsClose(fd); return SYSERR; }
        if (xfsRead(fd, code, hdr.code_size) != (int)hdr.code_size)
        { memfree(code, hdr.code_size); xfsClose(fd); return SYSERR; }
    }
    if (hdr.const_size > 0)
    {
        consts = (uint8_t *)memget(hdr.const_size);
        if ((void *)SYSERR == consts)
        { if (code) memfree(code, hdr.code_size); xfsClose(fd); return SYSERR; }
        if (xfsRead(fd, consts, hdr.const_size) != (int)hdr.const_size)
        { memfree(consts, hdr.const_size);
          if (code) memfree(code, hdr.code_size);
          xfsClose(fd); return SYSERR; }
    }
    xfsClose(fd);

    rc = run(code, hdr.code_size, consts, hdr.const_size,
             hdr.entry, hdr.nlocals);

    if (code)   memfree(code,   hdr.code_size);
    if (consts) memfree(consts, hdr.const_size);
    return rc;
}
