/**
 * @file rcu.h
 *
 * Minimal Read-Copy-Update (RCU) for the arm-rpi3 Xinu kernel.
 *
 * This is the `concurrency_safety = rcu` axis surfaced as the champion of the
 * Xinu kernel-evolution GA (aice-pi-evolution .../2026-05-21_xinu_kernel_evolution).
 * RCU lets readers traverse a shared structure with NO lock and NO interrupt
 * disable, while writers publish a new version and defer reclaiming the old one
 * until every pre-existing reader has finished (a "grace period").
 *
 * Model — why a simple implementation is correct here:
 *  - The arm-rpi3 port parks the secondary cores (MPIDR guard), so exactly one
 *    thread runs at a time (uniprocessor).
 *  - rcu_read_lock() makes the read section NON-PREEMPTIBLE: resched() defers
 *    while rcu_nest > 0 (see system/resched.c), so a reader can never be
 *    suspended part-way through.  A read section therefore runs to completion
 *    within a single scheduling quantum.
 *  - Consequently, whenever a writer is running, no reader holds a pointer into
 *    the structure.  After the writer unlinks a node and calls synchronize_rcu()
 *    (which yields, crossing a quiescent boundary), the old node is unreachable
 *    by any reader and is safe to free.
 *
 * Contract: a read section MUST NOT block, sleep(), yield(), or wait() — doing
 * so would stall preemption for the whole system (like resdefer).  Keep read
 * sections short pointer-chasing loops.
 */
#ifndef _RCU_H_
#define _RCU_H_

/* Data memory barrier — ARMv7-A / Cortex-A53 in AArch32 (inner shareable). */
#define rcu_mb() __asm__ __volatile__("dmb ish" ::: "memory")

/*
 * rcu_assign_pointer(p, v): publish pointer v into p so that all prior stores
 *   that initialised *v are visible to a reader before it can observe p.
 * rcu_dereference(p): read an RCU-protected pointer inside a read section,
 *   ordered before subsequent dereferences of it.
 */
#define rcu_assign_pointer(p, v) do { rcu_mb(); (p) = (v); } while (0)
#define rcu_dereference(p) ({ __typeof__(p) _rcu_p = (p); rcu_mb(); _rcu_p; })

/* Enter / leave an RCU read-side critical section (non-preemptible, nests). */
void rcu_read_lock(void);
void rcu_read_unlock(void);

/* Block until a grace period elapses: every reader that started before this
 * call has finished.  Call OUTSIDE a read section (typically a writer, after
 * unlinking a node and before freeing it). */
void synchronize_rcu(void);

/* Diagnostic: current read-section nesting depth (0 = not in a section). */
int  rcu_in_read_section(void);

#endif /* _RCU_H_ */
