/**
 * @file resched.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <thread.h>
#include <clock.h>
#include <queue.h>
#include <memory.h>

extern void ctxsw(void *, void *, uchar);
extern tid_typ getitem(tid_typ);
extern void mlfq_reset_quantum(int tid);  /* S2 MLFQ — system/aging.c */
extern volatile int rcu_nest;             /* >0 inside an RCU read section — system/rcu.c */
int resdefer;                   /* >0 if rescheduling deferred */

/* S3 DeadlineHints: scan thrtab for the THRREADY thread with the
 * soonest still-in-time deadline.  Returns the tid or -1 if no
 * deadline thread is currently eligible.  O(NTHREAD) per call, but
 * NTHREAD is small (~50) and the field is touched only when at
 * least one deadline_at is non-zero on a hot thread.  We don't
 * bother short-circuiting: the cost is dominated by the L1 hit
 * pattern, not the comparisons. */
static tid_typ s3_pick_deadline(void)
{
    int i;
    tid_typ best = (tid_typ)-1;
    unsigned long best_d = (unsigned long)-1;
    for (i = 0; i < NTHREAD; i++) {
        if (thrtab[i].state != THRREADY) continue;
        if (thrtab[i].deadline_at == 0) continue;
        if (thrtab[i].deadline_at < clkticks) continue;  /* missed → ignore */
        if (thrtab[i].deadline_at < best_d) {
            best_d = thrtab[i].deadline_at;
            best   = (tid_typ)i;
        }
    }
    return best;
}

/**
 * @ingroup threads
 *
 * Reschedule processor to highest priority ready thread.
 * Upon entry, thrcurrent gives current thread id.
 * Threadtab[thrcurrent].pstate gives correct NEXT state
 * for current thread if other than THRREADY.
 * @return OK when the thread is context switched back
 */
int resched(void)
{
    uchar asid;                 /* address space identifier */
    struct thrent *throld;      /* old thread entry */
    struct thrent *thrnew;      /* new thread entry */

    if (resdefer > 0)
    {                           /* if deferred, increase count & return */
        resdefer++;
        return (OK);
    }

    /* RCU read side: while a reader is mid-section, stay non-preemptible so it
     * runs to completion within this quantum.  A dropped preemption tick costs
     * at most one tick of latency (read sections are microseconds, the tick is
     * 10 ms); the next tick after rcu_read_unlock() preempts normally. */
    if (rcu_nest > 0)
    {
        return (OK);
    }

    throld = &thrtab[thrcurrent];

    throld->intmask = disable();

    if (THRCURR == throld->state)
    {
        if (nonempty(readylist) && (throld->prio > firstkey(readylist)))
        {
            restore(throld->intmask);
            return OK;
        }
        throld->state = THRREADY;
        insert(thrcurrent, readylist, throld->prio);
    }

    /* S3 DeadlineHints: prefer a still-eligible deadline thread over
     * the head of the ready queue.  EDF dominates priority — the
     * deadline says "this slice MUST run before tick D", which is a
     * stronger commitment than priority.  After dispatch we clear
     * deadline_at so the thread does not keep cutting in line. */
    {
        tid_typ dl_tid = s3_pick_deadline();
        if (dl_tid != (tid_typ)-1 && dl_tid < (tid_typ)NTHREAD) {
            (void)getitem(dl_tid);          /* unlink from readylist */
            thrcurrent = dl_tid;
            thrtab[dl_tid].deadline_at = 0;
        } else {
            /* get highest priority thread from ready list */
            thrcurrent = dequeue(readylist);
        }
    }
    thrnew = &thrtab[thrcurrent];
    thrnew->state = THRCURR;

    /* S1 PriorityAging: snap the dispatched thread's effective prio
     * back to its base, so any aging boost it accumulated while
     * waiting is "consumed" on selection.  Pure book-keeping — the
     * thread already has the CPU at this point. */
    thrnew->prio = thrnew->basepri;

    /* S2 MLFQ: fresh quantum for the new slice.  If the thread
     * voluntarily blocks before the quantum expires it keeps the
     * boost; if it spins, clkhandler's mlfq_tick will demote
     * prio after MLFQ_QUANTUM_MS ticks. */
    mlfq_reset_quantum((int)thrcurrent);

    /* change address space identifier to thread id */
    asid = thrcurrent & 0xff;
    ctxsw(&throld->stkptr, &thrnew->stkptr, asid);

    /* old thread returns here when resumed */
    restore(throld->intmask);
    return OK;
}
