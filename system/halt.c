/**
 * @file halt.c
 *
 * Implementation of system_halt() — the single entry point used by
 * shell/xsh_halt.c and apps/wm.c.  See halt.h for behaviour.
 */

#include <stddef.h>
#include <halt.h>
#include <stdio.h>
#include <thread.h>

#if defined(_XINU_PLATFORM_ARM_QEMU_)
/* ARM semihosting SYS_EXIT (operation 0x18) with
 * ADP_Stopped_ApplicationExit (0x20026).  When QEMU is launched
 * with -semihosting, this terminates the emulator process and so
 * also closes the Cocoa LCD window. */
static void semihost_exit(void)
{
    register unsigned long r0 asm("r0") = 0x18;
    register unsigned long r1 asm("r1") = 0x20026;
    asm volatile ("svc 0x123456" : : "r"(r0), "r"(r1) : "memory");
}
#endif

static void halt_forever(void)
{
    asm volatile ("cpsid if" ::: "memory");
    for (;;)
    {
        asm volatile ("wfi");
    }
}

void system_halt(void)
{
    kprintf("\r\nSystem halted.\r\n");

    /* Let the UART drain before we stop scheduling.  100 ms is
     * generous at 115200 baud. */
    sleep(100);

#if defined(_XINU_PLATFORM_ARM_QEMU_)
    semihost_exit();
    /* If semihosting was not enabled the SVC falls through. */
#endif

    halt_forever();
}
