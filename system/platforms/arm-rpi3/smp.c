/**
 * @file smp.c
 *
 * Worker-pool SMP bring-up + lock-free parallel dispatch for BCM2837
 * (Pi 3, 4x Cortex-A53, AArch32).  See include/smp.h for the rationale.
 *
 * This is the ARMv7/32-bit sibling of the Pi 4's AArch64 system/smp.c.  The
 * dispatch half is identical; only the core-identification and bring-up
 * halves differ:
 *
 *   Pi 4 (AArch64)          Pi 3 (AArch32, this file)
 *   ----------------        -------------------------
 *   mrs x, mpidr_el1        mrc p15, 0, r, c0, c0, 5
 *   spin table @ 0xe0..     ARM-local mailbox 3 @ 0x4000008C + 0x10*core
 *   PSCI / armstub8         armstub7
 */

#include <smp.h>
#include <interrupt.h>   /* disable()/restore() — see smp_pool_acquire() */

/* ---- start.S handoff -------------------------------------------------
 * These live in start.S's .data (NOT .bss) on purpose: a secondary core may
 * already be polling smp_release[] while core 0 is still clearing .bss, and
 * it must never see a stale non-zero release address.  .data is initialised
 * straight from the image, so both arrays read 0 from the instant the GPU
 * loads the kernel. */
extern volatile unsigned long smp_release[SMP_NCORES];   /* entry addr per core */
extern volatile unsigned long smp_stacktop[SMP_NCORES];  /* initial SP per core */
extern void _smp_start(void);                            /* start.S trampoline  */

/* Each secondary core's stack.  Static, so bring-up has no heap dependency. */
#define SMP_STACK_BYTES 16384
static unsigned char smp_stack[SMP_NCORES][SMP_STACK_BYTES] __attribute__((aligned(8)));

/* Online flags + the per-core job mailbox.  All cross-core, all volatile;
 * coherent without locks because the D-cache is off (every access hits RAM). */
static volatile int          smp_online[SMP_NCORES];
static volatile smp_range_fn smp_job_fn[SMP_NCORES];
static void * volatile       smp_job_ud[SMP_NCORES];
static volatile long         smp_job_lo[SMP_NCORES];
static volatile long         smp_job_hi[SMP_NCORES];
static volatile long         smp_job_res[SMP_NCORES];
static volatile int          smp_job_seq[SMP_NCORES];    /* bumped to post a job */
static volatile int          smp_job_done[SMP_NCORES];   /* == seq when finished */

/* Bound on how long core 0 waits for a worker before taking the chunk over
 * itself.  Far longer than any real chunk, so it only trips on a genuinely
 * dead core — at which point the job still completes, just serially. */
#define SMP_WAIT_LIMIT 2000000000UL

/* Bring-up wait: a released core announces itself within microseconds, so cap
 * this short.  A core that misses the window is treated as offline and boot
 * proceeds — never a multi-second stall on the boot path. */
#define SMP_BRINGUP_WAIT 20000000UL

/* ARM-local per-core mailbox 3 SET register.  The 32-bit Pi 3 firmware leaves
 * cores 1-3 spinning in armstub7 on exactly this word; writing a jump target
 * and issuing SEV releases the core to that address.  This MMIO page is
 * already Device-mapped by mmu.c (0x40000000..0x40FFFFFF). */
#define ARM_LOCAL_MBOX3_SET(core) \
    ((volatile unsigned long *)(0x4000008CUL + 0x10UL * (unsigned long)(core)))

static inline void dsb_sev(void) { __asm__ volatile("dsb\n\tsev" ::: "memory"); }
static inline void dsb(void)     { __asm__ volatile("dsb" ::: "memory"); }

int smp_core_id(void)
{
    unsigned long m;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(m));
    return (int)(m & 3);
}

/* The worker idle loop: wait (low-power) for a new job, run it, signal done. */
static void smp_worker_loop(int core)
{
    int last = smp_job_seq[core];
    for (;;) {
        while (smp_job_seq[core] == last) __asm__ volatile("wfe");
        last = smp_job_seq[core];
        smp_range_fn fn = smp_job_fn[core];
        long r = fn ? fn(smp_job_lo[core], smp_job_hi[core], core,
                         smp_job_ud[core]) : 0;
        smp_job_res[core] = r;
        dsb();                           /* publish the result ...            */
        smp_job_done[core] = last;       /* ... before announcing completion  */
        dsb_sev();                       /* wake core 0 out of its wait spin  */
    }
}

/* C entry for a freshly-started secondary core, called from start.S with its
 * stack set, in SVC mode, IRQ+FIQ masked.  Match core 0's MMU/cache config so
 * timings are comparable, announce online, then idle as a worker.
 *
 * No exception vectors are installed: a worker runs pure compute with
 * interrupts masked, and the BCM2837 routes GPU IRQs to core 0 only.  If a
 * worker ever faults it is a kernel bug — it parks in the dead loop below
 * rather than re-entering core 0's non-reentrant handlers. */
void smp_secondary_entry(int core)
{
    extern void mmu_enable_secondary(void);  /* MMU on, I-cache on, D-cache off */

    if (core <= 0 || core >= SMP_NCORES) { for (;;) __asm__ volatile("wfe"); }
    mmu_enable_secondary();
    smp_online[core] = 1;
    dsb_sev();                               /* tell core 0 we are up */
    smp_worker_loop(core);                   /* never returns */
}

void smp_init(void)
{
    int c;

    smp_online[0] = 1;                       /* core 0 is obviously up */
    for (c = 1; c < SMP_NCORES; c++) {
        smp_job_seq[c]  = 0;
        smp_job_done[c] = 0;
        smp_stacktop[c] = (unsigned long)(smp_stack[c] + SMP_STACK_BYTES);
    }
    dsb();                                   /* stacks visible before release */

    /* Release each secondary via BOTH mechanisms, then SEV:
     *   (a) the armstub spin-table mailbox — how the stock 32-bit Pi 3
     *       firmware actually holds cores 1-3, and
     *   (b) smp_release[], which start.S's own park_secondary path polls — in
     *       case a firmware instead drops all four cores into _start.
     * Whichever path a core is on, it converges on _smp_start. */
    for (c = 1; c < SMP_NCORES; c++) {
        smp_release[c] = (unsigned long)&_smp_start;
        *ARM_LOCAL_MBOX3_SET(c) = (unsigned long)&_smp_start;
    }
    dsb_sev();

    /* Wait (bounded) for each to announce itself online. */
    for (c = 1; c < SMP_NCORES; c++) {
        unsigned long spins = 0;
        while (!smp_online[c] && ++spins < SMP_BRINGUP_WAIT)
            __asm__ volatile("nop");
    }
}

int smp_cores_online(void)
{
    int n = 0, c;
    for (c = 0; c < SMP_NCORES; c++) if (smp_online[c]) n++;
    return n;
}

/* Post [lo,hi) to worker `c`, or run it inline on core 0 if that core is
 * offline.  Returns 1 if the job was posted (so it must be collected later). */
static int smp_post(int c, smp_range_fn fn, void *ud, long lo, long hi,
                    long *inline_total)
{
    if (!smp_online[c]) { *inline_total += fn(lo, hi, 0, ud); return 0; }
    smp_job_fn[c] = fn;
    smp_job_ud[c] = ud;
    smp_job_lo[c] = lo;
    smp_job_hi[c] = hi;
    dsb();                       /* job fields visible ...        */
    smp_job_seq[c]++;            /* ... before the job is armed   */
    dsb_sev();                   /* wake the worker               */
    return 1;
}

/* Collect worker `c`, taking the chunk over on core 0 if it never finishes. */
static long smp_collect(int c, smp_range_fn fn, void *ud, long lo, long hi)
{
    unsigned long spins = 0;
    while (smp_job_done[c] != smp_job_seq[c]) {
        if (++spins >= SMP_WAIT_LIMIT) return fn(lo, hi, 0, ud); /* worker stuck */
        __asm__ volatile("nop");
    }
    return smp_job_res[c];
}

/* ---- Serialising the pool ------------------------------------------------
 * There is ONE job mailbox, so two callers driving the workers at the same
 * time would overwrite each other's jobs.  Core 0 is preemptive (clkhandler
 * calls resched), so a thread CAN be preempted mid-job and another thread can
 * reach smp_parallel_sum — the AIPL actor threads make this a real case, not a
 * theoretical one.
 *
 * Rather than block the loser (a semaphore here would invert against the
 * webactor thread and needs an init order smp_init() runs too early for), a
 * caller that finds the pool busy simply runs its whole job inline.  Always
 * correct, never deadlocks, and costs only the parallel speedup — which the
 * loser would not have got anyway, since the cores are already busy.
 *
 * The flag is core-0-only state: workers never touch it. */
static volatile int smp_busy;

static int smp_pool_acquire(void)
{
    irqmask im = disable();
    if (smp_busy) { restore(im); return 0; }
    smp_busy = 1;
    restore(im);
    return 1;
}

static void smp_pool_release(void) { smp_busy = 0; }

long smp_parallel_sum_bounds(smp_range_fn fn, void *ud, const long *bounds,
                             int ncores)
{
    long total = 0;
    int c;
    int posted[SMP_NCORES];

    if (ncores < 1) ncores = 1;
    if (ncores > SMP_NCORES) ncores = SMP_NCORES;

    if (!smp_pool_acquire()) {
        /* Pool in use — run every chunk here.  Note this walks the chunks one
         * by one rather than collapsing them into fn(bounds[0], bounds[ncores]):
         * a strided job reads `lo` as its residue class, not as a range start,
         * so only the per-chunk calls reproduce the full workload. */
        for (c = 0; c < ncores; c++) total += fn(bounds[c], bounds[c + 1], 0, ud);
        return total;
    }

    /* Post chunks 1..ncores-1 to the worker cores. */
    for (c = 1; c < ncores; c++)
        posted[c] = smp_post(c, fn, ud, bounds[c], bounds[c + 1], &total);

    /* Core 0 runs chunk 0 inline while the workers run theirs. */
    total += fn(bounds[0], bounds[1], 0, ud);

    /* Collect the workers. */
    for (c = 1; c < ncores; c++)
        if (posted[c]) total += smp_collect(c, fn, ud, bounds[c], bounds[c + 1]);

    smp_pool_release();
    return total;
}

long smp_parallel_sum(smp_range_fn fn, void *ud, long n, int ncores)
{
    long bounds[SMP_NCORES + 1];
    long chunk;
    int c;

    if (ncores < 1) ncores = 1;
    if (ncores > SMP_NCORES) ncores = SMP_NCORES;
    if (n < 0) n = 0;

    chunk = n / ncores;
    for (c = 0; c < ncores; c++) bounds[c] = (long)c * chunk;
    bounds[ncores] = n;                      /* last chunk absorbs the remainder */

    return smp_parallel_sum_bounds(fn, ud, bounds, ncores);
}
