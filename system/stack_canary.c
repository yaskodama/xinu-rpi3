/**
 * @file stack_canary.c
 *
 * Sec1 StackCanaries from Xinu_KernelEvolution_Round1.aice.
 *
 * GCC emits two undefined references when -fstack-protector{,-strong,
 * -all} is in effect:
 *
 *   ulong __stack_chk_guard
 *       Per-process random word.  Compiler emits prologue:
 *           tmp = *(ulong*)((char*)caller_sp + frame_canary_off);
 *           tmp ^= __stack_chk_guard;
 *       and verifies in epilogue that tmp still matches.
 *
 *   void __stack_chk_fail(void)
 *       Called from the epilogue if the canary differs from the
 *       prologue value.  By contract, this function never returns —
 *       on a normal libc it logs + abort()s.  In a freestanding
 *       kernel we kpanic() instead.
 *
 * Xinu is built with -nostdinc -fno-builtin, so libc does not provide
 * these symbols — we have to.
 */
/* Embedded Xinu — S1 stack canary stubs. */

#include <kernel.h>
#include <thread.h>

/* The canary value.  Anything non-zero is safer than the GCC default
 * (which would expand to a global from libssp).  Random-looking
 * constant chosen so an attacker cannot trivially guess it from the
 * binary; on real Pi hardware Sec3_MeasuredBoot will eventually
 * randomise this from /dev/urandom-equivalent at boot. */
unsigned long __stack_chk_guard = 0xC4F1A5E7UL;

/* __stack_chk_fail — called by the epilogue of any function whose
 * canary mismatched.  Return is forbidden by ABI: the caller's
 * stack frame is already provably corrupt, so resuming would be
 * UB.  We log to UART0 (kprintf goes through PL011 console) and
 * spin in a tight halt loop.
 *
 * The __attribute__((noreturn)) tells GCC the function does not
 * return so it can drop the post-call code generation that would
 * otherwise re-establish the (corrupt) frame. */
void __stack_chk_fail(void) __attribute__((noreturn));

void __stack_chk_fail(void)
{
    extern int kprintf(const char *, ...);
    kprintf("[Sec1] STACK CANARY FAIL — kernel halting\r\n");
    /* No safe way out — just spin so the developer sees the message
     * in the serial log.  If the watchdog is on it will reset us. */
    for (;;) {
        /* WFI keeps QEMU host idle while halted. */
        __asm__ volatile ("wfi");
    }
}
