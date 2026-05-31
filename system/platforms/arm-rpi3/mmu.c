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
 * Note we ONLY set SCTLR.M.  Enabling D-cache (C=1) simultaneously
 * with MMU is hazardous on Cortex-A53 without a prior D-cache
 * set/way invalidate loop — bricked Pi 3 on the first attempt of
 * this commit's predecessor.  Region attributes (TEX/C/B/XN) in the
 * page-table descriptors are enforced by the MMU regardless of cache
 * state — so MMIO is already strongly-ordered Device memory.
 * Cache enable can come in a later commit with proper invalidation.
 *
 * Call exactly once, late in platforminit() after RAM bounds are known.
 * Idempotent guard avoids double-init if anyone re-enters.
 */

#include <stddef.h>
#include <stdint.h>

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
        /* (4) Invalidate TLB (entire) */
        "mov r1, #0\n"
        "mcr p15, 0, r1, c8, c7, 0\n"
        /* (5) DSB + ISB so all invalidations + table writes drain */
        "dsb\n"
        "isb\n"
        /* (6) SCTLR — read, set ONLY M (MMU), write back.  Do NOT
         * set C/I/Z here — see big comment at top of file. */
        "mrc p15, 0, r1, c1, c0, 0\n"
        "orr r1, r1, #(1 << 0)\n"    /* M = MMU enable */
        "mcr p15, 0, r1, c1, c0, 0\n"
        /* (7) ISB so the next fetch sees MMU on */
        "isb\n"
        :
        : "r" (l1_table)
        : "r1", "memory"
    );

    mmu_enabled = 1;
}
