/**
 * @file smpbench.c
 *
 * The SMP benchmark workloads.  See include/smpbench.h for why these live in
 * their own module rather than inside webactor.c: the DCACHE_EXPERIMENT kernel
 * has no networking (its USB stack is compiled out, because the DWC DMA engine
 * cannot coexist with the D-cache being on), so it must reach the same
 * workloads through a console harness instead of the /bench HTTP route.
 *
 * This code was moved verbatim out of apps/webactor.c, which had grown both
 * the workloads and the HTTP formatting.  webactor.c now calls in here.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <clock.h>      /* clkcount() — 1 MHz free-running; clkticks is stuck */
#include <smp.h>
#include <smpbench.h>

extern unsigned int mmu_read_sctlr(void);

/* ---- Benchmarks for the unified /bench route ----
 * Each kind runs twice: once serially on core 0 for the baseline, then across
 * the worker pool (see include/smp.h) for the N-core time.  The output fields
 * match the rpi4/rpi5 SMP boards so the Mesh Control Center tabulates all
 * three uniformly.
 *
 * Load balancing: a worker chunk is fixed at post time (there is no shared
 * work queue — a dynamic one would need LDREX/STREX, which faults on this
 * port's non-cacheable D-cache-off mappings).  So where per-index cost is
 * skewed, the chunks are INTERLEAVED by stride rather than contiguous:
 *   - nqueens: first-row subtree cost is centre-heavy and mirror-symmetric,
 *     so contiguous chunks would give the middle cores several times the work.
 *   - primes:  trial division costs ~sqrt(x), so the top contiguous chunk
 *     would cost ~3x the bottom one (capping speedup near 2.9 instead of 4).
 *   - dining:  uniform cost per table, so plain contiguous chunks balance.
 * The stride job's range [lo,hi) is a per-core seed, not an index span: core c
 * takes lo, lo+stride, lo+2*stride, ...  Job parameters ride in the caller's
 * `ud` (see struct sb_job), never in file statics — several threads use the
 * pool and core 0 is preemptive, so a static would let one caller's parameters
 * be overwritten by another's between staging and dispatch.
 *
 * ★ Every kind measures its 1-core baseline with the SAME function it measures
 * in parallel (stride 1 versus stride nc).  Timing one function against a
 * different serial one charges the difference in codegen to "speedup": doing
 * exactly that gave a reproducible x4.42 on 4 cores, which is impossible. */
static long sb_nq_solve(unsigned cols, unsigned d1, unsigned d2, unsigned all)
{
    if (cols == all) return 1;
    long count = 0;
    unsigned avail = ~(cols | d1 | d2) & all;
    while (avail) {
        unsigned bit = avail & (unsigned)(-(long)avail);   /* lowest set bit */
        avail -= bit;
        count += sb_nq_solve(cols | bit, (d1 | bit) << 1, (d2 | bit) >> 1, all);
    }
    return count;
}

/* Per-core sinks: each core writes only its own slot, so the sink that keeps
 * the compiler from folding the loop away costs no cross-core sharing. */
static volatile unsigned sb_din_sink[SMP_NCORES];

static long sb_dining_seats(int np, long t0, long t1, int core)
{
    long meals = 0, t;
    unsigned acc = 2654435761u;
    int round, p;
    for (t = t0; t < t1; t++)
        for (round = 0; round < 24; round++)
            for (p = 0; p < np; p++) {
                int l = p, r = (p + 1) % np;
                int fa = (l < r) ? l : r;
                int fb = (l < r) ? r : l;
                acc = acc * 1103515245u + 12345u + (unsigned)(fa * 131 + fb);
                meals++;
            }
    sb_din_sink[core] ^= acc;
    return meals;
}
static long sb_dining(int np, long tables)
{
    return sb_dining_seats(np, 0, tables, 0);
}

/* ---- Job parameters for the worker pool ----
 * Staged on the CALLER'S stack and handed to the pool via `ud`, never in file
 * globals: core 0 is preemptive and other threads (the AIPL actors) use the
 * same pool, so globals could be overwritten between staging and dispatch. */
struct sb_job {
    int n;        /* board size / prime bound / diners */
    int stride;   /* = number of cores in the job      */
    int c0, c1;   /* nqueens: first-row column window  */
};

/* smp_range_fn: N-Queens over the first-row columns of [j->c0, j->c1) that
 * fall in this core's residue class — c0+lo, c0+lo+stride, ...  The window
 * lets /nqpart (which owns only a slice of the board) share this job with
 * /bench (which owns the whole board, c0=0 c1=n). */
static long sb_nq_par(long lo, long hi, int core, void *ud)
{
    struct sb_job *j = (struct sb_job *)ud;
    int n = j->n, stride = j->stride, c;
    unsigned all = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    long total = 0;
    (void)hi; (void)core;
    for (c = j->c0 + (int)lo; c < j->c1; c += stride) {
        unsigned bit = 1u << c;
        total += sb_nq_solve(bit, bit << 1, bit >> 1, all);
    }
    return total;
}

/* Whole-board N-Queens across the worker pool. */
static long sb_nqueens_smp(int n, int nc)
{
    struct sb_job j;
    j.n = n; j.c0 = 0; j.c1 = n; j.stride = nc;
    return smp_parallel_sum(sb_nq_par, &j, nc, nc);
}

/* N-Queens partial for first-queen columns [c0,c1), across the worker pool.
 * This is the /nqpart cluster route's solver: the Mac hands each board a
 * column slice, and the board now spreads that slice over its own cores. */
long smpbench_nqueens_range(int n, int c0, int c1, int nc)
{
    struct sb_job j;
    if (c0 < 0) c0 = 0;
    if (c1 > n) c1 = n;
    j.n = n; j.c0 = c0; j.c1 = c1; j.stride = nc;
    return smp_parallel_sum(sb_nq_par, &j, nc, nc);
}

/* smp_range_fn: primes over x = lo, lo+stride, lo+2*stride, ... below n. */
static long sb_primes_par(long lo, long hi, int core, void *ud)
{
    struct sb_job *j = (struct sb_job *)ud;
    long n = j->n, stride = j->stride, cnt = 0, x, d;
    (void)hi; (void)core;
    /* Smallest x >= 2 in this core's residue class (x == lo mod stride).
     * Stepping up by stride keeps the classes disjoint; starting at 2 keeps
     * 0 and 1 out of the count. */
    for (x = lo; x < 2; x += stride) { }
    for (; x < n; x += stride) {
        int pr = 1;
        for (d = 2; d * d <= x; d++) if (x % d == 0) { pr = 0; break; }
        cnt += pr;
    }
    return cnt;
}

/* smp_range_fn: dining over the contiguous table range [lo,hi). */
static long sb_dining_par(long lo, long hi, int core, void *ud)
{
    return sb_dining_seats(((struct sb_job *)ud)->n, lo, hi, core);
}

/* ---- Memory-fill benchmark: does bulk *store* work scale on 4 cores? ----
 * This is the question that decides whether the window system could go faster
 * on the worker pool.  Graphics here is not compute-bound: with the D-cache
 * off (SCTLR.C=0) every framebuffer store is its own RAM transaction, so a
 * blit is bound by memory, not by the ALU.  Whether more cores help then
 * depends on which limit binds:
 *   - if the memory controller is already saturated by one core, extra cores
 *     add nothing and the WM cannot be made faster this way;
 *   - if a single core is instead stalling on store latency, extra cores
 *     overlap those stalls and bulk drawing scales.
 * Only the hardware can answer, so measure it on a plain buffer, which has
 * exactly the framebuffer's memory attributes (Normal, non-cacheable while
 * C=0) without disturbing the live display.
 *
 * The buffer is 1 MB (~1/4 of a 1280x800x32 screen), but the job size is the
 * caller's `n` words, so sweeping n traces the scaling curve and shows how big
 * a drawing job must be before splitting it pays.  SB_FILL_PASSES keeps even
 * small n well above the 1 us clock resolution. */
#define SB_FILL_WORDS  (256 * 1024)
#define SB_FILL_PASSES 8
static unsigned sb_fill_buf[SB_FILL_WORDS];

static long sb_fill_par(long lo, long hi, int core, void *ud)
{
    volatile unsigned *b = sb_fill_buf;
    long i, p;
    (void)core; (void)ud;
    for (p = 0; p < SB_FILL_PASSES; p++)
        for (i = lo; i < hi; i++) b[i] = (unsigned)(i + p);
    return hi - lo;
}

/* smp_range_fn that does nothing: times the pool's own dispatch + collect
 * cost.  That figure is the break-even point — any job shorter than it gets
 * SLOWER when handed to the workers, which is exactly what decides whether
 * the window system's small redraws are worth parallelising at all. */
static long sb_null_par(long lo, long hi, int core, void *ud)
{
    (void)core; (void)ud;
    return hi - lo;
}


/* ======================================================================
 *  Front-end: run one workload serially, then in parallel, and report.
 * ====================================================================== */

int smpbench_kind_of(const char *req)
{
    if (NULL != strstr(req, "kind=dining")) return SMPBENCH_DINING;
    if (NULL != strstr(req, "kind=primes")) return SMPBENCH_PRIMES;
    if (NULL != strstr(req, "kind=fill"))   return SMPBENCH_FILL;
    if (NULL != strstr(req, "kind=null"))   return SMPBENCH_NULL;
    return SMPBENCH_NQUEENS;
}

void smpbench_run(int kind, int n, struct smpbench_result *out)
{
    struct sb_job job;
    unsigned long t0;
    int nc = smp_cores_online();

    out->cores = nc;
    out->label = "?"; out->metric = "?";
    out->value = out->value_par = 0;
    out->us_1  = out->us_n = 0;

    switch (kind) {
    case SMPBENCH_DINING: {
        int np = n ? n : 5;
        if (np < 2)  np = 2;
        if (np > 64) np = 64;
        job.n = np; job.stride = 1;
        t0 = clkcount();
        out->value = sb_dining(np, 40000L);
        out->us_1 = clkcount() - t0;
        t0 = clkcount();                     /* uniform per table: contiguous */
        out->value_par = smp_parallel_sum(sb_dining_par, &job, 40000L, nc);
        out->us_n = clkcount() - t0;
        out->label = "dining"; out->metric = "meals";
        break;
    }
    case SMPBENCH_PRIMES: {
        int pn = n ? n : 200000;
        if (pn < 1) pn = 1;
        job.n = pn;
        job.stride = 1;
        t0 = clkcount();
        out->value = sb_primes_par(0, 0, 0, &job);
        out->us_1 = clkcount() - t0;
        job.stride = nc;                     /* interleave: cost grows with x */
        t0 = clkcount();
        out->value_par = smp_parallel_sum(sb_primes_par, &job, nc, nc);
        out->us_n = clkcount() - t0;
        out->label = "primes"; out->metric = "primes";
        break;
    }
    case SMPBENCH_FILL: {
        long w = n ? (long)n : SB_FILL_WORDS;
        if (w < 1) w = 1;
        if (w > SB_FILL_WORDS) w = SB_FILL_WORDS;
        t0 = clkcount();
        out->value = sb_fill_par(0, w, 0, &job);
        out->us_1 = clkcount() - t0;
        t0 = clkcount();                     /* uniform per word: contiguous */
        out->value_par = smp_parallel_sum(sb_fill_par, &job, w, nc);
        out->us_n = clkcount() - t0;
        out->label = "fill"; out->metric = "words";
        break;
    }
    case SMPBENCH_NULL:
        t0 = clkcount();
        out->value = sb_null_par(0, nc, 0, &job);
        out->us_1 = clkcount() - t0;
        t0 = clkcount();
        out->value_par = smp_parallel_sum(sb_null_par, &job, nc, nc);
        out->us_n = clkcount() - t0;         /* = the pool's round trip */
        out->label = "null"; out->metric = "chunks";
        break;
    default: {
        int bn = n ? n : 11;
        if (bn < 1)  bn = 1;
        if (bn > 13) bn = 13;                /* keep it snappy even on 1 core */
        /* ★ The 1-core baseline MUST be this same sb_nq_par, with stride 1, not
         * the standalone sb_nqueens().  They compute the same thing, but they
         * are different functions and the compiler treats them differently —
         * measuring one against the other charges the difference in codegen to
         * "parallel speedup".  That produced a reproducible x4.42 on 4 cores,
         * which is impossible, and moving the code between files shifted it
         * from x3.70 to x4.42 without touching any logic.  Same function on
         * both sides, and the only variable left is the core count. */
        job.n = bn; job.c0 = 0; job.c1 = bn; job.stride = 1;
        t0 = clkcount();
        out->value = smp_parallel_sum(sb_nq_par, &job, 1, 1);
        out->us_1 = clkcount() - t0;
        t0 = clkcount();
        out->value_par = sb_nqueens_smp(bn, nc);  /* interleaved: centre
                                                   * columns cost more */
        out->us_n = clkcount() - t0;
        out->label = "nqueens"; out->metric = "solutions";
        break;
    }
    }

    out->agree = (out->value == out->value_par);
    out->speedup_x100 = out->us_n ? (int)((out->us_1 * 100UL) / out->us_n) : 0;
}

/* ======================================================================
 *  Boot harness (DCACHE_EXPERIMENT): no network, so report to the console.
 * ====================================================================== */

/* Print speedup as a ratio with 2 decimals without pulling in floating point:
 * kprintf here has no %f, and the kernel is built soft-float anyway. */
static void sb_print_row(const char *name, const struct smpbench_result *r)
{
    kprintf("  %-14s %10lu %10lu   x%d.%02d  %s\r\n",
            name, r->us_1, r->us_n,
            r->speedup_x100 / 100, r->speedup_x100 % 100,
            r->agree ? "ok" : "DISAGREE");
}

void smpbench_run_all(void)
{
    struct smpbench_result r;
    int i;

    /* The fill sweep answers the question this harness exists for: does bulk
     * memory-store work scale once it can be cached?  With the D-cache off it
     * does not (x0.95-x1.35 measured), which is why the window system was left
     * alone.  Sweeping the size also shows where the pool's round trip stops
     * dominating. */
    static const int fill_sizes[] = { 64, 256, 1024, 4096, 16384, 65536, 262144 };

    kprintf("\r\n===== smpbench =====\r\n");
    kprintf("cores_online = %d\r\n", smp_cores_online());
#ifdef DCACHE_ON
    kprintf("D-cache      = ON   (SCTLR.C=1, SMPEN set)\r\n");
#else
    kprintf("D-cache      = OFF  (SCTLR.C=0)\r\n");
#endif
    kprintf("sctlr        = 0x%08x\r\n", mmu_read_sctlr());
    kprintf("★ compare RATIOS, not absolute us: the firmware moves the ARM\r\n"
            "  clock (first run after boot is ~2.33x faster). The serial and\r\n"
            "  parallel halves run back-to-back, so the ratio is stable.\r\n");

    kprintf("\r\n-- compute-bound (expect x3.2-x4.0) --\r\n");
    kprintf("  %-14s %10s %10s %8s\r\n", "bench", "1-core us", "N-core us", "speedup");
    smpbench_run(SMPBENCH_DINING, 5, &r);       sb_print_row("dining n=5", &r);
    smpbench_run(SMPBENCH_NQUEENS, 11, &r);     sb_print_row("nqueens n=11", &r);
    smpbench_run(SMPBENCH_NQUEENS, 12, &r);     sb_print_row("nqueens n=12", &r);
    smpbench_run(SMPBENCH_PRIMES, 200000, &r);  sb_print_row("primes 200k", &r);

    kprintf("\r\n-- memory-store-bound: THE QUESTION (x1.0 = no benefit) --\r\n");
    kprintf("  %-14s %10s %10s %8s\r\n", "words", "1-core us", "N-core us", "speedup");
    for (i = 0; i < (int)(sizeof(fill_sizes) / sizeof(fill_sizes[0])); i++) {
        char nm[16];
        smpbench_run(SMPBENCH_FILL, fill_sizes[i], &r);
        sprintf(nm, "fill %d", fill_sizes[i]);
        sb_print_row(nm, &r);
    }

    kprintf("\r\n-- pool round trip (the parallelise/don't break-even) --\r\n");
    smpbench_run(SMPBENCH_NULL, 0, &r);         sb_print_row("null", &r);

    kprintf("\r\n===== end smpbench =====\r\n");
}
