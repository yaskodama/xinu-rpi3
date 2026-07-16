/**
 * @file smp.h
 *
 * Worker-pool SMP for the BCM2837's 4 Cortex-A53 cores (Pi 3 B/B+).
 *
 * Architecture (mirrors the Pi 4 port, see xinu-rpi4 docs/SMP_REPORT_JA.md):
 *   - Core 0 keeps running the whole single-core cooperative OS (scheduler,
 *     ready list, actors, net, USB, WiFi, video, gwm) UNCHANGED.  The ready
 *     list stays core-0-private: the AIPL/actor/net/USB runtime is
 *     non-reentrant, so a shared symmetric run queue would destabilise it.
 *   - Cores 1-3 are brought up as dedicated compute workers.  They idle in
 *     WFE, run a posted job (a range of work) in parallel, then signal done.
 *     They never touch the OS, never take interrupts (IRQ+FIQ stay masked)
 *     and never allocate — so they cannot race the kernel.
 *
 * Why lock-free coordination is sound here: the D-cache is OFF on this port
 * (mmu.c sets SCTLR.M + SCTLR.I but deliberately leaves SCTLR.C = 0), so
 * every data access goes straight to RAM.  Plain `volatile` plus `dsb`
 * barriers are therefore coherent across all four cores — no cache protocol,
 * no spinlocks, and no need for the A53's CPUECTLR.SMPEN coherency bit.
 * ★ If D-cache is ever enabled, this file's assumption breaks and the job
 * mailbox below needs real cache maintenance (or SMPEN + shareable mappings).
 *
 * Bring-up: the 32-bit Pi 3 firmware releases only core 0; cores 1-3 spin in
 * the armstub polling their per-core mailbox 3.  See smp.c.
 */

#ifndef _SMP_H_
#define _SMP_H_

#define SMP_NCORES 4

/* Bring up cores 1-3.  Call once from core 0 in platforminit(), after
 * mmu_init() and before any thread starts.  Safe even if the secondary cores
 * never respond — they simply stay offline and parallel jobs fall back to
 * running entirely on core 0. */
void smp_init(void);

/* Number of cores that came online and are ready for work (1..4). */
int smp_cores_online(void);

/* MPIDR[1:0] of the calling core (0..3). */
int smp_core_id(void);

/* A unit of work: compute a partial result over the half-open range [lo,hi).
 * `core` is the executing core id (0..3), e.g. for per-core scratch.  `ud` is
 * the caller's opaque job argument, passed through untouched.
 * ★ Must be pure compute: no kernel calls, no allocation, no I/O, no device
 * access.  It runs on a bare worker core with interrupts masked.
 *
 * ★ Put job parameters in `ud` (pointing at the CALLER'S stack), never in
 * file globals.  Core 0 is preemptive and several threads use this pool, so a
 * thread that staged parameters in globals can be preempted by another that
 * overwrites them — and then compute a wrong answer from the other's job.
 * A per-caller `ud` makes that race structurally impossible. */
typedef long (*smp_range_fn)(long lo, long hi, int core, void *ud);

/* Split [0,n) into `ncores` contiguous chunks, run `fn` on each chunk in
 * parallel across cores 0..ncores-1 (core 0 runs its own chunk inline), and
 * return the sum of the partial results.  Blocks until every chunk is done.
 * A worker that does not finish within a bounded spin is taken over by core 0,
 * so the result is always correct and a hung worker can never wedge the OS.
 * If another thread already holds the pool, the whole job runs on the calling
 * core — correct, just not parallel. */
long smp_parallel_sum(smp_range_fn fn, void *ud, long n, int ncores);

/* As smp_parallel_sum, but the chunk boundaries are taken from `bounds`, an
 * array of ncores+1 ascending indices (bounds[0]..bounds[ncores]).  Lets a
 * caller hand out unequal chunks when the per-index cost is skewed — as it is
 * for N-Queens, where the centre columns of the first row cost far more than
 * the edges. */
long smp_parallel_sum_bounds(smp_range_fn fn, void *ud, const long *bounds,
                             int ncores);

#endif /* _SMP_H_ */
