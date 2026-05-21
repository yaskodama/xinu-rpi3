/**
 * @file measured_boot.c
 *
 * Sec3 MeasuredBoot (Xinu_KernelEvolution_Round1.aice).
 *
 * At kernel boot, fold every byte of the kernel `.text + .rodata`
 * range (linker symbols _stext .. _etext) into a 64-bit FNV-1a hash
 * and print it on the kernel console (UART0).  The hash is a
 * single deterministic identifier for *this* compiled image — any
 * single-byte tampering (post-compile patch, RAM corruption, ROM
 * sun-rot) flips many bits of the output.
 *
 *   Future Sec3+ work:  swap FNV-1a for SHA-256 once Sec2 cap
 *   tokens land — caps need a strong PRF anyway, so the SHA core
 *   will already be linked in.  For now FNV-1a is enough to
 *   demonstrate the boot-time integrity check and is < 30 LOC
 *   instead of ~250.
 *
 *   Future Sec3+ work:  compare the printed hash against an
 *   expected value baked in at build time (a constant the build
 *   system writes after `objcopy --dump-section .text=...`),
 *   refusing to continue on mismatch.  The current implementation
 *   only logs — it is a measurement, not yet an enforcement.
 */
/* Embedded Xinu — S1 Round 1 measured boot. */

#include <kernel.h>

extern char _stext[], _etext[];

#define FNV_OFFSET_BASIS_64  0xCBF29CE484222325ULL
#define FNV_PRIME_64         0x00000100000001B3ULL

static unsigned long long fnv1a64(const unsigned char *p,
                                  const unsigned char *end)
{
    unsigned long long h = FNV_OFFSET_BASIS_64;
    while (p < end) {
        h ^= (unsigned long long)(*p++);
        h *= FNV_PRIME_64;
    }
    return h;
}

void measured_boot_print(void)
{
    extern int kprintf(const char *, ...);
    unsigned long long h;
    unsigned long len;

    len = (unsigned long)(_etext - _stext);
    h   = fnv1a64((const unsigned char *)_stext,
                  (const unsigned char *)_etext);
    kprintf("[Sec3] measured-boot stext=0x%08lx etext=0x%08lx "
            "len=%lu fnv1a64=0x%08x%08x\r\n",
            (unsigned long)_stext, (unsigned long)_etext, len,
            (unsigned)(h >> 32), (unsigned)(h & 0xFFFFFFFFu));
}
