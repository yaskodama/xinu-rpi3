/**
 * @file smp.h
 *
 * Worker-pool SMP for the BCM2837 (Pi 3 B+, Cortex-A53 x4), AArch32.
 *
 * Core 0 runs the OS; cores 1-3 are compute workers that wait (WFE) for a
 * job posted to a lock-free mailbox, run a [lo,hi) range function, and signal
 * done.  D-cache is OFF on every core (SCTLR.C=0, see system/platforms/
 * arm-rpi3/mmu.c), so the mailbox is coherent with no cache maintenance.
 */
#ifndef _SMP_H_
#define _SMP_H_

#define SMP_NCORES 4

/* A parallel work unit: count/accumulate over the half-open range [lo,hi).
 * `core` is the executing core id (0..3), for per-core scratch if needed. */
typedef long (*smp_range_fn)(long lo, long hi, int core);

/* Release cores 1-3 into the worker pool.  Idempotent, bounded wait.  Call
 * once after the MMU is up (lazily, on the first benchmark, is fine). */
void smp_init(void);

/* Number of cores that answered the bring-up (1 if SMP failed, up to 4). */
int  smp_cores_online(void);

/* This core's id (MPIDR Aff0). */
int  smp_core_id(void);

/* Split [0,n) across `ncores` cores, run fn on each chunk in parallel, and
 * return the sum of the chunk results.  ncores is clamped to online cores. */
long smp_parallel_sum(smp_range_fn fn, long n, int ncores);

#endif /* _SMP_H_ */
