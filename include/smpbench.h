/**
 * @file smpbench.h
 *
 * The SMP benchmark workloads, shared by two front-ends:
 *
 *   - apps/webactor.c's /bench HTTP route (the default kernel), and
 *   - the boot-time console harness smpbench_run_all() (the DCACHE_EXPERIMENT
 *     kernel, which has no networking because its USB stack is compiled out —
 *     the DWC DMA engine cannot coexist with SCTLR.C=1; see mmu.c).
 *
 * ★ Both front-ends MUST call the same code.  The whole point of the D-cache
 * experiment is to compare cached against uncached, so if the two paths ran
 * separate copies of the workload the comparison would measure the difference
 * between the copies as much as the difference between the cache settings.
 *
 * Each run executes the workload twice — once serially on core 0, once across
 * the worker pool — and reports both times plus whether the two answers agree.
 * Timing uses clkcount() (the System Timer's 1 MHz free-running counter), NOT
 * clktime/clkticks: those are incremented by the timer interrupt, which does
 * not reliably fire on this board and made every measurement read 0 ms.
 *
 * Absolute times are NOT comparable across runs: the firmware changes the ARM
 * clock (the first run after boot is ~2.33x faster, matching the Pi 3 B+'s
 * 1.4 GHz / 600 MHz range).  The SPEEDUP is stable, because the serial and
 * parallel halves run back-to-back inside one call at whatever clock is
 * current.  Report ratios; treat absolute numbers as indicative only.
 */

#ifndef _SMPBENCH_H_
#define _SMPBENCH_H_

enum {
    SMPBENCH_NQUEENS = 0,   /* pure compute, recursive     */
    SMPBENCH_DINING,        /* pure register arithmetic    */
    SMPBENCH_PRIMES,        /* compute, trial division     */
    SMPBENCH_FILL,          /* bulk memory stores (= what drawing costs) */
    SMPBENCH_NULL,          /* nothing: measures the pool's own round trip */
    SMPBENCH_NKINDS
};

struct smpbench_result {
    const char   *label;
    const char   *metric;      /* what `value` counts: "solutions", "words", ... */
    long          value;       /* answer from the serial run                     */
    long          value_par;   /* answer from the parallel run — must match      */
    unsigned long us_1;        /* serial microseconds                            */
    unsigned long us_n;        /* parallel microseconds                          */
    int           cores;       /* cores the pool had available                   */
    int           agree;       /* value == value_par                             */
    int           speedup_x100;/* us_1 * 100 / us_n, 0 if us_n == 0              */
};

/* Run one workload.  `n` is the kind's size parameter; pass 0 for its default
 * (board size 11, 5 diners, 200000 primes, the whole 1 MB fill). */
void smpbench_run(int kind, int n, int cores, struct smpbench_result *out);

/* Map a request string containing "kind=..." to a SMPBENCH_* value.
 * Defaults to SMPBENCH_NQUEENS when no kind is named. */
int smpbench_kind_of(const char *req);

/* N-Queens partial over first-row columns [c0,c1), spread across `nc` cores.
 * Used by the /nqpart cluster route. */
long smpbench_nqueens_range(int n, int c0, int c1, int nc);

/* Boot-time harness: run the suite and print a report to the console.
 * Used by the DCACHE_EXPERIMENT build, which has no network to serve /bench. */
void smpbench_run_all(void);

#endif /* _SMPBENCH_H_ */
