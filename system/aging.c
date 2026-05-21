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

/* ============================================================
 *  S2 MLFQ — multi-level feedback via per-thread quantum.
 *
 *  Single ready queue (Xinu does not have separate band lists);
 *  feedback comes from decrementing prio when a thread consumes
 *  its full quantum without voluntarily blocking.  S1 aging then
 *  restores prio over time so I/O-bound threads keep interactive
 *  responsiveness while CPU-bound threads naturally settle into
 *  a "batch" effective priority.
 *
 *  Constants (in clkticks, = ms at CLKTICKS_PER_SEC=1000):
 *    MLFQ_QUANTUM_MS    40   one slice between feedback decisions
 *    MLFQ_DEMOTE_STEP   5    prio drop per quantum miss
 *    MLFQ_MAX_DEMOTE   15    floor = basepri - MLFQ_MAX_DEMOTE
 *
 *  S1 (aging) and S2 (MLFQ) share the prio field; S1 nudges +1
 *  every 100 ticks, S2 demotes -5 every 40 ticks if the current
 *  thread used its full quantum.  Net: a busy thread loses ~5/40
 *  per ms and gains ~1/100 per ms when ready, settling around
 *  prio - 12.5 if it never blocks.
 * ============================================================ */

#define MLFQ_QUANTUM_MS    40
#define MLFQ_DEMOTE_STEP   5
#define MLFQ_MAX_DEMOTE   15

void mlfq_tick(void)
{
    int t = (int)thrcurrent;
    if (t == NULLTHREAD) return;
    if (t < 0 || t >= NTHREAD) return;
    if (thrtab[t].state != THRCURR) return;
    /* Decrement remaining quantum.  When it reaches zero, demote
     * prio by MLFQ_DEMOTE_STEP (down to the basepri - MAX floor)
     * and reset the quantum for the next slice. */
    thrtab[t].quantum_left--;
    if (thrtab[t].quantum_left <= 0) {
        int floor_prio = thrtab[t].basepri - MLFQ_MAX_DEMOTE;
        if (thrtab[t].prio > floor_prio) {
            thrtab[t].prio -= MLFQ_DEMOTE_STEP;
            if (thrtab[t].prio < floor_prio) thrtab[t].prio = floor_prio;
        }
        thrtab[t].quantum_left = MLFQ_QUANTUM_MS;
    }
}

/* Reset quantum on dispatch — called from resched.c after a thread
 * becomes THRCURR.  Keeps every new slice starting with the full
 * MLFQ_QUANTUM_MS budget regardless of how the thread arrived. */
void mlfq_reset_quantum(int tid)
{
    if (tid < 0 || tid >= NTHREAD) return;
    thrtab[tid].quantum_left = MLFQ_QUANTUM_MS;
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
