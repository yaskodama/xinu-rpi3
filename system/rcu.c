/**
 * @file rcu.c
 *
 * Read-Copy-Update primitives for the uniprocessor arm-rpi3 Xinu kernel.
 * See include/rcu.h for the model + correctness argument.
 */
/* Embedded Xinu — arm-rpi3 kernel-evolution champion (concurrency_safety=rcu). */

#include <thread.h>
#include <interrupt.h>
#include <rcu.h>

/*
 * rcu_nest > 0 marks an active read-side section.  system/resched.c checks it
 * and defers rescheduling (just like resdefer) so the running reader is never
 * preempted part-way through.  Because the cores are parked, a single global
 * counter is sufficient: while it is non-zero the one reader thread owns the
 * CPU and no other thread (writer) can run.
 */
volatile int rcu_nest = 0;

void rcu_read_lock(void)
{
    irqmask im = disable();     /* brief: atomic bump vs. a clock-IRQ read */
    rcu_nest++;
    restore(im);
}

void rcu_read_unlock(void)
{
    irqmask im = disable();
    if (rcu_nest > 0)
        rcu_nest--;
    restore(im);
    rcu_mb();                   /* reader's loads complete before we leave */
}

/*
 * Wait out a grace period.  On this uniprocessor with non-preemptible readers,
 * the writer running synchronize_rcu() already implies no reader is mid-section
 * (only one thread runs at a time).  We still yield so that the scheduler
 * crosses at least one real quiescent boundary — this keeps the primitive
 * correct in spirit (and for any future SMP bring-up) rather than relying on a
 * subtle single-core argument at every call site.
 */
void synchronize_rcu(void)
{
    rcu_mb();
    yield();        /* let any other ready thread run to a quiescent point */
    yield();
    rcu_mb();
}

int rcu_in_read_section(void)
{
    return rcu_nest;
}
