/**
 * @file aout.h
 *
 * Tiny C-subset bytecode executable format and runtime.  Programs compiled
 * with the `cc` shell command are written to xfs as a.out files in this
 * format and executed by aoutRun().
 */

#ifndef _AOUT_H_
#define _AOUT_H_

#include <stddef.h>
#include <stdint.h>

#define AOUT_MAGIC "XAOU"
#define AOUT_VERSION 1u

struct aout_header
{
    char     magic[4];           /* "XAOU"                                  */
    uint32_t version;            /* AOUT_VERSION                            */
    uint32_t code_size;          /* bytes of code section                   */
    uint32_t const_size;         /* bytes of const-pool section             */
    uint32_t entry;              /* offset within code section to start at  */
    uint32_t nlocals;            /* main()'s local count                    */
    uint32_t reserved;
};

/* Bytecode opcodes (1 byte each, optional immediate operand follows). */
enum aout_op
{
    OP_HALT      = 0x00,
    OP_PUSH_I32  = 0x01,         /* + int32  : push immediate                */
    OP_PUSH_STR  = 0x02,         /* + uint32 : push offset into const pool   */
    OP_POP       = 0x03,
    OP_DUP       = 0x04,

    OP_LOAD_LOC  = 0x10,         /* + uint16 : push locals[idx]              */
    OP_STORE_LOC = 0x11,         /* + uint16 : pop, locals[idx] = popped     */

    OP_ADD       = 0x20,
    OP_SUB       = 0x21,
    OP_MUL       = 0x22,
    OP_DIV       = 0x23,
    OP_MOD       = 0x24,
    OP_NEG       = 0x25,

    OP_EQ        = 0x30,
    OP_NE        = 0x31,
    OP_LT        = 0x32,
    OP_LE        = 0x33,
    OP_GT        = 0x34,
    OP_GE        = 0x35,
    OP_NOT       = 0x36,
    OP_LAND      = 0x37,
    OP_LOR       = 0x38,

    OP_JMP       = 0x40,         /* + int32 (relative to next instruction)   */
    OP_JZ        = 0x41,
    OP_JNZ       = 0x42,

    OP_CALL_BI   = 0x50,         /* + uint8 builtin id, uint8 nargs          */
    OP_RET       = 0x60,
    OP_ENTER     = 0x70          /* + uint16 nlocals (zero-fill locals)      */
};

/* Builtin function IDs (recognized by name in the compiler). */
#define BI_PRINTF     0
#define BI_PUTS       1
#define BI_PUTCHAR    2
#define BI_GETCHAR    3
#define BI_EXIT       4
#define BI_RGB        5     /* rgb(r, g, b) -> packed BGR565            */
#define BI_WM_LINE    6     /* wm_line(idx, x1, y1, x2, y2, color)      */
#define BI_WM_RENDER  7     /* wm_render(on) — 1 enable, 0 disable       */
#define BI_WM_CLEAR   8     /* wm_clear()                                */
#define BI_SLEEP_MS   9     /* sleep_ms(ms)                              */
#define BI_ISIN      10     /* isin(deg) -> Q12                          */
#define BI_ICOS      11     /* icos(deg) -> Q12                          */
#define BI_SCREEN_W  12     /* screen_w() -> 1024                        */
#define BI_SCREEN_H  13     /* screen_h() -> 768                         */

/* Compile src_path into out_path.  Both are xfs paths.  Returns OK or SYSERR.
 * Writes diagnostics via printf(). */
int ccCompile(const char *src_path, const char *out_path);

/* Load and execute an a.out program from xfs.  Returns the program's exit
 * value, or SYSERR on load failure. */
int aoutRun(const char *path);

#endif /* _AOUT_H_ */
