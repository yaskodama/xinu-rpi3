/**
 * @file halt.h
 *
 * System-wide halt entry point.  Shared by:
 *   - shell/xsh_halt.c — `halt` typed at the console xsh prompt
 *   - apps/wm.c         — `halt` typed in the WM mini-shell input box,
 *                         or the "[Halt]" button click in the topbar
 *
 * On arm-qemu: invokes the ARM semihosting SYS_EXIT (Angel ABI) so
 * the QEMU process — including the Cocoa LCD window — terminates
 * cleanly.  Requires QEMU to be launched with -semihosting.
 *
 * On arm-rpi: masks IRQ/FIQ in CPSR and parks the core in WFI forever.
 *
 * Never returns.
 */
#ifndef _HALT_H_
#define _HALT_H_

void system_halt(void);

#endif /* _HALT_H_ */
