/**
 * @file xsh_rcutest.c
 *
 * Shell command `rcutest`: stress + verify the RCU primitives (system/rcu.c).
 *
 * Spawns two LOCK-FREE reader threads that repeatedly walk an RCU-protected
 * singly-linked list, and one writer thread that prepends nodes and, every few
 * iterations, unlinks an interior node, calls synchronize_rcu(), then frees it.
 * Readers take NO lock and never disable interrupts; correctness rests on the
 * grace period guaranteeing the freed node is unreachable first.
 *
 * The readers validate the list is always a finite, well-formed chain whose
 * values stay in range: a use-after-free or torn pointer would show up as a
 * cycle (length blow-up) or an out-of-range value, counted as corruption.
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread.h>
#include <rcu.h>

#define RCU_VMAX   50          /* writer values are 1..RCU_VMAX            */
#define RCU_WALK   200000      /* per-walk node cap (cycle => corruption)  */

struct rnode { int val; struct rnode *next; };

static struct rnode * volatile rcu_list;     /* RCU-protected list head    */
static volatile int           rcu_stop;
static volatile ulong         rcu_reads, rcu_updates, rcu_frees, rcu_bad;

/* Reader: lock-free traversal, repeated until told to stop. */
static thread rcu_reader(void)
{
    while (!rcu_stop)
    {
        int n = 0;
        long sum = 0;
        struct rnode *p;

        rcu_read_lock();
        p = rcu_dereference(rcu_list);
        while (p != NULL && n < RCU_WALK)
        {
            int v = p->val;
            if (v < 1 || v > RCU_VMAX)      /* torn / freed node          */
                rcu_bad++;
            sum += v;
            p = rcu_dereference(p->next);
            n++;
        }
        rcu_read_unlock();

        if (n >= RCU_WALK)                  /* never terminated => a cycle */
            rcu_bad++;
        rcu_reads++;
        (void)sum;
    }
    return OK;
}

/* Writer: prepend nodes; periodically unlink an interior node and reclaim it
 * after a grace period. */
static thread rcu_writer(void)
{
    int i;
    for (i = 0; i < 4000 && !rcu_stop; i++)
    {
        struct rnode *nn = (struct rnode *)malloc(sizeof(struct rnode));
        if (nn == NULL)
            break;
        nn->val  = (i % RCU_VMAX) + 1;
        nn->next = rcu_list;
        rcu_assign_pointer(rcu_list, nn);   /* publish the new head       */
        rcu_updates++;

        /* Every 8th iteration, splice out the 2nd node and free it safely. */
        if ((i & 7) == 7)
        {
            struct rnode *h = rcu_list;
            if (h != NULL && h->next != NULL)
            {
                struct rnode *victim = h->next;
                rcu_assign_pointer(h->next, victim->next);  /* unlink     */
                synchronize_rcu();                          /* grace period */
                free(victim);                               /* now safe   */
                rcu_frees++;
            }
        }

        if ((i & 31) == 31)
            sleep(1);                       /* yield CPU; bound the run    */
    }
    return OK;
}

shellcmd xsh_rcutest(int nargs, char *args[])
{
    tid_typ r1, r2, w;
    struct rnode *p;
    int k;

    (void)nargs; (void)args;

    rcu_list = NULL;
    rcu_stop = 0;
    rcu_reads = rcu_updates = rcu_frees = rcu_bad = 0;

    /* Seed a few nodes so readers have something to walk immediately. */
    for (k = 0; k < 4; k++)
    {
        struct rnode *nn = (struct rnode *)malloc(sizeof(struct rnode));
        if (nn == NULL)
            break;
        nn->val  = k + 1;
        nn->next = rcu_list;
        rcu_list = nn;
    }

    printf("rcutest: 2 lock-free RCU readers + 1 writer (concurrency_safety=rcu)\n");

    r1 = create((void *)rcu_reader, 8192, 30, "rcu_r1", 0);
    r2 = create((void *)rcu_reader, 8192, 30, "rcu_r2", 0);
    w  = create((void *)rcu_writer, 8192, 30, "rcu_w",  0);
    if ((int)r1 == SYSERR || (int)r2 == SYSERR || (int)w == SYSERR)
    {
        printf("  could not create worker threads\n");
        return 1;
    }
    ready(r1, RESCHED_NO);
    ready(r2, RESCHED_NO);
    ready(w,  RESCHED_YES);

    sleep(2000);                 /* let them run ~2 s                      */
    rcu_stop = 1;
    sleep(300);                  /* let the workers observe stop + exit    */

    /* Reclaim whatever is left on the list. */
    p = rcu_list;
    rcu_list = NULL;
    while (p != NULL)
    {
        struct rnode *n = p->next;
        free(p);
        p = n;
    }

    printf("  reads=%lu  updates=%lu  frees=%lu  corruption=%lu\n",
           rcu_reads, rcu_updates, rcu_frees, rcu_bad);
    printf("  %s\n", (rcu_bad == 0)
           ? "PASS: lock-free readers always saw a consistent list"
           : "FAIL: traversal corruption detected");
    return 0;
}
