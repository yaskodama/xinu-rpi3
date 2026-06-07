/**
 * @file start.S
 *
 * Initialization code for Embedded Xinu on the Raspberry Pi.
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */


#include <arm.h>     /* For ARM_MODE_SYS */

/* Null-thread stack.  sysinit()/usbinit() run on this stack during early
 * boot, and the USB enumeration call chain is deep; Cortex-A53 frames are
 * larger than Pi1's ARM1176, so 8 KB overflowed (crash in a bind_device
 * prologue).  128 KB is plenty given the ~195 MB heap. */
#define NULLSTK 0x20000

.section .init
	.globl _start

	/* _start:  Entry point of the Xinu kernel.  This will be the very first
	 * byte of the kernel image and on the Raspberry Pi will be loaded by
	 * the GPU at address 0x8000.  */
	.func _start
_start:
	/* Pi 2/3 32-bit firmware enters the kernel in HYP mode (PL2); the Pi1
	 * ARM1176 entered in SVC.  A later `cps #SYS` executed from HYP is
	 * UNPREDICTABLE and faults into the (unset) HYP vector → silent hang.
	 * So drop HYP->SVC right away.  Preserve r0-r2 (boot args: 0 / machine
	 * type / atags); use r3 and r12 as scratch. */
	mrs r12, cpsr
	and r3, r12, #0x1f
	cmp r3, #0x1a                /* 0x1a = HYP mode? */
	bne _in_svc
	bic r12, r12, #0x1f
	orr r12, r12, #0xd3          /* SVC mode, IRQ+FIQ masked */
	msr spsr_cxsf, r12
	adr r12, _in_svc
	msr elr_hyp, r12
	eret
_in_svc:

	/* Park secondary cores.  The Pi 3 (BCM2837) is quad-core; the GPU
	 * firmware normally releases only core 0, but guard explicitly so a
	 * stray secondary core spins in low-power WFE instead of trampling the
	 * single-core Xinu kernel.  Uses r4 as scratch to preserve r0-r2 (the
	 * boot protocol's zero / machine-type / atags registers). */
	mrc p15, 0, r4, c0, c0, 5    /* read MPIDR */
	and r4, r4, #3               /* Aff0 = CPU id 0..3 */
	cmp r4, #0
	bne park_secondary

	/* Save the pointer to the atags (ARM boot tags).  The bootloader should
	 * pass this in r2, but override 0 with 0x100 to deal with old
	 * bootloaders that assume Linux's default behavior.  (This is the case
	 * with, for example, the bootloader on the BCM2835.)  */
	ldr r3, =atags_ptr
	cmp r2, #0
	moveq r2, #0x100
	str r2, [r3]

	/* Continue execution at reset_handler.  */
	b reset_handler

park_secondary:
	wfe
	b park_secondary
	.endfunc

/* ARM exception vector table.  This is copied to address 0.  See A2.6
 * "Exceptions" of the ARM Architecture Reference Manual.  */
_vectors:
	ldr pc, reset_addr    /* Reset handler */
	ldr pc, undef_addr	  /* Undefined instruction handler */
	ldr pc, swi_addr	  /* Software interrupt handler */
	ldr pc, prefetch_addr /* Prefetch abort handler */
	ldr pc, abort_addr	  /* Data abort handler */
	ldr pc, reserved_addr /* Reserved */
	ldr pc, irq_addr	  /* IRQ (Interrupt request) handler */
	ldr pc, fiq_addr	  /* FIQ (Fast interrupt request) handler */

reset_addr:     .word reset_handler
undef_addr:     .word exc_undef       /* Pi3 bring-up: report + halt instead */
swi_addr:       .word reset_handler   /* of silently looping to reset_handler */
prefetch_addr:  .word exc_pabort
abort_addr:     .word exc_dabort
reserved_addr:  .word reset_handler
irq_addr:       .word irq_handler
fiq_addr:       .word reset_handler

_endvectors:

.section .text

/* Pi3 bring-up exception reporters: print "!EXC <type> <faulting-pc>" to the
 * PL011 UART and halt, instead of silently jumping to reset_handler (which
 * looked like a reboot loop).  Register-only (banked SP isn't set up in the
 * exception modes).  Look the printed PC up in xinu.elf to find the culprit. */
exc_undef:    mov r5, #'U'; sub r6, lr, #4; mov r8, #0; b exc_report
exc_pabort:   mov r5, #'P'; sub r6, lr, #4; mov r8, #0; b exc_report
exc_dabort:   mov r5, #'A'; sub r6, lr, #8; mrc p15, 0, r8, c6, c0, 0; b exc_report
/* (r9 = SCTLR, captured in exc_report so all paths show it) */
exc_report:
	mrc p15, 0, r9, c1, c0, 0    /* SCTLR: bit0=M(MMU) bit1=A(align) bit2=C */
	ldr r2, =0x3F201000          /* PL011 base */
	ldr r3, =0x301
	str r3, [r2, #0x30]          /* ensure UART enabled */
	mov r3, #0x0d; bl exc_putc
	mov r3, #0x0a; bl exc_putc
	mov r3, #'!'; bl exc_putc
	mov r3, #'E'; bl exc_putc
	mov r3, #'X'; bl exc_putc
	mov r3, #'C'; bl exc_putc
	mov r3, #' '; bl exc_putc
	mov r3, r5;   bl exc_putc    /* exception type: U/P/A */
	mov r3, #' '; bl exc_putc
	mov r7, #28                  /* print r6 as 8 hex digits */
exc_hex:
	mov r3, r6, lsr r7
	and r3, r3, #0xf
	cmp r3, #10
	addlt r3, r3, #'0'
	addge r3, r3, #('a' - 10)
	bl exc_putc
	subs r7, r7, #4
	bge exc_hex
	mov r3, #' '; bl exc_putc
	mov r3, #'d'; bl exc_putc
	mov r3, #'='; bl exc_putc
	mov r7, #28                  /* print r8 (DFAR, data fault addr) as hex */
exc_hex2:
	mov r3, r8, lsr r7
	and r3, r3, #0xf
	cmp r3, #10
	addlt r3, r3, #'0'
	addge r3, r3, #('a' - 10)
	bl exc_putc
	subs r7, r7, #4
	bge exc_hex2
	mov r3, #' '; bl exc_putc
	mov r3, #'s'; bl exc_putc
	mov r3, #'='; bl exc_putc
	mov r7, #28                  /* print r9 (SCTLR) as hex */
exc_hex3:
	mov r3, r9, lsr r7
	and r3, r3, #0xf
	cmp r3, #10
	addlt r3, r3, #'0'
	addge r3, r3, #('a' - 10)
	bl exc_putc
	subs r7, r7, #4
	bge exc_hex3
	mov r3, #0x0d; bl exc_putc
	mov r3, #0x0a; bl exc_putc
exc_halt:
	wfe
	b exc_halt
exc_putc:
	ldr r4, [r2, #0x18]          /* FR */
	tst r4, #0x20               /* TXFF */
	bne exc_putc
	str r3, [r2, #0x00]          /* DR */
	bx lr

	/* reset_handler: Reset handler routine executed to start up the kernel,
	 * when the ARM processor is reset, or (currently) when an unhandled
	 * exception occurs.  */
	.func reset_handler
reset_handler:

	/* Enter SYS mode with all interrupts disabled so the ARM processor is
	 * in a known state. */
	cpsid if, #ARM_MODE_SYS

	/* Re-enable the PL011 UART for early console output.  With enable_uart=1
	 * + dtoverlay=disable-bt the GPU firmware routes PL011 to GPIO14/15 and
	 * sets the 115200 baud divisor, but DISABLES the UART (CR=0) before
	 * handing off.  Set CR=UARTEN|TXE|RXE WITHOUT touching IBRD/FBRD, so
	 * kprintf works (at the firmware's correct 115200) from the very start —
	 * before the uart-pl011 driver's uartHwInit runs in sysinit.  The driver
	 * also skips IBRD/FBRD on arm-rpi3 to keep this inherited baud. */
	ldr r0, =0x3F201000        /* PL011 base (firmware-configured @115200) */
	ldr r1, =0x301
	str r1, [r0, #0x30]        /* CR = UARTEN | TXE | RXE */

	/* Copy the ARM exception table from .init section to address 0,
	 * including the absolute address table.  Here we assume it is exactly
	 * 16 words.  */
	mov r0, #0
	ldr r1, =_vectors
	ldmia r1!, {r2-r9}
	stmia r0!, {r2-r9}
	ldmia r1!, {r2-r9}
	stmia r0!, {r2-r9}

	/* Allow unaligned memory accesses.  gcc generates unaligned loads/stores
	 * (e.g. 16-bit reads of USB descriptor fields like wMaxPacketSize at odd
	 * offsets), and the USB enumeration hit an alignment data abort on the
	 * Cortex-A53.  On ARMv8-AArch32 the legacy U bit (22) is deprecated/RAO;
	 * what matters is SCTLR.A (bit 1) — it MUST be CLEAR to permit unaligned
	 * accesses to Normal memory.  Clear A, and set bit 22 for old cores. */
	mrc p15, 0, r0, c1, c0, 0
	bic r0, r0, #(1 << 1)        /* clear SCTLR.A: allow unaligned accesses */
	orr r0, r0, #(1 << 22)       /* legacy unaligned-support bit (RAO on ARMv7+) */
	mcr p15, 0, r0, c1, c0, 0

	/* Clear the .bss section of the kernel.  */
	ldr r0, =_bss
	ldr r1, =_end
	mov r2, #0
	mov r3, #0
	mov r4, #0
	mov r5, #0
	b bssloopa
bssloopb:
	stmia r0!, {r2-r5}
bssloopa:
	cmp r0, r1 	/* check that we still haven't hit the end of bss yet */
	blo bssloopb	/* if still below, go backwards and loop */

	/* Put the null thread's stack directly after the kernel image.  */
	add sp, r1, #NULLSTK

	/* The remaining memory available to the ARM will be Xinu's "memheap"
	 * region, which is used for dynamic memory allocation.  Set its
	 * starting address.  */
	ldr r0, =memheap
	str sp, [r0]

	/* Graphics (screenInit) is intentionally skipped on this Pi3 bring-up
	 * build: it drives the VideoCore mailbox/framebuffer, and framebuffer.h
	 * still hardcodes the Pi1 MMIO base (0x20000000), which would hang the
	 * GPU handshake on a Pi3.  The serial (PL011) console comes up without
	 * it.  Re-enable after framebuffer.h is ported to 0x3F000000.  */
	/* bl screenInit */

	/* Branch to the platform-independent C startup code, which takes no
	 * parameters and never returns.  */
	b nulluser
	.endfunc
