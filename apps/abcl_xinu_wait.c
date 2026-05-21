/**
 * @file abcl_xinu_wait.c
 *
 * Xinu-side implementation of AIPL's wait(ms) builtin.  aipl2c
 * --xinu now emits `b_wait(n_args, args)` for any AIPL wait()
 * call (mangled to avoid colliding with Xinu kernel's wait(sem)).
 * This file is what those calls actually resolve to at link time.
 *
 * Signature matches every other AIPL builtin emitted by
 * gen_program_xinu:
 *    value_t b_wait(int n_args, value_t *args)
 *
 * Behaviour: read the (first) integer argument as milliseconds and
 * call Xinu's `sleep(ms)`.  Float arguments are truncated to int
 * milliseconds (AIPL types wait : int|float -> unit but the unit of
 * the float overload is also seconds vs milliseconds-ambiguous; we
 * pick ms for both so callers don't accidentally sleep 1000× too
 * long).  Zero or negative arg → return immediately.
 *
 * Returns NIL — AIPL `wait` has return type unit.
 */
#include <stddef.h>

typedef enum { V_NIL, V_INT, V_FLOAT, V_STR, V_OBJ } vtag_t;
typedef struct {
    vtag_t      tag;
    long        i;
    double      f;
    const char *s;
    int         obj_id;
} value_t;

extern int sleep(unsigned int ms);
extern int kprintf(const char *fmt, ...);

static value_t v_nil(void)
{
    value_t v;
    v.tag = V_NIL;
    v.i = 0;
    v.f = 0;
    v.s = 0;
    v.obj_id = 0;
    return v;
}

value_t b_wait(int n_args, value_t *args)
{
    long ms = 0;

    if (n_args >= 1 && args != 0) {
        if (args[0].tag == V_INT) {
            ms = args[0].i;
        } else if (args[0].tag == V_FLOAT) {
            ms = (long)args[0].f;
        }
    }
    if (ms > 0) {
        sleep((unsigned int)ms);
    }
    return v_nil();
}
