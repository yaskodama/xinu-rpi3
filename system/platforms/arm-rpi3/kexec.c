/**
 * @file kexec.c
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#include <interrupt.h>
#include <kernel.h>
#include <kexec.h>
#include <string.h>


/* The below array contains a stub of ARM instructions used to copy the
 * new kernel into its final location, then pass control to it.
 *
 * This stub is hard-coded as an array because it needs to be copied to a
 * location in which it cannot be overwritten by itself while copying the new
 * kernel.  Therefore, its size needs to be known and it needs to be fully
 * relocatable (which in theory the assembler does not guarantee).
 *
 * Arguments are:
 *
 * r0:  pointer to new kernel
 * r1:  size of new kernel in 32-bit words
 * r2:  pointer to ARM boot tags (preserved in r2 for convenience of new kernel)
 *
 * This is hard-coded to copy the kernel to address 0x8000.
 */

/*00000000 <copy_kernel>:*/
  /* 0:   e3a04902    mov     r4, #32768        ; 0x8000 */
  /* 4:   e4903004    ldr     r3, [r0], #4               */
  /* 8:   e4843004    str     r3, [r4], #4               */
  /* c:   e2511001    subs    r1, r1, #1                 */
  /*10:   1afffffb    bne     4 <copy_kernel+0x4>        */
  /*14:   e3a0f902    mov     pc, #32768        ; 0x8000 */
static const ulong copy_kernel[] = {
    0xe3a04902,
    0xe4903004,
    0xe4843004,
    0xe2511001,
    0x1afffffb,
    0xe3a0f902,
};

#define COPY_KERNEL_ADDR ((void*)(0x8000 - sizeof(copy_kernel)))

/**
 * Kernel execute - Transfer control to a new kernel.
 *
 * This is the Raspberry Pi implementation.  In this implementation, the new
 * kernel must be valid for the Raspberry Pi, including being linked to run at
 * and having an entry point at address 0x8000.
 *
 * @param kernel
 *      Pointer to the new kernel image loaded anywhere in memory.
 * @param size
 *      Size of the new kernel image in bytes.
 *
 * @return
 *      This function never returns.  If it somehow does, then something has
 *      gone horribly wrong.
 */
syscall kexec(const void *kernel, uint size)
{
    irqmask im;

    im = disable();

    /* Copy the assembly stub into a safe location.  (D-cache is OFF in this
     * kernel — see system/platforms/arm-rpi3/mmu.c — so these writes reach RAM
     * directly.) */
    memcpy(COPY_KERNEL_ADDR, copy_kernel, sizeof(copy_kernel));

    /* Warm-restart cache/MMU teardown — THE fix for "kexec hangs the new
     * kernel".  This kernel runs with MMU + I-cache ENABLED.  After we copy the
     * new kernel over 0x8000, the I-cache still holds the OLD kernel's
     * instructions for that range, so `mov pc,#0x8000` would execute stale code
     * and crash.  Drop the MMU and I-cache and invalidate them so the new
     * kernel is fetched fresh from RAM — i.e. the same bare environment the
     * firmware hands a cold boot.  Identity-mapped (VA==PA), so turning the MMU
     * off keeps execution flowing. */
    asm volatile (
        "dsb\n isb\n"
        "mrc p15, 0, r0, c1, c0, 0\n"   /* read SCTLR                       */
        "bic r0, r0, #(1 << 0)\n"       /* M = 0  -> MMU off                */
        "bic r0, r0, #(1 << 12)\n"      /* I = 0  -> I-cache off            */
        "mcr p15, 0, r0, c1, c0, 0\n"
        "mov r0, #0\n"
        "mcr p15, 0, r0, c7, c5, 0\n"   /* ICIALLU — invalidate I-cache     */
        "mcr p15, 0, r0, c7, c5, 6\n"   /* BPIALL  — invalidate branch pred */
        "mcr p15, 0, r0, c8, c7, 0\n"   /* TLBIALL — invalidate TLB         */
        "dsb\n isb\n"
        : : : "r0", "memory"
    );

    /* Enter the assembly stub to copy the new kernel into its final location,
     * then pass control to it.  */
    extern void *atags_ptr;
    (( void (*)(const void *, ulong, void *))(COPY_KERNEL_ADDR))
                (kernel, (size + 3) / 4, atags_ptr);

    /* Control should never reach here.  */
    restore(im);
    return SYSERR;
}

/**
 * Hardened chainload — boot a freshly-uploaded kernel image from RAM without a
 * power cycle, the rpi4-style way.  This is the robust sibling of kexec(): the
 * body called by webactor's /chainload route, and what fixes the "kexec
 * sometimes hard-hangs / resets to the SD kernel" instability.
 *
 * Two reliability differences vs kexec():
 *
 *   1. It masks BOTH IRQ *and FIQ* (cpsid if).  kexec()'s disable() masks IRQ
 *      only; an FIQ arriving after we drop the MMU/I-cache — but before the new
 *      kernel installs its own vectors — would vector through 0x1C, which by
 *      then holds the *new* image's bytes, and crash.  Masking FIQ closes that
 *      window.
 *
 *   2. It is self-contained: it does the MMU/cache teardown itself with
 *      interrupts already masked, instead of relying on the caller to have
 *      called mmu_disable() first (which /kexec did from thread context with
 *      IRQs still live — a race that could fault).
 *
 * The copy is forward (src high in .bss, dst = 0x8000, so src > dst) and thus
 * memmove-safe even if the ranges overlap.  RAM-only: a bad image just needs a
 * power cycle, the SD is untouched.  Never returns.
 *
 * @param kernel  pointer to the new kernel image (anywhere in RAM above 0x8000)
 * @param size    image size in bytes
 */
void kernel_chainload(const void *kernel, uint size)
{
    /* Mask IRQ + FIQ atomically — we are committing, nothing may preempt us. */
    asm volatile ("cpsid if\n dsb\n isb\n" : : : "memory");

    /* Relocate the copy/jump stub just below the load address (D-cache is OFF,
     * so this write lands in RAM directly). */
    memcpy(COPY_KERNEL_ADDR, copy_kernel, sizeof(copy_kernel));

    /* Warm-restart teardown: drop MMU + I-cache and invalidate I-cache, branch
     * predictor and TLB so the new kernel is fetched fresh from RAM (the same
     * bare state firmware hands a cold boot).  Identity-mapped, so turning the
     * MMU off keeps execution flowing from the same instructions. */
    asm volatile (
        "dsb\n isb\n"
        "mrc p15, 0, r0, c1, c0, 0\n"   /* read SCTLR                       */
        "bic r0, r0, #(1 << 0)\n"       /* M = 0  -> MMU off                */
        "bic r0, r0, #(1 << 12)\n"      /* I = 0  -> I-cache off            */
        "mcr p15, 0, r0, c1, c0, 0\n"
        "mov r0, #0\n"
        "mcr p15, 0, r0, c7, c5, 0\n"   /* ICIALLU — invalidate I-cache     */
        "mcr p15, 0, r0, c7, c5, 6\n"   /* BPIALL  — invalidate branch pred */
        "mcr p15, 0, r0, c8, c7, 0\n"   /* TLBIALL — invalidate TLB         */
        "dsb\n isb\n"
        : : : "r0", "memory"
    );

    extern void *atags_ptr;
    (( void (*)(const void *, ulong, void *))(COPY_KERNEL_ADDR))
                (kernel, (size + 3) / 4, atags_ptr);

    /* Unreachable. */
    while (1) { }
}
