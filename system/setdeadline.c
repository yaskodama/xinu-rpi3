/**
 * @file setdeadline.c
 *
 * S3 DeadlineHints from Xinu_KernelEvolution_Round1.aice (minimal).
 *
 * Attach an absolute clock-tick deadline to a thread.  When resched
 * runs (every clock tick, or on yield/wait/signal), the scheduler
 * looks for any THRREADY thread whose deadline is still in the
 * future and dispatches the soonest one ahead of priority order.
 * After dispatch the deadline is cleared so it doesn't re-trigger.
 *
 * Call sites:
 *   - Kernel: `setdeadline(tid, ticks_from_now)`.  ticks_from_now is
 *     a relative count in clkticks (= ms, since CLKTICKS_PER_SEC=1000).
 *   - AIPL: typing_env exposes `setdeadline(tid:int, ms:int) -> int`.
 *     A reasonable use case is to mark the next dispatch of a
 *     time-sensitive actor (e.g., a watchdog) as urgent.
 */
/* Embedded Xinu — S1 Round 1 deadline hints. */

#include <kernel.h>
#include <thread.h>
#include <clock.h>

int setdeadline(tid_typ tid, int ticks_from_now)
{
    irqmask im;
    if (isbadtid(tid)) return SYSERR;
    if (ticks_from_now <= 0) return SYSERR;
    im = disable();
    thrtab[tid].deadline_at = clkticks + (unsigned long)ticks_from_now;
    restore(im);
    return OK;
}

int cleardeadline(tid_typ tid)
{
    irqmask im;
    if (isbadtid(tid)) return SYSERR;
    im = disable();
    thrtab[tid].deadline_at = 0;
    restore(im);
    return OK;
}
