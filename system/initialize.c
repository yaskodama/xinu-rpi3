/**
 * @file initialize.c
 * The system begins intializing after the C environment has been
 * established.  After intialization, the null thread remains always in
 * a ready (THRREADY) or running (THRCURR) state.
 */
/* Embedded Xinu, Copyright (C) 2009, 2013.  All rights reserved. */

#include <kernel.h>
#include <backplane.h>
#include <clock.h>
#include <device.h>
#include <gpio.h>
#include <memory.h>
#include <bufpool.h>
#include <mips.h>
#include <thread.h>
#include <tlb.h>
#include <queue.h>
#include <semaphore.h>
#include <monitor.h>
#include <mailbox.h>
#include <network.h>
#include <nvram.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <safemem.h>
#include <platform.h>

#ifdef WITH_USB
#  include <usb_subsystem.h>
#endif

/* Function prototypes */
extern thread main(void);       /* main is the first thread created    */
static int sysinit(void);       /* intializes system structures        */

/* Declarations of major kernel variables */
struct thrent thrtab[NTHREAD];  /* Thread table                   */
struct sement semtab[NSEM];     /* Semaphore table                */
struct monent montab[NMON];     /* Monitor table                  */
qid_typ readylist;              /* List of READY threads          */
struct memblock memlist;        /* List of free memory blocks     */
struct bfpentry bfptab[NPOOL];  /* List of memory buffer pools    */

/* Active system status */
int thrcount;                   /* Number of live user threads         */
tid_typ thrcurrent;             /* Id of currently running thread      */

/* Params set by startup.S */
void *memheap;                  /* Bottom of heap (top of O/S stack)   */
ulong cpuid;                    /* Processor id                        */

struct platform platform;       /* Platform specific configuration     */

/**
 * Intializes the system and becomes the null thread.
 * This is where the system begins after the C environment has been 
 * established.  Interrupts are initially DISABLED, and must eventually 
 * be enabled explicitly.  This routine turns itself into the null thread 
 * after initialization.  Because the null thread must always remain ready 
 * to run, it cannot execute code that might cause it to be suspended, wait 
 * for a semaphore, or put to sleep, or exit.  In particular, it must not 
 * do I/O unless it uses kprintf for synchronous output.
 */
void nulluser(void)
{
    /* Platform-specific initialization  */
    platforminit();

    /* Sec3 MeasuredBoot — log a fingerprint of the kernel text+rodata
     * range before anything else runs.  Pure observation here; future
     * work will refuse to continue boot on mismatch. */
    {
        extern void measured_boot_print(void);
        measured_boot_print();
    }

#ifdef _XINU_PLATFORM_ARM_RPI3_
    /* Bring up the HDMI framebuffer via the VideoCore mailbox.  Done here
     * (rather than in start.S like the Pi1 path) so the serial console is
     * already usable and a mailbox stall would be visible on UART. */
    {
        extern void screenInit(void);
        screenInit();
    }
#endif

    /* General initialization  */
    sysinit();

    /* Buddy allocator (Xinu Kernel Evolution Round 2 champion axis
     * memory_alloc=buddy).  Bring up the arena and run its boot
     * self-test now that the classic heap (memget) is live but before
     * interrupts/threads start, so the [Buddy] PASS/FAIL line lands on
     * the console early.  The result is also retrievable at runtime via
     * the webactor /api/buddy route. */
    {
        extern int buddy_selftest(void);
        buddy_selftest();
    }

    /* Enable interrupts  */
    enable();

    /* Spawn the main thread  */
    ready(create(main, INITSTK, INITPRIO, "MAIN", 0), RESCHED_YES);

    /* null thread has nothing else to do but cannot exit.
     * S4 (Xinu_KernelEvolution Round 1): the historic `#ifndef DEBUG`
     * guard around pause() (= ARM `wfi`) was a foot-gun — any debug
     * build, or a Makefile that piped a -D flag through the DEBUG
     * variable, would silently turn the idle loop into a hot spin.
     * The idle thread does NO useful work, so wfi is always correct;
     * any pending interrupt (clock tick at minimum) wakes the CPU.
     * Drops the QEMU host process from ~100% CPU to single digits and
     * is a prerequisite for power-aware scheduling on real Raspberry
     * Pi hardware. */
    while (TRUE)
    {
        pause();
    }
}

/**
 * Intializes all Xinu data structures and devices.
 * @return OK if everything is initialized successfully
 */
static int sysinit(void)
{
    int i;
    struct thrent *thrptr;      /* thread control block pointer  */
    struct memblock *pmblock;   /* memory block pointer          */

    /* Initialize system variables */
    /* Count this NULLTHREAD as the first thread in the system. */
    thrcount = 1;

    /* Initialize free memory list */
    memheap = roundmb(memheap);
    platform.maxaddr = truncmb(platform.maxaddr);
    memlist.next = pmblock = (struct memblock *)memheap;
    memlist.length = (uint)(platform.maxaddr - memheap);
    pmblock->next = NULL;
    pmblock->length = (uint)(platform.maxaddr - memheap);

    /* Initialize thread table */
    for (i = 0; i < NTHREAD; i++)
    {
        thrtab[i].state = THRFREE;
    }

    /* initialize null thread entry */
    thrptr = &thrtab[NULLTHREAD];
    thrptr->state = THRCURR;
    thrptr->prio = 0;
    strlcpy(thrptr->name, "prnull", TNMLEN);
    thrptr->stkbase = (void *)&_end;
    thrptr->stklen = (ulong)memheap - (ulong)&_end;
    thrptr->stkptr = 0;
    thrptr->memlist.next = NULL;
    thrptr->memlist.length = 0;
    thrcurrent = NULLTHREAD;

    /* Initialize semaphores */
    for (i = 0; i < NSEM; i++)
    {
        semtab[i].state = SFREE;
        semtab[i].queue = queinit();
    }

    /* Initialize monitors */
    for (i = 0; i < NMON; i++)
    {
        montab[i].state = MFREE;
    }

    /* Initialize buffer pools */
    for (i = 0; i < NPOOL; i++)
    {
        bfptab[i].state = BFPFREE;
    }

    /* initialize thread ready list */
    readylist = queinit();

#if SB_BUS
    backplaneInit(NULL);
#endif                          /* SB_BUS */

#if RTCLOCK
    /* initialize real time clock */
    clkinit();
#endif                          /* RTCLOCK */

#ifdef UHEAP_SIZE
    /* Initialize user memory manager */
    {
        void *userheap;             /* pointer to user memory heap   */
        userheap = stkget(UHEAP_SIZE);
        if (SYSERR != (int)userheap)
        {
            userheap = (void *)((uint)userheap - UHEAP_SIZE + sizeof(int));
            memRegionInit(userheap, UHEAP_SIZE);

            /* initialize memory protection */
            safeInit();

            /* initialize kernel page mappings */
            safeKmapInit();
        }
    }
#endif

#if USE_TLB
    /* initialize TLB */
    tlbInit();
    /* register system call handler */
    exceptionVector[EXC_SYS] = syscall_entry;
#endif                          /* USE_TLB */

#if NMAILBOX
    /* intialize mailboxes */
    mailboxInit();
#endif

#if NDEVS
    for (i = 0; i < NDEVS; i++)
    {
        devtab[i].init((device*)&devtab[i]);
    }
#endif

#ifdef WITH_USB
    { extern void usbmsc_init(void); usbmsc_init(); }  /* register USB MSC before enumeration */
    usbinit();
#endif

#if NVRAM
    nvramInit();
#endif

#if NNETIF
    netInit();
#endif

#if GPIO
    gpioLEDOn(GPIO_LED_CISCOWHT);
#endif
    return OK;
}
