/**
 * @file buddy.c
 *
 * Binary buddy allocator — Xinu Kernel Evolution Round 2 champion axis
 * `memory_alloc=buddy`.  See buddy.h for the rationale.
 *
 * Layout
 * ------
 *   arena      : a BUDDY_ARENA_SIZE-aligned, power-of-two region carved
 *                from the classic heap via memget() at init.
 *   freelist[] : one singly-linked list per order; a free block stores its
 *                successor pointer in its own first word (every block is at
 *                least BUDDY_MIN_SIZE >= sizeof(void*), so this is safe).
 *   meta[]     : one byte per BUDDY_MIN_SIZE unit, indexed by
 *                (addr - base) >> BUDDY_MIN_ORDER.  Encodes the *head* of a
 *                block: low bits = order, bit 0x80 = allocated.  META_NONE
 *                marks a non-head unit.  This lets buddy_free() recover a
 *                block's order from its pointer alone — no user-visible
 *                header, so returned blocks are fully usable and naturally
 *                aligned.
 *
 * Buddy identity: the buddy of the block at min-unit index @mi with order
 * @o is at  mi XOR (1 << (o - BUDDY_MIN_ORDER)).  Because @base is
 * BUDDY_ARENA_SIZE-aligned, this XOR never leaves the arena.
 */

#include <kernel.h>
#include <memory.h>
#include <interrupt.h>
#include <buddy.h>

#define META_NONE   0xFFu       /* unit is not a block head            */
#define META_ALLOC  0x80u       /* bit set on an allocated block head  */
#define META_ORDER(m)  ((uint)((m) & 0x7Fu))

struct bnode                    /* overlaid on a free block's first word */
{
    struct bnode *next;
};

static struct bnode *freelist[BUDDY_NORDERS];
static unsigned char meta[BUDDY_ARENA_SIZE >> BUDDY_MIN_ORDER];

static ulong arena_base;        /* aligned base address of the arena   */
static int   inited = FALSE;

static char  report_buf[160] = "buddy: untested";

/* ---- small helpers (caller holds the interrupt mask) ---------------- */

static inline uint mi_of(ulong addr)
{
    return (uint)((addr - arena_base) >> BUDDY_MIN_ORDER);
}

static inline ulong addr_of(uint mi)
{
    return arena_base + ((ulong)mi << BUDDY_MIN_ORDER);
}

static void fl_push(uint order, ulong addr)
{
    struct bnode *n = (struct bnode *)addr;
    n->next = freelist[order - BUDDY_MIN_ORDER];
    freelist[order - BUDDY_MIN_ORDER] = n;
}

static struct bnode *fl_pop(uint order)
{
    struct bnode *n = freelist[order - BUDDY_MIN_ORDER];
    if (n != NULL)
    {
        freelist[order - BUDDY_MIN_ORDER] = n->next;
    }
    return n;
}

/* Unlink a specific address from order's free list.  Returns TRUE if it
 * was present. */
static int fl_remove(uint order, ulong addr)
{
    struct bnode **pp = &freelist[order - BUDDY_MIN_ORDER];
    struct bnode  *n  = (struct bnode *)addr;

    while (*pp != NULL)
    {
        if (*pp == n)
        {
            *pp = n->next;
            return TRUE;
        }
        pp = &(*pp)->next;
    }
    return FALSE;
}

/* ---- public API ------------------------------------------------------ */

syscall buddy_init(void)
{
    irqmask im;
    void   *raw;
    ulong   base;
    uint    i;

    im = disable();
    if (inited)
    {
        restore(im);
        return OK;
    }

    /* Over-allocate by one arena so we can align the base up to a
     * BUDDY_ARENA_SIZE boundary (required for the XOR buddy identity).
     * The slack on either side is simply never handed to the allocator;
     * it stays owned by us for the life of the kernel. */
    raw = memget(BUDDY_ARENA_SIZE * 2);
    if ((void *)SYSERR == raw)
    {
        restore(im);
        return SYSERR;
    }

    base = ((ulong)raw + (BUDDY_ARENA_SIZE - 1)) & ~((ulong)BUDDY_ARENA_SIZE - 1);
    arena_base = base;

    for (i = 0; i < BUDDY_NORDERS; i++)
    {
        freelist[i] = NULL;
    }
    for (i = 0; i < (BUDDY_ARENA_SIZE >> BUDDY_MIN_ORDER); i++)
    {
        meta[i] = META_NONE;
    }

    /* The whole arena starts as a single maximal-order free block. */
    meta[0] = (unsigned char)BUDDY_MAX_ORDER;
    fl_push(BUDDY_MAX_ORDER, base);

    inited = TRUE;
    restore(im);
    return OK;
}

void *buddy_alloc(uint nbytes)
{
    irqmask im;
    uint    need, order, o;
    ulong   block;

    if (0 == nbytes)
    {
        return (void *)SYSERR;
    }

    /* Smallest order whose block size covers nbytes. */
    need = BUDDY_MIN_ORDER;
    while (((ulong)1 << need) < (ulong)nbytes)
    {
        need++;
    }
    if (need > BUDDY_MAX_ORDER)
    {
        return (void *)SYSERR;
    }
    order = need;

    im = disable();
    if (!inited)
    {
        restore(im);
        return (void *)SYSERR;
    }

    /* Find the smallest available block of at least the requested order. */
    o = order;
    while (o <= BUDDY_MAX_ORDER && freelist[o - BUDDY_MIN_ORDER] == NULL)
    {
        o++;
    }
    if (o > BUDDY_MAX_ORDER)
    {
        restore(im);
        return (void *)SYSERR;          /* arena exhausted */
    }

    block = (ulong)fl_pop(o);
    meta[mi_of(block)] = META_NONE;     /* no longer a free head */

    /* Split down to the requested order, freeing the upper buddies. */
    while (o > order)
    {
        ulong buddy;
        o--;
        buddy = block + ((ulong)1 << o);
        meta[mi_of(buddy)] = (unsigned char)o;
        fl_push(o, buddy);
    }

    meta[mi_of(block)] = (unsigned char)(order | META_ALLOC);
    restore(im);
    return (void *)block;
}

syscall buddy_free(void *ptr)
{
    irqmask im;
    uint    mi, order;

    im = disable();
    if (!inited
        || (ulong)ptr < arena_base
        || (ulong)ptr >= arena_base + BUDDY_ARENA_SIZE)
    {
        restore(im);
        return SYSERR;
    }

    mi = mi_of((ulong)ptr);
    if (!(meta[mi] & META_ALLOC) || meta[mi] == META_NONE)
    {
        restore(im);
        return SYSERR;                  /* not a live allocated head */
    }
    order = META_ORDER(meta[mi]);
    meta[mi] = META_NONE;

    /* Coalesce upward while the buddy is a free block of the same order. */
    while (order < BUDDY_MAX_ORDER)
    {
        uint bmi = mi ^ (1u << (order - BUDDY_MIN_ORDER));

        if (meta[bmi] != (unsigned char)order)
        {
            break;                      /* buddy absent, allocated, or split */
        }
        fl_remove(order, addr_of(bmi));
        meta[bmi] = META_NONE;
        if (bmi < mi)
        {
            mi = bmi;                   /* lower address becomes the head */
        }
        order++;
    }

    meta[mi] = (unsigned char)order;
    fl_push(order, addr_of(mi));
    restore(im);
    return OK;
}

uint buddy_freecount(uint order)
{
    irqmask im;
    uint    n = 0;
    struct bnode *p;

    if (order < BUDDY_MIN_ORDER || order > BUDDY_MAX_ORDER)
    {
        return 0;
    }
    im = disable();
    for (p = freelist[order - BUDDY_MIN_ORDER]; p != NULL; p = p->next)
    {
        n++;
    }
    restore(im);
    return n;
}

const char *buddy_report(void)
{
    return report_buf;
}

/* ---- boot self-test -------------------------------------------------- */

syscall buddy_selftest(void)
{
    extern int kprintf(const char *, ...);
    extern int sprintf(char *, const char *, ...);

    void *a, *b, *c, *d;
    int   pass = TRUE;
    int   step = 0;
    uint  full0, full1;

    if (OK != buddy_init())
    {
        sprintf(report_buf, "buddy: init FAILED (no heap)");
        kprintf("[Buddy] %s\r\n", report_buf);
        return SYSERR;
    }

    /* A pristine arena is exactly one maximal-order block. */
    full0 = buddy_freecount(BUDDY_MAX_ORDER);
    if (full0 < 1) { pass = FALSE; }
    step = 1;

    /* Split: a tiny request must carve the big block into one block of
     * every intermediate order, leaving zero maximal-order blocks. */
    a = buddy_alloc(1);
    if ((void *)SYSERR == a) { pass = FALSE; }
    if (buddy_freecount(BUDDY_MAX_ORDER) != full0 - 1) { pass = FALSE; }
    step = 2;

    /* Distinct, in-arena, 8-byte-aligned, non-overlapping allocations. */
    b = buddy_alloc(40);            /* -> order 6 (64 B)  */
    c = buddy_alloc(5000);          /* -> order 13 (8 KB) */
    if ((void *)SYSERR == b || (void *)SYSERR == c) { pass = FALSE; }
    if (a == b || b == c || a == c) { pass = FALSE; }
    if (((ulong)a | (ulong)b | (ulong)c) & 0x7u) { pass = FALSE; }
    step = 3;

    /* Over-large request must be refused, not satisfied. */
    d = buddy_alloc(BUDDY_ARENA_SIZE + 1);
    if ((void *)SYSERR != d) { pass = FALSE; }
    step = 4;

    /* Free everything; the arena must coalesce back to its pristine
     * single maximal-order block (full coalescing — the whole point). */
    if (OK != buddy_free(a)) { pass = FALSE; }
    if (OK != buddy_free(b)) { pass = FALSE; }
    if (OK != buddy_free(c)) { pass = FALSE; }
    full1 = buddy_freecount(BUDDY_MAX_ORDER);
    if (full1 != full0) { pass = FALSE; }
    step = 5;

    /* Double free must be rejected. */
    if (SYSERR != buddy_free(a)) { pass = FALSE; }
    step = 6;

    sprintf(report_buf,
            "buddy: %s (arena=%uKB min=%uB orders=%u; full %u->%u, step %d)",
            pass ? "PASS" : "FAIL",
            (uint)(BUDDY_ARENA_SIZE >> 10), (uint)BUDDY_MIN_SIZE,
            (uint)BUDDY_NORDERS, full0, full1, step);
    kprintf("[Buddy] %s\r\n", report_buf);
    return pass ? OK : SYSERR;
}
