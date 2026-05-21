/**
 * @file aging.c
 *
 * S1 PriorityAging from Xinu_KernelEvolution_Round1.aice.
 *
 * Every AGING_PERIOD_MS milliseconds, walk every THRREADY thread and
 * bump its effective priority by +1 (capped at basepri + AGING_MAX_BOOST).
 * This prevents a long-running higher-priority thread from indefinitely
 * starving lower-priority neighbours: aged-up threads eventually catch
 * up and get dispatched.
 *
 * When a thread is *selected* (transitions to THRCURR — see resched.c)
 * its prio is snapped back to basepri so the boost is consumed and
 * the thread re-earns aging over its next ready interval.
 *
 * The ready queue is sorted by descending key (= prio).  We bump
 * `quetab[tid].key` in place along with `thrtab[tid].prio`, then walk
 * the queue once with a single insertion-sort pass.  At INTPRIO=20
 * with NTHREAD typically 50, this is ~50 comparisons — trivial cost
 * against a 100ms tick.
 */
/* Embedded Xinu — S1 PriorityAging extension. */

#include <kernel.h>
#include <thread.h>
#include <queue.h>
#include <clock.h>

#define AGING_PERIOD_MS  100        /* aging tick interval (= 100 clkticks) */
#define AGING_MAX_BOOST    5        /* prio can be raised at most basepri+5 */

/* The current tick counter modulo AGING_PERIOD_MS.  clkhandler bumps
 * this on every tick; when it wraps we run the aging sweep. */
static int aging_cntr = 0;

/* Resort the readylist after we may have bumped keys upwards.  The
 * underlying queue is a doubly-linked list in descending-key order;
 * since we only increased keys, we can repair the invariant by
 * scanning from the head and re-inserting any out-of-order entry.
 *
 * Worst case is O(n^2) but n is bounded by NTHREAD (~50), which on
 * 100ms cadence is negligible.  Interrupts are already disabled by
 * the clock-handler context so we can mutate quetab[] freely. */
static void age_readylist_resort(void)
{
    int head, tail, cur, next, key, k_next;

    head = quehead(readylist);
    tail = quetail(readylist);
    cur  = quetab[head].next;

    /* Walk forward; if cur's key < next's key (descending order
     * broken), bubble cur backwards. */
    while (cur != tail) {
        next  = quetab[cur].next;
        if (next == tail) break;
        k_next = quetab[next].key;
        key    = quetab[cur].key;
        if (key < k_next) {
            /* Move `cur` so it sits AFTER `next` (preserve descending). */
            int prev = quetab[cur].prev;
            int nn   = quetab[next].next;
            /* unlink cur */
            quetab[prev].next = next;
            quetab[next].prev = prev;
            /* relink cur after next */
            quetab[next].next = cur;
            quetab[cur].prev  = next;
            quetab[cur].next  = nn;
            quetab[nn].prev   = cur;
            /* don't advance cur — re-check from same logical position */
            continue;
        }
        cur = next;
    }
}

/* Public entry point — called from clkhandler() once per tick. */
void aging_tick(void)
{
    int i;
    int bumped = 0;

    aging_cntr++;
    if (aging_cntr < AGING_PERIOD_MS) {
        return;
    }
    aging_cntr = 0;

    /* Walk every thread that is currently READY; bump its effective
     * prio by 1, capped at basepri + AGING_MAX_BOOST.  Update the
     * matching ready-queue key so the resort pass sees the new
     * value. */
    for (i = 0; i < NTHREAD; i++) {
        if (thrtab[i].state != THRREADY) continue;
        if (thrtab[i].prio >= thrtab[i].basepri + AGING_MAX_BOOST) continue;
        thrtab[i].prio++;
        quetab[i].key = thrtab[i].prio;
        bumped++;
    }

    if (bumped > 0) {
        age_readylist_resort();
    }
}
