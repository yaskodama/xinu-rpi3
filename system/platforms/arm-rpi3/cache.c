/**
 * @file cache.c
 *
 * ARMv7 / Cortex-A53 D-cache maintenance + coherency enable (BCM2837).
 * See include/cache.h for what each of these is for and which hazard it
 * guards against.
 *
 * All of it is written from scratch: this tree had no D-cache maintenance at
 * all, because the port ran with SCTLR.C = 0.
 */

#include <stdint.h>
#include <cache.h>

/* ---- set/way maintenance --------------------------------------------------
 * The architectural walk (ARM ARM B4.2.1, "Example code for cache
 * maintenance operations"): read CLIDR for the cache topology, and for each
 * level that has a data/unified cache, read CCSIDR for that level's geometry
 * and issue the operation for every (set, way).
 *
 * The set/way operand layout is  level<<1 | set<<L | way<<A  where
 *   L = log2(line bytes)      -- from CCSIDR.LineSize
 *   A = 32 - log2(associativity)  -- i.e. ways are packed at the TOP of the
 *       word, so the shift is the leading-zero count of the max way index.
 */

#define DCISW(v)   __asm__ volatile("mcr p15, 0, %0, c7, c6, 2"  :: "r"(v) : "memory")
#define DCCISW(v)  __asm__ volatile("mcr p15, 0, %0, c7, c14, 2" :: "r"(v) : "memory")

static void dcache_setway_all(int clean)
{
    uint32_t clidr, loc, level;

    __asm__ volatile("mrc p15, 1, %0, c0, c0, 1" : "=r"(clidr));   /* CLIDR   */
    loc = (clidr >> 24) & 7;                                       /* LoC     */

    for (level = 0; level < loc; level++) {
        uint32_t ctype = (clidr >> (level * 3)) & 7;
        uint32_t ccsidr, linesh, ways, sets, wayshift, w, s;

        /* ctype: 2 = data, 3 = separate I+D, 4 = unified.  1 = I-cache only,
         * 0 = none — neither has a D-side to maintain. */
        if (ctype < 2) continue;

        /* CSSELR = (level << 1) | 0  -> select this level's data cache. */
        __asm__ volatile("mcr p15, 2, %0, c0, c0, 0" :: "r"(level << 1) : "memory");
        __asm__ volatile("isb");                  /* CSSELR must land before CCSIDR */
        __asm__ volatile("mrc p15, 1, %0, c0, c0, 0" : "=r"(ccsidr));

        linesh = (ccsidr & 7) + 4;                /* log2(line bytes)          */
        ways   = (ccsidr >> 3)  & 0x3FF;          /* max way index             */
        sets   = (ccsidr >> 13) & 0x7FFF;         /* max set index             */

        /* Ways sit at the top of the operand: shift = clz(max way index). */
        wayshift = (uint32_t)__builtin_clz(ways);

        for (w = 0; w <= ways; w++) {
            for (s = 0; s <= sets; s++) {
                uint32_t v = (level << 1) | (s << linesh) | (w << wayshift);
                if (clean) DCCISW(v);
                else       DCISW(v);
            }
        }
    }

    /* CSSELR back to level 0, and make every maintenance op visible. */
    __asm__ volatile("mcr p15, 2, %0, c0, c0, 0" :: "r"(0) : "memory");
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}

void dcache_invalidate_all(void)       { dcache_setway_all(0); }
void dcache_clean_invalidate_all(void) { dcache_setway_all(1); }

/* ---- range maintenance ---------------------------------------------------
 * Line size is read from CTR rather than assumed: getting it wrong silently
 * skips lines (leaving stale data) instead of failing loudly. */
static uint32_t dcache_line_bytes(void)
{
    uint32_t ctr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));   /* CTR */
    /* CTR.DminLine [19:16] = log2(words) of the smallest D-line. */
    return 4u << ((ctr >> 16) & 0xF);
}

static void dcache_range(const void *addr, unsigned long len, int invalidate)
{
    uint32_t line = dcache_line_bytes();
    uint32_t p    = (uint32_t)(unsigned long)addr & ~(line - 1);   /* round down */
    uint32_t end  = (uint32_t)(unsigned long)addr + (uint32_t)len;

    for (; p < end; p += line) {
        if (invalidate)
            __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(p) : "memory"); /* DCIMVAC */
        else
            __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(p) : "memory");/* DCCMVAC */
    }
    __asm__ volatile("dsb" ::: "memory");
}

void dcache_clean_range(const void *addr, unsigned long len)
{
    dcache_range(addr, len, 0);
}

void dcache_invalidate_range(const void *addr, unsigned long len)
{
    dcache_range(addr, len, 1);
}

/* ---- coherency enable ----------------------------------------------------
 * Cortex-A53 CPUECTLR is a 64-bit CP15 register reached with MRRC/MCRR
 * p15, 1, <Rt>, <Rt2>, c15 (A53 TRM 4.5.66).  SMPEN is bit 6 of the low word:
 * "enables data coherency with other cores".  The TRM requires it be set
 * before the caches and MMU are enabled, and it is NOT set out of reset —
 * the firmware's armstub sets it for core 0 only, and our secondary cores
 * enter through our own _smp_start, which the armstub never touches. */
void smpen_set(void)
{
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 1, %0, %1, c15" : "=r"(lo), "=r"(hi));
    if (lo & (1u << 6)) return;                  /* already in the domain */
    lo |= (1u << 6);                             /* CPUECTLR.SMPEN */
    __asm__ volatile("mcrr p15, 1, %0, %1, c15" :: "r"(lo), "r"(hi) : "memory");
    __asm__ volatile("isb" ::: "memory");
}
