/**
 * @file mmu.c
 *
 * ARMv7-A short-descriptor identity-mapped MMU for BCM2837 (Pi 3 B+).
 *
 * Builds ONE 16 KB L1 page table covering all 4 GB of virtual address
 * space in 1 MB sections.  VA == PA (identity map); attributes vary
 * by region:
 *
 *   0x00000000 .. 0x3EFFFFFF  RAM (≤1 GB)   Normal write-back, RWX
 *   0x3F000000 .. 0x3FFFFFFF  BCM2835 perips Device, RW, no-execute
 *   0x40000000 .. 0x40FFFFFF  ARM local regs Device, RW, no-execute
 *   0x41000000 .. 0xFFFFFFFF  unused          fault (invalid descriptor)
 *
 * RAM is left RWX deliberately — the cc_mvp JIT memget()s buffers
 * from this region and executes the bytes in place.  Marking the
 * data heap NX would require either a JIT-specific RWX arena or a
 * page-protection flip per compile.  Future stage.
 *
 * Enable sequence per ARM ARM B3.10.3:
 *   1. Set TTBR0 to L1 table base (must be 16 KB-aligned)
 *   2. Set TTBCR.N = 0 so we use only TTBR0 for the whole 32-bit VA
 *   3. Set DACR — domain 0 = client (01) so permissions apply
 *   4. Invalidate TLB
 *   5. DSB + ISB so writes drained, pipeline cleared
 *   6. SCTLR.M = 1 (MMU on, caches LEFT AS-IS)
 *   7. ISB so the next instruction fetch uses MMU-translated PA
 *
 * Region attributes (TEX/C/B/XN) in the page-table descriptors are enforced by
 * the MMU regardless of cache state — so MMIO is strongly-ordered Device
 * memory either way.
 *
 * === D-cache: OFF by default, ON under DCACHE_ON ==========================
 * The default build sets only SCTLR.M + SCTLR.I and leaves the D-cache (C)
 * OFF.  That is what the rest of the kernel is built on: it is why smp.c can
 * coordinate the worker cores with plain volatile + dsb, why screenInit can
 * hand the GPU a stack-local struct, and why the USB driver can DMA to
 * unaligned heap and stack buffers without any maintenance.
 *
 * -DDCACHE_ON turns the D-cache on.  Enabling C=1 naively is NOT safe and
 * bricked the Pi 3 on an earlier attempt: the caches hold garbage out of
 * reset, so it must be preceded by a set/way invalidate (cache.c), and on the
 * A53 the shareable mappings below are not coherent between cores until
 * CPUECTLR.SMPEN is set.  Both are done in mmu_init/mmu_enable_secondary.
 * The framebuffer is re-mapped non-cacheable afterwards (the GPU never snoops
 * us), and mmu_disable now cleans before turning the cache off.
 *
 * ★ DCACHE_ON is only sound in combination with -DDCACHE_EXPERIMENT, which
 * removes the one agent that cannot be fixed this way: the DWC USB DMA
 * engine, which is handed unaligned heap and even stack pointers.  See
 * docs/smp_report and compile/Makefile.
 *
 * Call exactly once, late in platforminit() after RAM bounds are known.
 * Idempotent guard avoids double-init if anyone re-enters.
 */

#include <stddef.h>
#include <stdint.h>
#include <cache.h>

#define L1_ENTRIES 4096

/* === L1 section descriptor flags (ARMv7-A short descriptor) ===
 * Bit layout:
 *   [31:20] PA[31:20]   1 MB-aligned physical base
 *   [18]    0           section (1 = supersection)
 *   [17]    nG          non-global (0 = global)
 *   [16]    S           shareable
 *   [15]    AP[2]
 *   [14:12] TEX[2:0]
 *   [11:10] AP[1:0]
 *   [8:5]   Domain
 *   [4]     XN          execute-never
 *   [3]     C           cacheable
 *   [2]     B           bufferable
 *   [1:0]   10          section descriptor type
 *
 * AP[2:0] = 011  → privileged + user RW
 * TEX=001, C=1, B=1 → Normal, write-back, write-allocate, cacheable
 * TEX=000, C=0, B=1 → Device, shareable
 */
#define SECT_DESC       0x2u
#define SECT_AP_RW      (3u << 10)
#define SECT_DOMAIN0    (0u << 5)
#define SECT_XN         (1u << 4)
#define SECT_C          (1u << 3)
#define SECT_B          (1u << 2)
#define SECT_TEX_NORM   (1u << 12)
#define SECT_TEX_DEV    (0u << 12)
#define SECT_S          (1u << 16)

#define ATTR_RAM  (SECT_DESC | SECT_AP_RW | SECT_DOMAIN0 | \
                   SECT_TEX_NORM | SECT_C | SECT_B | SECT_S)
#define ATTR_MMIO (SECT_DESC | SECT_AP_RW | SECT_DOMAIN0 | \
                   SECT_TEX_DEV  | SECT_B | SECT_XN | SECT_S)

/* TEX=001, C=0, B=0 -> Normal, Inner AND Outer Non-cacheable.  For memory the
 * CPU shares with a non-coherent agent (the GPU's framebuffer).  Normal rather
 * than Device on purpose: Device would forbid the write buffer from merging
 * neighbouring pixel stores and would make unaligned accesses fault, so it
 * would cost far more than it needs to. */
#define ATTR_NC   (SECT_DESC | SECT_AP_RW | SECT_DOMAIN0 | \
                   SECT_TEX_NORM | SECT_S)

/* L1 table — 4096 word-sized entries = 16 KB.  Alignment is critical:
 * TTBR0 holds bits [31:14] of the base, so it MUST be 16 KB-aligned. */
static uint32_t __attribute__((aligned(16384))) l1_table[L1_ENTRIES];

static int mmu_enabled = 0;

/* Read SCTLR for /api/mmu introspection. */
uint32_t mmu_read_sctlr(void)
{
    uint32_t v;
    asm volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r" (v));
    return v;
}

uint32_t mmu_read_ttbr0(void)
{
    uint32_t v;
    asm volatile ("mrc p15, 0, %0, c2, c0, 0" : "=r" (v));
    return v;
}

uint32_t mmu_table_base(void) { return (uint32_t)(unsigned long)l1_table; }

int mmu_is_enabled(void) { return mmu_enabled; }

void mmu_init(void)
{
    if (mmu_enabled) return;

    /* Build identity-mapped L1 table.  Iterate 4096 sections, pick
     * RAM vs MMIO attributes by VA range. */
    int i;
    for (i = 0; i < L1_ENTRIES; i++) {
        uint32_t pa = (uint32_t)i << 20;
        if (pa < 0x3F000000u) {
            /* RAM region — Normal cacheable RWX.  Pi 3 has 1 GB so
             * the first 1008 MB are RAM. */
            l1_table[i] = pa | ATTR_RAM;
        } else if (pa < 0x40000000u) {
            /* BCM2835 peripherals (UART, mailbox, EMMC, USB, etc.) */
            l1_table[i] = pa | ATTR_MMIO;
        } else if (pa < 0x41000000u) {
            /* ARM-local regs (per-core mailbox FIFOs, generic timer) */
            l1_table[i] = pa | ATTR_MMIO;
        } else {
            /* Untranslated — leave as invalid (0).  Any access faults. */
            l1_table[i] = 0;
        }
    }

#ifdef DCACHE_ON
    /* ★ ORDER IS LOAD-BEARING.  Both of these must happen BEFORE SCTLR.C is
     * set below:
     *   - The caches hold garbage out of reset, not zeroes.  Enabling the
     *     D-cache without invalidating first tells the core that garbage is
     *     valid data — this is the exact hazard that bricked the earlier
     *     attempt described at the top of this file.  Invalidate (not clean:
     *     there is nothing worth writing back, and writing garbage back would
     *     be worse).
     *   - On the A53, the Normal-cacheable-SHAREABLE mapping we build above is
     *     not actually coherent between cores until CPUECTLR.SMPEN is set, and
     *     the TRM requires SMPEN precede the caches/MMU coming up.
     * Note the L1 table itself was built with the D-cache still off, so it is
     * already in RAM where the (non-cacheable) table walker will find it. */
    dcache_invalidate_all();
    smpen_set();
#endif

    /* (1) TTBR0 = table base.  Low bits of TTBR0 are RGN / IRGN /
     * shareability hints; for a simple identity map without
     * inner/outer cacheable page-table walks we leave them 0.
     * (2) TTBCR.N = 0 (use TTBR0 for the whole VA).
     * (3) DACR — all 16 domains = client (0b01 in each 2-bit field).
     * (4-5) Invalidate TLB, I-cache, branch predictor; DSB/ISB.
     * (6) SCTLR: enable MMU + caches + branch prediction.
     * (7) ISB so the very next instruction fetch is through MMU.
     */
    asm volatile (
        /* (1) TTBR0 */
        "mcr p15, 0, %0, c2, c0, 0\n"
        /* (2) TTBCR = 0 */
        "mov r1, #0\n"
        "mcr p15, 0, r1, c2, c0, 2\n"
        /* (3) DACR — load 0x55555555 (all client) via 16-bit halves */
        "movw r1, #0x5555\n"
        "movt r1, #0x5555\n"
        "mcr p15, 0, r1, c3, c0, 0\n"
        /* (4) Invalidate TLB (entire) + I-cache (ICIALLU) + BP */
        "mov r1, #0\n"
        "mcr p15, 0, r1, c8, c7, 0\n"   /* TLBIALL */
        "mcr p15, 0, r1, c7, c5, 0\n"   /* ICIALLU — invalidate I-cache */
        "mcr p15, 0, r1, c7, c5, 6\n"   /* BPIALL — invalidate branch pred */
        /* (5) DSB + ISB so all invalidations + table writes drain */
        "dsb\n"
        "isb\n"
        /* (6) SCTLR — enable MMU (M) + I-cache (I).  D-cache (C) is LEFT
         * OFF: enabling it would break the USB-ethernet / framebuffer DMA
         * paths (which hand uncached bus aliases to the GPU/DMA) without
         * cache-maintenance.  I-cache has no such DMA hazard and speeds up
         * compute (e.g. PBKDF2).  ★ self-modifying code (cc_mvp JIT) must
         * ICIALLU after writing instructions now that I-cache is on. */
        "mrc p15, 0, r1, c1, c0, 0\n"
        "orr r1, r1, #(1 << 0)\n"     /* M = MMU enable */
        "orr r1, r1, #(1 << 12)\n"    /* I = instruction cache enable */
#ifdef DCACHE_ON
        "orr r1, r1, #(1 << 2)\n"     /* C = data cache enable (DCACHE_ON) */
#endif
        "mcr p15, 0, r1, c1, c0, 0\n"
        /* (7) ISB so the next fetch sees MMU on */
        "isb\n"
        :
        : "r" (l1_table)
        : "r1", "memory"
    );

    mmu_enabled = 1;
}

/* Bring a secondary core's MMU/cache configuration in line with core 0's:
 * point TTBR0 at the SAME identity-mapped L1 table core 0 already built, then
 * enable MMU + I-cache (D-cache stays off, as on core 0).
 *
 * A worker core must never rebuild the table — it only borrows it.  Sharing is
 * safe: the map is identity and read-only after mmu_init(), and page-table
 * walks are non-cacheable here (TTBR0 RGN/IRGN = 0), so no walker coherency
 * problem arises between cores.
 *
 * Matching core 0 exactly matters for more than tidiness: the whole worker
 * pool assumes D-cache off for its lock-free job mailbox (see smp.h), and
 * equal cache config is what makes a 4-core timing comparison honest.
 *
 * Called from smp_secondary_entry() with the MMU still off.  Enabling it
 * mid-function is safe because VA == PA. */
void mmu_enable_secondary(void)
{
#ifdef DCACHE_ON
    /* Same ordering rule as mmu_init: this core's caches also hold reset
     * garbage, and it must join the coherency domain before its cache comes
     * up — otherwise it would cache the shared job mailbox privately and
     * smp.c's volatile+dsb protocol would break silently between cores. */
    dcache_invalidate_all();
    smpen_set();
#endif
    asm volatile (
        /* (1) TTBR0 = core 0's table base */
        "mcr p15, 0, %0, c2, c0, 0\n"
        /* (2) TTBCR = 0 */
        "mov r1, #0\n"
        "mcr p15, 0, r1, c2, c0, 2\n"
        /* (3) DACR — all 16 domains = client */
        "movw r1, #0x5555\n"
        "movt r1, #0x5555\n"
        "mcr p15, 0, r1, c3, c0, 0\n"
        /* (4) Invalidate TLB + I-cache + branch predictor on THIS core */
        "mov r1, #0\n"
        "mcr p15, 0, r1, c8, c7, 0\n"   /* TLBIALL */
        "mcr p15, 0, r1, c7, c5, 0\n"   /* ICIALLU */
        "mcr p15, 0, r1, c7, c5, 6\n"   /* BPIALL  */
        /* (5) DSB + ISB so the invalidations drain */
        "dsb\n"
        "isb\n"
        /* (6) SCTLR — M + I only, exactly as core 0 (C stays off) */
        "mrc p15, 0, r1, c1, c0, 0\n"
        "orr r1, r1, #(1 << 0)\n"       /* M = MMU enable */
        "orr r1, r1, #(1 << 12)\n"      /* I = instruction cache enable */
#ifdef DCACHE_ON
        "orr r1, r1, #(1 << 2)\n"       /* C = data cache enable (DCACHE_ON) */
#endif
        "mcr p15, 0, r1, c1, c0, 0\n"
        /* (7) ISB so the next fetch is translated */
        "isb\n"
        :
        : "r" (l1_table)
        : "r1", "memory"
    );
}

/* Re-map [pa, pa+len) as Normal Non-cacheable, rounding OUTWARDS to whole 1 MB
 * sections, and flush the TLB so the change takes effect.
 *
 * This exists for memory shared with a non-coherent agent whose address is only
 * known at runtime.  The one case is the GPU framebuffer: the VideoCore picks
 * its address and screenInit only learns it AFTER mmu_init has already built
 * the table, so it cannot be baked in up front.  Without this, C=1 leaves the
 * CPU's pixel writes sitting in the D-cache while the GPU scans out stale RAM.
 *
 * ★ Section granularity means any RAM sharing those 1 MB sections also becomes
 * uncached.  That is acceptable for the framebuffer (a large, contiguous, GPU-
 * owned region) and is the price of not needing a second-level table.
 *
 * Cleans the range first: whatever the CPU already wrote through the cache
 * would otherwise be stranded there once the mapping stops being cacheable. */
void mmu_set_range_noncached(unsigned long pa, unsigned long len)
{
    uint32_t first = (uint32_t)(pa >> 20);
    uint32_t last  = (uint32_t)((pa + len - 1) >> 20);
    uint32_t i;

    if (!mmu_enabled || len == 0) return;
    if (last >= L1_ENTRIES) last = L1_ENTRIES - 1;

    dcache_clean_range((const void *)(unsigned long)(first << 20),
                       (unsigned long)(last - first + 1) << 20);

    for (i = first; i <= last; i++)
        l1_table[i] = (i << 20) | ATTR_NC;

    /* The table is walked non-cacheably (TTBR0 RGN/IRGN = 0), but our own
     * writes to it went through the D-cache — push them out before the walker
     * can look, then drop the stale TLB entries. */
    dcache_clean_range(&l1_table[first], (last - first + 1) * sizeof(l1_table[0]));
    asm volatile (
        "dsb\n"
        "mov r1, #0\n"
        "mcr p15, 0, r1, c8, c7, 0\n"   /* TLBIALL */
        "dsb\n"
        "isb\n"
        ::: "r1", "memory"
    );
}

/* Disable MMU.  Safe because the identity-map means VA==PA throughout —
 * after we clear SCTLR.M, accesses go straight to PA and execution
 * continues from the same instructions.  Required before kexec because the
 * next-kernel's start.S expects MMU off (re-enables via its own
 * platforminit/mmu_init).
 *
 * ★ Under DCACHE_ON we must write the cache back first.  The original code
 * skipped this and said so ("Caches were off so no flush is needed") — true
 * then, false now: with C = 1 the next kernel's image, which /upload wrote
 * through the cache, would still be sitting in dirty lines when the cache
 * goes away, and kexec would jump into stale memory. */
void mmu_disable(void)
{
    if (!mmu_enabled) return;
#ifdef DCACHE_ON
    dcache_clean_invalidate_all();
#endif
    asm volatile (
        "mrc p15, 0, r1, c1, c0, 0\n"
        "bic r1, r1, #(1 << 0)\n"           /* clear M */
        "bic r1, r1, #(1 << 2)\n"           /* clear C: no cache without the
                                              * mappings that gave it its
                                              * attributes */
        "mcr p15, 0, r1, c1, c0, 0\n"
        "isb\n"
        "mov r1, #0\n"
        "mcr p15, 0, r1, c8, c7, 0\n"       /* TLBIALL */
        "dsb\n"
        "isb\n"
        :
        :
        : "r1", "memory"
    );
    mmu_enabled = 0;
}
