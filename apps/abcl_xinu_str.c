/**
 * @file abcl_xinu_str.c
 *
 * AIPL F4 — strings + int-array primitives for the Xinu runtime.
 *
 * Pool-backed (no GC yet) but tracks alloc/free counts so smokes can
 * verify no leaks after `n` cycles.  The pool sizes are tuned for
 * small embedded demos — a 32-slot array pool of 256 ints each, and
 * 8 rotating 128-byte string buffers.
 *
 * Builtins exposed to AIPL (via apps/Makerules + src/typing_env.ml):
 *
 *   str_concat(a, b)    -> str    a "+" b in the next rotating slot
 *   str_len(s)          -> int
 *   str_eq(a, b)        -> int    1 if equal, 0 otherwise
 *   str_int(n)          -> str    int -> decimal string
 *
 *   array_new(n, init)  -> obj    allocates an int array slot
 *   array_len(arr)      -> int
 *   array_get(arr, i)   -> int    bounds-checked; OOB marker + return 0
 *   array_set(arr, i, v) -> obj   bounds-checked; returns the same obj
 *   array_free(arr)     -> int    explicit release for the leak smoke
 *
 *   heap_stats()        -> int    encodes (in_use << 16) | total_alloc
 */

#include <stddef.h>
#include <kernel.h>
#include <stdint.h>
#include <string.h>

/* Value layout must match c_translator.ml's runtime_prelude_xinu. */
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
 *  String slots — 8 rotating buffers, 128 bytes each.  Good for
 *  ~8 in-flight str_concat results.
 * ============================================================ */

#define STR_SLOTS 8
#define STR_SLOT_LEN 128

static char  str_pool[STR_SLOTS][STR_SLOT_LEN];
static int   str_next;

static char *next_str_slot(void)
{
    char *p = str_pool[str_next];
    str_next = (str_next + 1) % STR_SLOTS;
    return p;
}

value_t str_concat(int n_args, value_t *args)
{
    const char *a, *b;
    char *out;
    int la, lb;
    if (n_args < 2) return v_str("");
    a = (args[0].tag == V_STR && args[0].s) ? args[0].s : "";
    b = (args[1].tag == V_STR && args[1].s) ? args[1].s : "";
    la = strlen(a);
    lb = strlen(b);
    if (la + lb >= STR_SLOT_LEN) {
        kprintf("[aipl] str_concat: too long (%d + %d >= %d)\r\n",
                la, lb, STR_SLOT_LEN);
        la = (la < STR_SLOT_LEN - 1) ? la : STR_SLOT_LEN - 1;
        lb = (la + lb >= STR_SLOT_LEN) ? STR_SLOT_LEN - 1 - la : lb;
    }
    out = next_str_slot();
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = 0;
    return v_str(out);
}

value_t str_len(int n_args, value_t *args)
{
    if (n_args < 1 || args[0].tag != V_STR || args[0].s == NULL)
        return v_int(0);
    return v_int((long)strlen(args[0].s));
}

value_t str_eq(int n_args, value_t *args)
{
    const char *a, *b;
    if (n_args < 2) return v_int(0);
    a = (args[0].tag == V_STR && args[0].s) ? args[0].s : "";
    b = (args[1].tag == V_STR && args[1].s) ? args[1].s : "";
    return v_int(0 == strcmp(a, b) ? 1 : 0);
}

value_t str_int(int n_args, value_t *args)
{
    long n;
    char *out;
    int neg, i, j;
    char tmp[16];

    if (n_args < 1) return v_str("");
    n   = (args[0].tag == V_INT) ? args[0].i : (long)args[0].f;
    out = next_str_slot();
    if (n == 0) { out[0] = '0'; out[1] = 0; return v_str(out); }
    neg = (n < 0);
    if (neg) n = -n;
    i = 0;
    while (n > 0 && i < (int)sizeof(tmp)) { tmp[i++] = '0' + (n % 10); n /= 10; }
    j = 0;
    if (neg) out[j++] = '-';
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
    return v_str(out);
}

/* ============================================================
 *  Int-array pool — 32 slots, up to 256 ints each.
 * ============================================================ */

#define ARR_SLOTS 32
#define ARR_CAP   256

struct arr_slot {
    int in_use;        /* 0 = free, 1 = allocated */
    int len;
    long data[ARR_CAP];
};

static struct arr_slot arr_pool[ARR_SLOTS];

/* Heap accounting — incremented by alloc, decremented by free.  Used
 * by the F4 leak-check smoke. */
static int g_alloc_count = 0;     /* lifetime allocations */
static int g_free_count  = 0;     /* lifetime frees */

static int alloc_slot(void)
{
    int i;
    for (i = 0; i < ARR_SLOTS; i++) {
        if (!arr_pool[i].in_use) {
            arr_pool[i].in_use = 1;
            arr_pool[i].len    = 0;
            return i;
        }
    }
    return -1;
}

value_t array_new(int n_args, value_t *args)
{
    int n, init, id, i;
    if (n_args < 1) return v_int(-1);
    n    = (int)args[0].i;
    init = (n_args >= 2) ? (int)args[1].i : 0;
    if (n < 0)          n = 0;
    if (n > ARR_CAP)    n = ARR_CAP;
    id = alloc_slot();
    if (id < 0) {
        kprintf("[aipl] array_new: pool exhausted (in_use=%d)\r\n",
                g_alloc_count - g_free_count);
        return v_int(-1);
    }
    arr_pool[id].len = n;
    for (i = 0; i < n; i++) arr_pool[id].data[i] = init;
    g_alloc_count++;
    if (g_alloc_count % 100 == 0) {
        kprintf("[aipl] heap alloc_count=%d free_count=%d in_use=%d\r\n",
                g_alloc_count, g_free_count, g_alloc_count - g_free_count);
    }
    /* Return as V_OBJ-tagged with obj_id = slot.  AIPL prints it as
     * `<obj N>` via v_print, which is fine for marker grep. */
    {
        value_t v;
        v.tag = V_OBJ; v.i = 0; v.f = 0; v.s = 0; v.obj_id = id;
        return v;
    }
}

static int arr_id_of(value_t v)
{
    if (v.tag == V_OBJ) return v.obj_id;
    if (v.tag == V_INT) return (int)v.i;
    return -1;
}

value_t array_len(int n_args, value_t *args)
{
    int id;
    if (n_args < 1) return v_int(0);
    id = arr_id_of(args[0]);
    if (id < 0 || id >= ARR_SLOTS || !arr_pool[id].in_use) return v_int(0);
    return v_int(arr_pool[id].len);
}

value_t array_get(int n_args, value_t *args)
{
    int id, i;
    if (n_args < 2) return v_int(0);
    id = arr_id_of(args[0]);
    i  = (int)args[1].i;
    if (id < 0 || id >= ARR_SLOTS || !arr_pool[id].in_use) {
        kprintf("[aipl] array_get: bad slot id=%d\r\n", id);
        return v_int(0);
    }
    if (i < 0 || i >= arr_pool[id].len) {
        kprintf("[aipl] array OOB get id=%d i=%d len=%d\r\n",
                id, i, arr_pool[id].len);
        return v_int(0);
    }
    return v_int(arr_pool[id].data[i]);
}

value_t array_set(int n_args, value_t *args)
{
    int id, i;
    long v;
    if (n_args < 3) return v_int(-1);
    id = arr_id_of(args[0]);
    i  = (int)args[1].i;
    v  = args[2].i;
    if (id < 0 || id >= ARR_SLOTS || !arr_pool[id].in_use) {
        kprintf("[aipl] array_set: bad slot id=%d\r\n", id);
        return v_int(-1);
    }
    if (i < 0 || i >= arr_pool[id].len) {
        kprintf("[aipl] array OOB set id=%d i=%d len=%d\r\n",
                id, i, arr_pool[id].len);
        return v_int(-1);
    }
    arr_pool[id].data[i] = v;
    return args[0];
}

value_t array_push(int n_args, value_t *args)
{
    /* Compatibility shim — appends one element if there's room.  Returns
     * the same obj or -1 on overflow. */
    int id;
    long v;
    if (n_args < 2) return v_int(-1);
    id = arr_id_of(args[0]);
    v  = args[1].i;
    if (id < 0 || id >= ARR_SLOTS || !arr_pool[id].in_use) {
        kprintf("[aipl] array_push: bad slot id=%d\r\n", id);
        return v_int(-1);
    }
    if (arr_pool[id].len >= ARR_CAP) {
        kprintf("[aipl] array_push: full id=%d len=%d\r\n",
                id, arr_pool[id].len);
        return v_int(-1);
    }
    arr_pool[id].data[arr_pool[id].len++] = v;
    return args[0];
}

value_t array_free(int n_args, value_t *args)
{
    int id;
    if (n_args < 1) return v_int(0);
    id = arr_id_of(args[0]);
    if (id < 0 || id >= ARR_SLOTS || !arr_pool[id].in_use) return v_int(0);
    arr_pool[id].in_use = 0;
    arr_pool[id].len    = 0;
    g_free_count++;
    return v_int(1);
}

value_t heap_stats(int n_args, value_t *args)
{
    int in_use;
    (void)n_args; (void)args;
    in_use = g_alloc_count - g_free_count;
    kprintf("[aipl] heap stats alloc=%d free=%d in_use=%d\r\n",
            g_alloc_count, g_free_count, in_use);
    return v_int(((long)in_use << 16) | (long)(g_alloc_count & 0xFFFF));
}
