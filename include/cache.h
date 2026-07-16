/**
 * @file cache.h
 *
 * ARMv7 / Cortex-A53 D-cache maintenance and coherency control.
 *
 * This port historically ran with the D-cache OFF (SCTLR.C = 0), which is why
 * none of this existed: with every access going straight to RAM there was
 * nothing to maintain.  These primitives exist for the DCACHE_ON build
 * variant, which turns the D-cache on to answer one question — does
 * memory-bound work scale across the 4 cores once it can be cached?
 * (With C = 0 it measurably does not: 0.95--1.35x, versus 3.2--4.0x for
 * compute.  See docs/smp_report.)
 *
 * ★ Two hazards these guard against, both of which are silent and fatal:
 *
 *   1. Out of reset the caches hold GARBAGE, not zeroes.  Setting SCTLR.C
 *      without invalidating first tells the core that garbage is valid data.
 *      An earlier attempt at D-cache on this port bricked the board exactly
 *      here (see the comment in mmu.c).  Call dcache_invalidate_all() BEFORE
 *      enabling the cache — never after, which would instead discard the
 *      real data you just cached.
 *
 *   2. On Cortex-A53, Normal-cacheable-shareable memory is NOT coherent
 *      between cores unless CPUECTLR.SMPEN is set.  mmu.c already marks RAM
 *      shareable, so with C = 1 and SMPEN = 0 the SMP job mailbox in smp.c
 *      (plain volatile + dsb) breaks silently — `volatile` constrains the
 *      compiler, not the cache.  Call smpen_set() on EVERY core, before its
 *      caches and MMU come up (the A53 TRM requires that ordering).
 */

#ifndef _CACHE_H_
#define _CACHE_H_

/* Invalidate the entire D-cache by set/way, all levels to the Level of
 * Coherency.  Discards contents WITHOUT writing back — for use only on a
 * cache whose contents are meaningless (i.e. before first enabling it). */
void dcache_invalidate_all(void);

/* Clean + invalidate the entire D-cache by set/way, all levels to LoC.
 * Writes dirty lines back to RAM.  Needed before handing memory to a
 * non-coherent agent wholesale, and before turning the MMU/cache off
 * (mmu_disable/kexec), where dirty lines would otherwise be lost. */
void dcache_clean_invalidate_all(void);

/* Clean (write back) the D-cache over [addr, addr+len).  Use before a
 * non-coherent reader — another core running with its MMU still off, a DMA
 * engine, or the VideoCore — reads memory this core has written.
 * ★ Operates on whole 64-byte lines: the range is rounded outwards, so never
 * call this on a buffer that shares a line with data another agent may be
 * writing concurrently. */
void dcache_clean_range(const void *addr, unsigned long len);

/* Invalidate the D-cache over [addr, addr+len) so a subsequent read sees what
 * a non-coherent writer put in RAM.
 * ★ DANGEROUS on unaligned buffers: this DISCARDS whole 64-byte lines, so any
 * neighbouring data sharing the first or last line is lost.  The buffer must
 * be 64-byte aligned and 64-byte padded.  This is precisely why the USB DMA
 * path (heap and stack buffers at 8-byte alignment) cannot simply be fixed by
 * sprinkling calls to this — see docs/smp_report. */
void dcache_invalidate_range(const void *addr, unsigned long len);

/* Set CPUECTLR.SMPEN on the calling core: join the coherency domain, and
 * enable receipt of cache/TLB maintenance broadcasts.  Must be called before
 * this core enables its caches and MMU.  Harmless if already set. */
void smpen_set(void);

#endif /* _CACHE_H_ */
