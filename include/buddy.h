/**
 * @file buddy.h
 *
 * Binary buddy allocator (Xinu Kernel Evolution — memory_alloc=buddy).
 *
 * Round2 of the AIPL/GA kernel-evolution search converged on a champion
 * genome (I24, score 0.825) whose `memory_alloc=buddy` axis appeared in
 * every one of the top-six genomes.  This is that axis, implemented as a
 * self-contained subsystem that coexists with the classic first-fit
 * `memget()`/`memfree()` heap rather than replacing it — so the existing
 * regression gate stays green and the new code can be verified in
 * isolation on real Raspberry Pi 3 hardware.
 *
 * The allocator manages a single power-of-two arena carved from the heap
 * at init time.  Allocation splits the smallest sufficient free block;
 * free coalesces buddies bottom-up via the address-XOR identity.  No
 * per-block header is stored in the user region: a side table records the
 * order of each block head, so buddy_free() needs only the pointer.
 */
#ifndef _BUDDY_H_
#define _BUDDY_H_

#include <stddef.h>

#define BUDDY_MIN_ORDER   5                       /* 32-byte minimum block  */
#define BUDDY_MAX_ORDER   18                      /* 256 KiB arena          */
#define BUDDY_NORDERS     (BUDDY_MAX_ORDER - BUDDY_MIN_ORDER + 1)
#define BUDDY_ARENA_SIZE  (1u << BUDDY_MAX_ORDER)
#define BUDDY_MIN_SIZE    (1u << BUDDY_MIN_ORDER)

/* Bring up the buddy arena.  Returns OK, or SYSERR if the heap cannot
 * satisfy the (aligned) arena reservation.  Idempotent: a second call is
 * a no-op once initialized. */
syscall buddy_init(void);

/* Allocate at least @nbytes bytes, 8-byte aligned, from the buddy arena.
 * Returns a pointer, or (void *)SYSERR on a zero/too-large request or when
 * the arena is exhausted.  Free with buddy_free(). */
void   *buddy_alloc(uint nbytes);

/* Free a block previously returned by buddy_alloc().  Returns OK, or
 * SYSERR if @ptr is not a live buddy block head (bad pointer / double
 * free). */
syscall buddy_free(void *ptr);

/* Number of free blocks currently on the free list of a given order
 * (BUDDY_MIN_ORDER..BUDDY_MAX_ORDER).  Used by the boot self-test and the
 * /api/buddy diagnostic route. */
uint    buddy_freecount(uint order);

/* Boot self-test.  Exercises split, exact-fit, OOM, and full coalescing,
 * then writes a one-line summary into a static buffer and returns OK (all
 * checks passed) or SYSERR.  The summary is readable via buddy_report(). */
syscall buddy_selftest(void);

/* Pointer to the static NUL-terminated self-test summary (never NULL once
 * buddy_selftest() has run; otherwise an "untested" placeholder). */
const char *buddy_report(void);

#endif                          /* _BUDDY_H_ */
