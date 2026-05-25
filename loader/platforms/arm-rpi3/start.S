/**
 * @file start.S
 *
 * Initialization code for Embedded Xinu on the Raspberry Pi.
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */


#include <arm.h>     /* For ARM_MODE_SYS */

#define NULLSTK 8192

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
undef_addr:     .word reset_handler
swi_addr:       .word reset_handler
prefetch_addr:  .word reset_handler
abort_addr:     .word reset_handler
reserved_addr:  .word reset_handler
irq_addr:       .word irq_handler
fiq_addr:       .word reset_handler

_endvectors:

.section .text

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

	/* Enable the following features by modifying the ARM processor's
	 * Control Register:
	 *
	 * - unaligned memory accesses   (bit 22)
	 *
	 * Note: Xinu shouldn't do any unaligned memory accesses on purpose, but
	 * we found that gcc can still generate unaligned memory accesses for
	 * tasks such as copying character strings.  (An alternative would be to
	 * pass -mno-unaligned-access to gcc to prevent it from doing so.)  */
	mrc p15, 0, r0, c1, c0, 0
	orr r0, #1 << 22
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
