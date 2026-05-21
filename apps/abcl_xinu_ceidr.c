/**
 * @file abcl_xinu_ceidr.c
 *
 * F3 — CE/DR runtime stubs.
 *
 * The AIPL next-gen samples (CE-1..13 capability/effect + DR-1..13
 * distributed) emit calls into a small set of runtime helpers
 * (`grant_cap`, `pool_create`, `crdt_gcounter_*`, etc.) that the
 * POSIX C runtime ships with via libc + pthread.  On Xinu we provide
 * minimum-viable stubs so the same .abcl source links, the actor
 * system still spawns + dispatches, and the type/effect/refinement
 * inference performed at aipl2c --check time becomes observable in
 * the QEMU log via `[aipl] cap/pool/crdt ...` markers.
 *
 * These stubs intentionally do NOT implement the full semantics —
 * that's the job of dedicated F-series follow-ups (real capabilities,
 * a real CRDT runtime, an actual worker pool).  Their purpose here
 * is to make CE-10/11/12 + DR-10/11/12/13 runnable end-to-end on
 * Xinu so the F3 Xinu parity column reaches ≥6/13.
 */

#include <stddef.h>
#include <kernel.h>
#include <string.h>

typedef enum { V_NIL, V_INT, V_FLOAT, V_STR, V_OBJ } vtag_t;
typedef struct {
    vtag_t      tag;
    long        i;
    double      f;
    const char *s;
    int         obj_id;
} value_t;

static value_t v_int(long n)
{
    value_t v; v.tag = V_INT; v.i = n; v.f = 0; v.s = 0; v.obj_id = 0;
    return v;
}
static value_t v_str(const char *s)
{
    value_t v; v.tag = V_STR; v.i = 0; v.f = 0; v.s = s; v.obj_id = 0;
    return v;
}

/* ============================================================
 *  CE-11 capability stubs
 * ============================================================ */

static char g_caps[128] = "";

value_t current_caps(int n_args, value_t *args)
{
    (void)n_args; (void)args;
    return v_str(g_caps);
}

value_t grant_cap(int n_args, value_t *args)
{
    const char *c;
    int len, have;
    if (n_args < 1 || args[0].tag != V_STR || args[0].s == NULL)
        return v_int(0);
    c    = args[0].s;
    len  = strlen(c);
    have = strlen(g_caps);
    if (have + 1 + len + 1 < (int)sizeof(g_caps)) {
        if (have > 0) { g_caps[have++] = ','; g_caps[have] = 0; }
        memcpy(g_caps + have, c, len + 1);
        kprintf("[aipl] cap grant=%s now=%s\r\n", c, g_caps);
        return v_int(1);
    }
    return v_int(0);
}

value_t revoke_cap(int n_args, value_t *args)
{
    const char *c;
    char *p;
    int  cl;
    if (n_args < 1 || args[0].tag != V_STR || args[0].s == NULL)
        return v_int(0);
    c  = args[0].s;
    cl = strlen(c);
    /* Find `c` in the comma-separated list and excise it. */
    p = g_caps;
    while (*p) {
        if (0 == strncmp(p, c, cl) &&
            (p[cl] == 0 || p[cl] == ','))
        {
            int tail_off = (p[cl] == ',') ? (cl + 1) : cl;
            int rest_len = strlen(p + tail_off);
            /* shift the rest left in place (libxc has no memmove) */
            int i;
            for (i = 0; i <= rest_len; i++) p[i] = p[i + tail_off];
            /* trim trailing comma if we removed the last entry */
            {
                int n = strlen(g_caps);
                if (n > 0 && g_caps[n - 1] == ',') g_caps[n - 1] = 0;
            }
            kprintf("[aipl] cap revoke=%s now=%s\r\n", c, g_caps);
            return v_int(1);
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return v_int(0);
}

/* ============================================================
 *  DR-13 worker pool stubs
 *  pool_create(name, min, max, initial) -> str (just echoes name)
 * ============================================================ */

static char g_pool_name[64] = "";
static int  g_pool_size     = 0;

value_t pool_create(int n_args, value_t *args)
{
    const char *name;
    long initial;
    if (n_args < 4) return v_str("");
    name    = (args[0].tag == V_STR && args[0].s) ? args[0].s : "pool";
    initial = (args[3].tag == V_INT) ? args[3].i : 0;
    strncpy(g_pool_name, name, sizeof(g_pool_name) - 1);
    g_pool_name[sizeof(g_pool_name) - 1] = 0;
    g_pool_size = (int)initial;
    kprintf("[aipl] pool create name=%s initial=%d\r\n", g_pool_name, g_pool_size);
    return v_str(g_pool_name);
}

value_t pool_size(int n_args, value_t *args)
{
    (void)n_args; (void)args;
    return v_int(g_pool_size);
}

value_t pool_destroy(int n_args, value_t *args)
{
    (void)n_args; (void)args;
    kprintf("[aipl] pool destroy name=%s prev_size=%d\r\n",
            g_pool_name, g_pool_size);
    g_pool_size = 0;
    return v_str("ok");
}

/* ============================================================
 *  DR-10 CRDT G-counter stubs (4-replica fixed limit)
 * ============================================================ */

#define CRDT_REPLICAS 4
struct crdt_g {
    int  in_use;
    long replica[CRDT_REPLICAS];
};
#define CRDT_SLOTS 8
static struct crdt_g crdt_pool[CRDT_SLOTS];

static int crdt_alloc(void)
{
    int i;
    for (i = 0; i < CRDT_SLOTS; i++) if (!crdt_pool[i].in_use) {
        int j;
        crdt_pool[i].in_use = 1;
        for (j = 0; j < CRDT_REPLICAS; j++) crdt_pool[i].replica[j] = 0;
        return i;
    }
    return -1;
}

value_t crdt_gcounter_new(int n_args, value_t *args)
{
    int id;
    value_t v;
    (void)n_args; (void)args;
    id = crdt_alloc();
    kprintf("[aipl] crdt gcounter new id=%d\r\n", id);
    v.tag = V_OBJ; v.i = 0; v.f = 0; v.s = 0; v.obj_id = id;
    return v;
}

value_t crdt_gcounter_inc(int n_args, value_t *args)
{
    int id, r;
    long n;
    if (n_args < 3) return v_int(-1);
    id = (args[0].tag == V_OBJ) ? args[0].obj_id : (int)args[0].i;
    r  = (int)args[1].i;
    n  = args[2].i;
    if (id < 0 || id >= CRDT_SLOTS || !crdt_pool[id].in_use) return v_int(-1);
    if (r  < 0 || r  >= CRDT_REPLICAS)                       return v_int(-1);
    crdt_pool[id].replica[r] += n;
    return v_int(0);
}

value_t crdt_gcounter_value(int n_args, value_t *args)
{
    int id, r;
    long sum = 0;
    if (n_args < 1) return v_int(0);
    id = (args[0].tag == V_OBJ) ? args[0].obj_id : (int)args[0].i;
    if (id < 0 || id >= CRDT_SLOTS || !crdt_pool[id].in_use) return v_int(0);
    for (r = 0; r < CRDT_REPLICAS; r++) sum += crdt_pool[id].replica[r];
    kprintf("[aipl] crdt gcounter id=%d value=%ld\r\n", id, sum);
    return v_int(sum);
}

value_t crdt_gcounter_merge(int n_args, value_t *args)
{
    int dst, src, r;
    if (n_args < 2) return v_int(0);
    dst = (args[0].tag == V_OBJ) ? args[0].obj_id : (int)args[0].i;
    src = (args[1].tag == V_OBJ) ? args[1].obj_id : (int)args[1].i;
    if (dst < 0 || dst >= CRDT_SLOTS || !crdt_pool[dst].in_use) return v_int(0);
    if (src < 0 || src >= CRDT_SLOTS || !crdt_pool[src].in_use) return v_int(0);
    for (r = 0; r < CRDT_REPLICAS; r++) {
        if (crdt_pool[src].replica[r] > crdt_pool[dst].replica[r])
            crdt_pool[dst].replica[r] = crdt_pool[src].replica[r];
    }
    kprintf("[aipl] crdt gcounter merge dst=%d src=%d\r\n", dst, src);
    return v_int(0);
}

/* ============================================================
 *  DR-12 multi-region failover stubs
 * ============================================================ */

static const char *g_region = "primary";

value_t current_region(int n_args, value_t *args)
{
    (void)n_args; (void)args;
    return v_str(g_region);
}

value_t failover_region(int n_args, value_t *args)
{
    const char *to;
    if (n_args < 1) return v_str(g_region);
    to = (args[0].tag == V_STR && args[0].s) ? args[0].s : "primary";
    /* Store the literal pointer (must live for program lifetime — AIPL
     * string literals are static).  Print marker for the smoke. */
    g_region = to;
    kprintf("[aipl] region failover -> %s\r\n", g_region);
    return v_str(g_region);
}
