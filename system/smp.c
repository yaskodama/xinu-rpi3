/**
 * @file smp.c
 *
 * Worker-pool SMP bring-up + lock-free parallel dispatch for the Pi 3 B+
 * (BCM2837, Cortex-A53 x4), AArch32.  Ported from the AArch64 xinu-rpi4/5
 * design; the coherency model is simpler here because every core already runs
 * with the D-cache OFF (SCTLR.C=0 — see system/platforms/arm-rpi3/mmu.c), so
 * the lock-free mailbox needs no clean/invalidate maintenance.
 *
 * Bring-up path: cores 1-3 are parked by loader/platforms/arm-rpi3/start.S,
 * polling smp_release[core].  smp_init() publishes _smp_start there (and, as a
 * belt-and-braces fallback for firmware that spin-tables the cores instead,
 * pokes the ARM-local mailbox 3 set register), then SEVs.  A released core
 * lands in _smp_start (start.S), sets its per-core stack, and calls
 * smp_secondary_entry() below.
 */

#include <smp.h>

extern void _smp_start(void);            /* AArch32 trampoline in start.S      */
extern void mmu_enable_secondary(void);  /* MMU on / I-cache on / D-cache off  */

/* SMP handoff tables.  Global (start.S reads smp_release/smp_stacktop).  Zero-
 * initialised, so a parked core polling smp_release never sees a stale entry. */
volatile unsigned long smp_release[SMP_NCORES];    /* entry addr per core */
volatile unsigned long smp_stacktop[SMP_NCORES];   /* initial SP per core */

#define SMP_STACK_BYTES 16384
static unsigned char smp_stack[SMP_NCORES][SMP_STACK_BYTES] __attribute__((aligned(16)));

/* Online flags + the per-core job mailbox.  All cross-core, all volatile. */
static volatile int          smp_online[SMP_NCORES];
static volatile smp_range_fn smp_job_fn[SMP_NCORES];
static volatile long         smp_job_lo[SMP_NCORES];
static volatile long         smp_job_hi[SMP_NCORES];
static volatile long         smp_job_res[SMP_NCORES];
static volatile int          smp_job_seq[SMP_NCORES];    /* bumped to post a job */
static volatile int          smp_job_done[SMP_NCORES];   /* == seq when finished */

/* Core 0 takes a stuck worker's chunk over itself after this many spins
 * (~2e9 ≈ a few seconds, far longer than any real chunk). */
#define SMP_WAIT_LIMIT    2000000000UL
/* Bounded wait for a secondary to announce itself online (~1-2 s). */
#define SMP_BRINGUP_WAIT  100000000UL

static inline void dsb_sev(void) { __asm__ volatile("dsb\n\tsev" ::: "memory"); }
static inline void dsb(void)     { __asm__ volatile("dsb" ::: "memory"); }

int smp_core_id(void)
{
    unsigned long m;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(m));   /* MPIDR */
    return (int)(m & 3);
}

/* Worker idle loop: wait (low-power WFE) for a new job, run it, signal done. */
static void smp_worker_loop(int core)
{
    int last = smp_job_seq[core];
    for (;;) {
        while (smp_job_seq[core] == last) { __asm__ volatile("wfe"); }
        last = smp_job_seq[core];
        smp_range_fn fn = smp_job_fn[core];
        long r = fn ? fn(smp_job_lo[core], smp_job_hi[core], core) : 0;
        smp_job_res[core]  = r;
        smp_job_done[core] = last;
        dsb_sev();                         /* wake core 0 out of its collect spin */
    }
}

/* C entry for a released secondary core (called from _smp_start). */
void smp_secondary_entry(int core)
{
    mmu_enable_secondary();                /* same map as core 0; D-cache stays off */
    smp_online[core] = 1;
    dsb();
    smp_worker_loop(core);                 /* never returns */
}

void smp_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;

    smp_online[0] = 1;                      /* core 0 is obviously up */
    for (int c = 1; c < SMP_NCORES; c++) {
        smp_job_seq[c]  = 0;
        smp_job_done[c] = 0;
        smp_stacktop[c] = (unsigned long)(smp_stack[c] + SMP_STACK_BYTES);
    }
    dsb();

    /* Release each secondary via BOTH mechanisms, then SEV:
     *   (a) smp_release[] — the parked core in start.S polls it, and
     *   (b) the ARM-local mailbox 3 set register (0x4000008C + 0x10*core),
     *       in case this firmware spin-tables the cores instead of dropping
     *       them into the kernel _start. */
    for (int c = 1; c < SMP_NCORES; c++) {
        smp_release[c] = (unsigned long)&_smp_start;
        volatile unsigned long *mbox =
            (volatile unsigned long *)(0x4000008CUL + 0x10UL * (unsigned long)c);
        *mbox = (unsigned long)&_smp_start;
    }
    dsb_sev();

    /* Wait (bounded) for each to announce itself online. */
    for (int c = 1; c < SMP_NCORES; c++) {
        unsigned long spins = 0;
        while (!smp_online[c] && ++spins < SMP_BRINGUP_WAIT) { __asm__ volatile("nop"); }
    }
}

int smp_cores_online(void)
{
    int n = 0;
    for (int c = 0; c < SMP_NCORES; c++) if (smp_online[c]) n++;
    return n;
}

long smp_parallel_sum(smp_range_fn fn, long n, int ncores)
{
    if (ncores < 1) ncores = 1;
    if (ncores > SMP_NCORES) ncores = SMP_NCORES;
    if (n < 0) n = 0;

    long chunk = n / ncores;
    long total = 0;

    /* Post chunks 1..ncores-1 to the worker cores (offline ones are computed
     * by core 0 inline). */
    for (int c = 1; c < ncores; c++) {
        long lo = (long)c * chunk;
        long hi = (c == ncores - 1) ? n : lo + chunk;
        if (!smp_online[c]) { total += fn(lo, hi, 0); continue; }
        smp_job_fn[c] = fn;
        smp_job_lo[c] = lo;
        smp_job_hi[c] = hi;
        dsb();
        smp_job_seq[c]++;                  /* arm the job, then wake the worker */
        dsb_sev();
    }

    /* Core 0 runs chunk 0 inline while the workers run theirs. */
    total += fn(0, (ncores == 1) ? n : chunk, 0);

    /* Collect the workers, taking over any that did not finish in time. */
    for (int c = 1; c < ncores; c++) {
        if (!smp_online[c]) continue;
        unsigned long spins = 0;
        while (smp_job_done[c] != smp_job_seq[c]) {
            if (++spins >= SMP_WAIT_LIMIT) {
                long lo = (long)c * chunk;
                long hi = (c == ncores - 1) ? n : lo + chunk;
                smp_job_res[c] = fn(lo, hi, 0);
                break;
            }
            __asm__ volatile("nop");
        }
        total += smp_job_res[c];
    }
    return total;
}
