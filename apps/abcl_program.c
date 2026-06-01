#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <thread.h>
#include <semaphore.h>
#include <clock.h>     /* P3: clkticks for throughput markers */
#include <stdio.h>
#include <string.h>

/* P3: bumped from 16 to 64 so the lock-free MPSC ring can sustain
   higher producer fan-in without back-pressure dropping messages. */
#define MAX_MAILBOX 64
#define MAX_OBJECTS 16
#define MAX_FIELDS  16
#define MAX_ARGS    8

static int max_messages       = 20;
static int messages_processed = 0;
volatile int global_shutdown  = 0;
static semaphore counter_mu;
static semaphore print_mu;

typedef enum { V_NIL, V_INT, V_FLOAT, V_STR, V_OBJ } vtag_t;
typedef struct {
  vtag_t      tag;
  long        i;
  double      f;
  const char *s;
  int         obj_id;
} value_t;

static value_t mk_int(long n)        { value_t v; v.tag=V_INT;   v.i=n;   v.f=0; v.s=0; v.obj_id=0; return v; }
static value_t mk_float(double n)    { value_t v; v.tag=V_FLOAT; v.f=n;   v.i=0; v.s=0; v.obj_id=0; return v; }
static value_t mk_str(const char *s) { value_t v; v.tag=V_STR;   v.s=s;   v.i=0; v.f=0; v.obj_id=0; return v; }
static value_t mk_obj(int id)        { value_t v; v.tag=V_OBJ;   v.obj_id=id; v.i=0; v.f=0; v.s=0; return v; }

static int truthy(value_t v) {
  switch (v.tag) {
  case V_INT:   return v.i != 0;
  case V_FLOAT: return v.f != 0.0;
  case V_STR:   return v.s != NULL && v.s[0] != '\0';
  case V_OBJ:   return v.obj_id >= 0;
  default:      return 0;
  }
}

static value_t v_binop(const char *op, value_t a, value_t b) {
  long ai = (a.tag == V_INT) ? a.i : (a.tag == V_FLOAT ? (long)a.f : 0);
  long bi = (b.tag == V_INT) ? b.i : (b.tag == V_FLOAT ? (long)b.f : 0);
  if (op[0] == '+' && op[1] == '\0') return mk_int(ai + bi);
  if (op[0] == '-' && op[1] == '\0') return mk_int(ai - bi);
  if (op[0] == '*' && op[1] == '\0') return mk_int(ai * bi);
  if (op[0] == '/' && op[1] == '\0') return mk_int(bi != 0 ? ai / bi : 0);
  if (op[0] == '=' && op[1] == '=')  return mk_int(ai == bi);
  if (op[0] == '!' && op[1] == '=')  return mk_int(ai != bi);
  if (op[0] == '<' && op[1] == '=')  return mk_int(ai <= bi);
  if (op[0] == '>' && op[1] == '=')  return mk_int(ai >= bi);
  if (op[0] == '<' && op[1] == '\0') return mk_int(ai <  bi);
  if (op[0] == '>' && op[1] == '\0') return mk_int(ai >  bi);
  return mk_int(0);
}

#ifdef _XINU_PLATFORM_ARM_RPI3_
extern void aipl_console_puts(const char *s);  /* HDMI console (apps/gwm.c) */
#endif

static void v_int_to_buf(char *b, int *p, long n) {
  char t[20];
  int  i = 0, j, neg = (n < 0);
  if (neg) n = -n;
  if (n == 0) t[i++] = '0';
  while (n > 0) { t[i++] = (char)('0' + (n % 10)); n /= 10; }
  if (neg) b[(*p)++] = '-';
  for (j = i - 1; j >= 0; j--) b[(*p)++] = t[j];
}

/* Print a value to the serial console AND (on the Pi3) the on-screen
 * AIPL print window. */
static void v_print(value_t v) {
  char line[80];
  int  p = 0, i;
  switch (v.tag) {
  case V_STR: { const char *s = v.s ? v.s : "";
                for (i = 0; s[i] && p < 79; i++) line[p++] = s[i]; break; }
  case V_INT: v_int_to_buf(line, &p, v.i); break;
  case V_OBJ: { const char *pre = "<obj ";
                for (i = 0; pre[i] && p < 79; i++) line[p++] = pre[i];
                v_int_to_buf(line, &p, v.obj_id);
                if (p < 79) line[p++] = '>'; break; }
  default:    { const char *s = "<nil>";
                for (i = 0; s[i] && p < 79; i++) line[p++] = s[i]; break; }
  }
  line[p] = '\0';
  wait(print_mu);
  kprintf("%s\r\n", line);
  signal(print_mu);
#ifdef _XINU_PLATFORM_ARM_RPI3_
  aipl_console_puts(line);
#endif
}

/* Narrate a Xinu philosopher's state into the AIPL print stream (serial +
 * HDMI console): formats "P<pid> <action>" and prints it. */
void abcl_phil_say(int pid, const char *action) {
  static char pool[8][48];
  static int  slot = 0;
  char *b = pool[slot & 7];
  int   p = 0, i = 0;
  slot++;
  b[p++] = 'P';
  if (pid >= 10) b[p++] = (char)('0' + (pid / 10));
  b[p++] = (char)('0' + (pid % 10));
  b[p++] = ' ';
  while (action[i] && p < 46) b[p++] = action[i++];
  b[p] = '\0';
  v_print(mk_str(b));
}

typedef struct {
  int         sender;
  int         receiver;
  const char *method;
  int         n_args;
  value_t     args[MAX_ARGS];
} message_t;

/* P3: lock-free MPSC bounded ring buffer (Vyukov style, single consumer).
   - `enq` is bumped via CAS by multiple producers, no critical section.
   - `deq` is touched only by the owning actor's thread, so a plain
     atomic store suffices.
   - `slot_seq[i]` is a per-slot sequence stamp; producer sees its slot
     ready when seq==pos, consumer sees ready payload when seq==pos+1,
     and the consumer recycles the slot for the producer at pos+CAP by
     storing seq=pos+CAP.  Initialized so slot_seq[i]=i.
   - `items` is a counting semaphore kept solely so the receiver can
     block on empty.  Lock-free bookkeeping eliminates wait()/signal()
     on the producer path's critical section (only the kernel signal()
     call into `items` remains, which is ISR-safe in Xinu).
*/
typedef struct {
  message_t msgs[MAX_MAILBOX];
  volatile uint32_t slot_seq[MAX_MAILBOX];
  volatile uint32_t enq;
  volatile uint32_t deq;
  volatile uint32_t drops;   /* producer-side drop counter (full mailbox) */
  semaphore items;
} mailbox_t;

typedef struct {
  int       class_id;
  value_t   fields[MAX_FIELDS];
  mailbox_t mbox;
  tid_typ   tid;
  int       started;
  volatile int dead;     /* set by abcl_actor_suicide(): the actor's worker
                            thread exits after the current dispatch returns */
  /* Activity timestamps for the GC sweep — both stamped on each enq/deq
   * so a "zombie" actor (no recent mailbox activity AND no progress
   * processing what's queued) shows up as old by either metric. */
  volatile unsigned long last_enq_ticks;
  volatile unsigned long last_deq_ticks;
  unsigned long          birth_ticks;
  volatile int           protected_from_gc;
} object_t;

static object_t objects[MAX_OBJECTS];
static int      n_objects = 0;
static semaphore objects_mu;

static void mailbox_init(mailbox_t *mb) {
  int i;
  mb->enq   = 0;
  mb->deq   = 0;
  mb->drops = 0;
  for (i = 0; i < MAX_MAILBOX; i++) mb->slot_seq[i] = (uint32_t)i;
  mb->items = semcreate(0);
}

void wake_all_actors(void) {
  int i;
  for (i = 0; i < n_objects; i++) {
    /* items を 1 増やすことで wait() しているアクターを起こす */
    signal(objects[i].mbox.items);
  }
}

void abcl_shutdown(void) {
  global_shutdown = 1;
  wake_all_actors();
}

/* F1: public accessors over the static `objects[]` table.  Used by
   abcl_xinu_chkpt.c to snapshot / restore actor fields without
   re-declaring the object_t layout in two files.  Returns 1 on
   success, 0 on a bad slot or field index. */
int abcl_object_field_count(void) { return MAX_FIELDS; }

int abcl_object_field_get(int obj_id, int field_idx, value_t *out) {
  if (obj_id < 0 || obj_id >= n_objects) return 0;
  if (field_idx < 0 || field_idx >= MAX_FIELDS) return 0;
  *out = objects[obj_id].fields[field_idx];
  return 1;
}

int abcl_object_field_set(int obj_id, int field_idx, value_t v) {
  if (obj_id < 0 || obj_id >= n_objects) return 0;
  if (field_idx < 0 || field_idx >= MAX_FIELDS) return 0;
  objects[obj_id].fields[field_idx] = v;
  return 1;
}

int abcl_object_class_id(int obj_id) {
  if (obj_id < 0 || obj_id >= n_objects) return -1;
  return objects[obj_id].class_id;
}

/* Render actor field as plain text into the caller's buf.  Returns
 * bytes written.  Caller must pass cap >= ~80 (Xinu's libxc has no
 * snprintf so we sprintf and trust the cap).  Used by webactor's
 * /api/object-field since value_t is file-scope here. */
int abcl_object_field_render(int obj_id, int field_idx, char *buf, int cap) {
  value_t v;
  if (cap < 80) { if (cap > 0) buf[0] = 0; return 0; }
  if (abcl_object_field_get(obj_id, field_idx, &v) == 0)
    return sprintf(buf, "out_of_range");
  switch (v.tag) {
    case V_NIL:   return sprintf(buf, "nil");
    case V_INT:   return sprintf(buf, "int=%ld", v.i);
    case V_FLOAT: return sprintf(buf, "float=%f", v.f);
    case V_STR: {
      /* Truncate the string to keep us under cap; sprintf is bounds-less. */
      char tmp[48];
      int i = 0;
      const char *s = v.s ? v.s : "(null)";
      while (s[i] && i < 47) { tmp[i] = s[i]; i++; }
      tmp[i] = 0;
      return sprintf(buf, "str=%s", tmp);
    }
    case V_OBJ:   return sprintf(buf, "obj_id=%d", v.obj_id);
    default:      return sprintf(buf, "tag=%d", (int)v.tag);
  }
}

/* H3 RPC: expose total live actor count so the dispatcher LIST command
   can answer without walking the table. */
int abcl_n_objects(void) { return n_objects; }

/* S3 DeadlineHints: expose the Xinu tid_typ that backs an AIPL actor,
   so the set_deadline builtin can translate an obj_id into the
   actual thread id that the kernel's setdeadline() takes. */
int abcl_object_tid(int obj_id) {
  if (obj_id < 0 || obj_id >= n_objects) return -1;
  return (int)objects[obj_id].tid;
}

/* Mailbox telemetry for the on-screen actor monitor (apps/gwm.c).
 * enq = total messages received, deq = total processed, so (enq - deq)
 * is the current backlog; drops = messages lost to a full mailbox;
 * started = 1 once the actor's consumer thread has been spawned. */
int abcl_object_enq(int obj_id) {
  if (obj_id < 0 || obj_id >= n_objects) return -1;
  return (int)objects[obj_id].mbox.enq;
}
int abcl_object_deq(int obj_id) {
  if (obj_id < 0 || obj_id >= n_objects) return -1;
  return (int)objects[obj_id].mbox.deq;
}
int abcl_object_drops(int obj_id) {
  if (obj_id < 0 || obj_id >= n_objects) return -1;
  return (int)objects[obj_id].mbox.drops;
}
int abcl_object_started(int obj_id) {
  if (obj_id < 0 || obj_id >= n_objects) return -1;
  return objects[obj_id].started;
}
int abcl_object_dead(int obj_id) {
  if (obj_id < 0 || obj_id >= n_objects) return -1;
  return objects[obj_id].dead;
}

/* GC support: age = ms since the more recent of last_enq / last_deq.
 * On the 100 Hz Xinu clock, each clkticks unit is 10 ms.  Returns -1
 * for an invalid id. */
long abcl_object_age_ms(int obj_id) {
  if (obj_id < 0 || obj_id >= n_objects) return -1;
  unsigned long now = clkticks;
  unsigned long e = objects[obj_id].last_enq_ticks;
  unsigned long d = objects[obj_id].last_deq_ticks;
  unsigned long last = (e > d) ? e : d;
  return (long)((now - last) * 10);
}

void abcl_object_protect(int obj_id, int on) {
  if (obj_id < 0 || obj_id >= n_objects) return;
  objects[obj_id].protected_from_gc = on ? 1 : 0;
}

int abcl_object_protected(int obj_id) {
  if (obj_id < 0 || obj_id >= n_objects) return 0;
  return objects[obj_id].protected_from_gc;
}

/* GC sweep: iterate live actors, force-kill those older than threshold
 * (unless protected).  dry_run=1 reports without killing.  Returns
 * count of kills. */
int abcl_gc_sweep(long threshold_ms, int dry_run, int *out_scanned) {
  int killed = 0, scanned = 0;
  int i;
  for (i = 0; i < n_objects; i++) {
    if (objects[i].dead) continue;
    if (!objects[i].started) continue;
    scanned++;
    if (objects[i].protected_from_gc) continue;
    long age = abcl_object_age_ms(i);
    if (age >= 0 && age >= threshold_ms) {
      if (!dry_run) {
        tid_typ t = objects[i].tid;
        if (t > 0) {
          kill(t);
          objects[i].dead = 1;
        }
      }
      killed++;
    }
  }
  if (out_scanned) *out_scanned = scanned;
  return killed;
}

/* 自殺関数: the actor terminates itself.  Sets the dead flag so its worker
 * thread (abcl_actor_main) leaves its dispatch loop after the current
 * message and exits.  Callable from a generated method via self_id. */
void abcl_actor_suicide(int self_id) {
  if (self_id < 0 || self_id >= n_objects) return;
  objects[self_id].dead = 1;
}

/* Clear the whole actor table so a host program can run again from a clean
 * slate (the next SPAWN starts at id 0).  Without this, re-running a program
 * spawns new actors at shifted ids while leftover actors from the previous
 * run persist, and the table fills toward MAX_OBJECTS.  Stops every actor
 * thread cooperatively (mark dead + wake it so it leaves its loop), frees the
 * per-mailbox semaphores, then resets n_objects.  Call when the actors are
 * idle / between runs (the RESET RPC opcode invokes this). */
void abcl_rt_reset(void) {
  int i, total;
  wait(objects_mu);
  total = n_objects;
  signal(objects_mu);
  for (i = 0; i < total; i++) objects[i].dead = 1;
  for (i = 0; i < total; i++) signal(objects[i].mbox.items); /* wake blocked */
  sleep(100);                       /* let the worker threads exit their loops */
  for (i = 0; i < total; i++) {
    semfree(objects[i].mbox.items); /* avoid a semaphore leak across resets */
    objects[i].started = 0;
    objects[i].dead    = 0;
  }
  wait(objects_mu);
  n_objects = 0;
  signal(objects_mu);
  kprintf("[aipl] runtime reset — %d actors cleared, next id starts at 0\r\n",
          total);
}

/* Xinu の queue.h にある enqueue() と名前が衝突するのでリネーム。
   以降 abcl 側のコードでは enqueue マクロで本関数を呼ぶ。 */
/* R1 smoke markers: print the FIRST send + FIRST recv to the serial
   console so a -nographic QEMU run can verify the AIPL actor system
   was reached via grep.  Subsequent sends/recvs are silent to keep
   the log readable. */
static volatile int abcl_first_send_logged = 0;
static volatile int abcl_first_recv_logged = 0;
extern semaphore print_mu;
void abcl_log_first_send(int receiver, const char *method) {
  if (abcl_first_send_logged) return;
  abcl_first_send_logged = 1;
  wait(print_mu);
  kprintf("[aipl] first-send to=%d method=%s\r\n",
          receiver, method ? method : "?");
  signal(print_mu);
}
void abcl_log_first_recv(int self_id, const char *method) {
  if (abcl_first_recv_logged) return;
  abcl_first_recv_logged = 1;
  wait(print_mu);
  kprintf("[aipl] first-recv on=%d method=%s\r\n",
          self_id, method ? method : "?");
  signal(print_mu);
}

/* P3 lock-free MPSC enqueue.
   The hot path has zero kernel disable()/restore() — only LDREX/STREX
   pairs (GCC __atomic primitives on ARMv6+).  The trailing signal() on
   `items` is the only kernel call; it is safe to invoke from an ISR
   because Xinu's signal() handles its own irq mask. */
void abcl_enqueue(int sender, int receiver, const char *method,
                  int n_args, value_t *args) {
  if (receiver < 0 || receiver >= n_objects) return;
  abcl_log_first_send(receiver, method);
  /* Stamp activity timestamp early (under the same critical section is
   * not strictly needed since both clkticks read + the field are
   * single-word writes — the GC sweep tolerates a one-tick fuzz). */
  objects[receiver].last_enq_ticks = clkticks;
  mailbox_t *mb = &objects[receiver].mbox;
#ifdef _XINU_PLATFORM_ARM_RPI3_
  /* Pi3 (Cortex-A53 with MMU / L1 D-cache OFF => strongly-ordered memory):
     LDREX/STREX, which the __atomic lock-free path below relies on, are
     UNPREDICTABLE / fault on strongly-ordered memory and abort here.  Use a
     short interrupt-disabled critical section instead — Xinu is
     non-preemptive while interrupts are disabled, so plain accesses are
     race-free across producers and the single consumer. */
  {
    irqmask im = disable();
    uint32_t pos = mb->enq;
    uint32_t idx;
    int i;
    if (pos - mb->deq >= (uint32_t)MAX_MAILBOX) {
      mb->drops++;
      restore(im);
      return;
    }
    idx = pos % (uint32_t)MAX_MAILBOX;
    mb->msgs[idx].sender   = sender;
    mb->msgs[idx].receiver = receiver;
    mb->msgs[idx].method   = method;
    mb->msgs[idx].n_args   = n_args;
    for (i = 0; i < n_args && i < MAX_ARGS; i++)
      mb->msgs[idx].args[i] = args[i];
    mb->slot_seq[idx] = pos + 1;
    mb->enq = pos + 1;
    restore(im);
  }
  signal(mb->items);
#else
  {
  uint32_t pos;
  uint32_t idx;
  uint32_t cur;
  int retries;

  /* (1) Reserve a slot.  Loop: load enq, verify slot is free
     (slot_seq==pos), CAS-bump enq.  Bounded retry. */
  for (retries = 0; retries < 256; retries++) {
    pos = __atomic_load_n(&mb->enq, __ATOMIC_ACQUIRE);
    if (pos - __atomic_load_n(&mb->deq, __ATOMIC_ACQUIRE) >= (uint32_t)MAX_MAILBOX) {
      /* Bounded ring is full — back off briefly and retry.  After
         several yields, give up so a stuck consumer can't deadlock
         a producer thread. */
      if (retries > 16) {
        __atomic_fetch_add(&mb->drops, 1, __ATOMIC_RELAXED);
        return;
      }
      yield();
      continue;
    }
    idx = pos % (uint32_t)MAX_MAILBOX;
    cur = __atomic_load_n(&mb->slot_seq[idx], __ATOMIC_ACQUIRE);
    if (cur != pos) {
      /* Slot not yet recycled by consumer — retry (rare under MPSC). */
      continue;
    }
    if (__atomic_compare_exchange_n(&mb->enq, &pos, pos + 1,
                                    /*weak=*/0,
                                    __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
      break;  /* won reservation */
    }
    /* CAS lost — another producer grabbed pos; retry. */
  }
  if (retries >= 256) {
    __atomic_fetch_add(&mb->drops, 1, __ATOMIC_RELAXED);
    return;
  }

  /* (2) Write payload — we have exclusive ownership of slot[idx]
     because slot_seq[idx]==pos blocked any other producer. */
  mb->msgs[idx].sender   = sender;
  mb->msgs[idx].receiver = receiver;
  mb->msgs[idx].method   = method;
  mb->msgs[idx].n_args   = n_args;
  {
    int i;
    for (i = 0; i < n_args && i < MAX_ARGS; i++)
      mb->msgs[idx].args[i] = args[i];
  }

  /* (3) Publish — releasing store on slot_seq makes the payload
     visible to the consumer. */
  __atomic_store_n(&mb->slot_seq[idx], pos + 1, __ATOMIC_RELEASE);

  /* (4) Wake the consumer if blocked. */
  signal(mb->items);
  }
#endif
}

/* 以降の生成コードでは abcl_enqueue を enqueue として書く */
#define enqueue abcl_enqueue

/* P1 heartbeat thread: sleep + print, sleep + print, ...  Six ticks
   over ~6 sec.  Always lands as long as Xinu's scheduler keeps
   running — i.e. as long as the actor pool isn't busy-looping.
   Each tick also reports the live actor count, which grows past the
   global-declaration count as constructors spawn nested actors. */
thread abcl_heartbeat(void) {
  int i;
  for (i = 0; i < 6; i++) {
    sleep(1000);
    if (global_shutdown) break;
    wait(print_mu);
    kprintf("[aipl] heartbeat tick=%d actors=%d msgs=%d\r\n",
            i, n_objects, messages_processed);
    signal(print_mu);
  }
  return OK;
}

/* runtime cap override */
static int _abcl_cap = 0;

#define CLASS_Fork 0
#define CLASS_Philosopher 1
#define CLASS_WebReceiver 2   /* web-bridge demo actor (apps/webactor.c) */
#define CLASS_Dispatcher 3    /* load-balancer router */
#define CLASS_Worker 4        /* load-balancer worker (compute target) */
#define CLASS_Collector 5     /* periodic actor GC */

/* P2: AIPL class -> Xinu priority */
#define ABCL_PRIO_Fork 20
#define ABCL_PRIO_Philosopher 20
#define ABCL_PRIO_WebReceiver 20
#define ABCL_PRIO_Dispatcher 25   /* routing is fast — keep ahead of workers */
#define ABCL_PRIO_Worker 22       /* one above philosophers so submit() lands */
#define ABCL_PRIO_Collector 26    /* GC runs above dispatcher so sweeps land */
static int abcl_class_prio(int class_id) {
  switch (class_id) {
  case CLASS_Fork: return ABCL_PRIO_Fork;
  case CLASS_Philosopher: return ABCL_PRIO_Philosopher;
  case CLASS_WebReceiver: return ABCL_PRIO_WebReceiver;
  case CLASS_Dispatcher: return ABCL_PRIO_Dispatcher;
  case CLASS_Worker:     return ABCL_PRIO_Worker;
  case CLASS_Collector:  return ABCL_PRIO_Collector;
  default: return INITPRIO;
  }
}
const char* abcl_class_name(int class_id) {
  switch (class_id) {
  case CLASS_Fork: return "Fork";
  case CLASS_Philosopher: return "Philosopher";
  case CLASS_WebReceiver: return "WebReceiver";
  case CLASS_Dispatcher: return "Dispatcher";
  case CLASS_Worker:     return "Worker";
  case CLASS_Collector:  return "Collector";
  default: return "?";
  }
}

static void dispatch_Fork(int, int, const char*, value_t*, int);
static void dispatch_Philosopher(int, int, const char*, value_t*, int);
static void dispatch_Dispatcher(int, int, const char*, value_t*, int);
static void dispatch_Worker(int, int, const char*, value_t*, int);
static void dispatch_Collector(int, int, const char*, value_t*, int);
static void dispatch(int, int, const char*, value_t*, int);
static int  alloc_obj(int class_id, int n_args, value_t* args);
static void spawn_actor(int id);
int         create_obj(int class_id, int n_args, value_t* args);
thread      abcl_actor_main(int self_id);

static int g_f0 = -1;
static int g_f1 = -1;
static int g_f2 = -1;
static int g_f3 = -1;
static int g_f4 = -1;
static int g_p4 = -1;
static int g_p5 = -1;
/* load-balancer globals (set in aipl_main, read by webactor via accessors) */
#define LB_N_WORKERS 4
static int g_loadbal_disp = -1;
static int g_loadbal_w[LB_N_WORKERS] = { -1, -1, -1, -1 };

/* Garbage-collector actor globals.  g_gc_actor is the Collector obj_id;
 * the heartbeat thread sends "tick" to it every GC_HEARTBEAT_MS as long
 * as the actor's F_GC_enabled field is non-zero. */
static int g_gc_actor = -1;
#define GC_HEARTBEAT_MS 1000   /* heartbeat poll granularity (ms) */

/* Rate-limiter (token bucket) for HTTP /submit + /jit gating.  Kept as
 * file-scope state because the Dispatcher object is at the F_Disp__N=13
 * fields budget already (MAX_FIELDS=16 leaves room for 3 more, all
 * earmarked for future Dispatcher-internal use).  Race on the counters
 * is benign — worst case we miss a refill tick or count a throttle
 * twice; not worth a mutex on the hot path. */
static long g_rate_tokens       = 100;   /* current bucket level */
static long g_rate_capacity     = 100;   /* burst cap */
static long g_rate_per_sec      = 50;    /* refill rate */
static long g_rate_last_refill  = 0;     /* clkticks at last refill */
static long g_rate_throttled    = 0;     /* lifetime 429-from-rate count */

/* Server-side task timeout.  Collector.tick scans the task table and
 * marks any PENDING entry older than this CANCELLED.  Mac can detect
 * via /api/loadbal/task?id=N (state=CANCELLED + done_ms still 0). */
static long g_task_timeout_ms   = 30000;
static long g_task_timed_out    = 0;     /* lifetime auto-expire count */

enum { F_Fork_holder, F_Fork__N };

static void init_fields_Fork(int self_id) {
  (void)self_id;
  objects[self_id].fields[F_Fork_holder] = mk_int((long)(0L));
}

static void Fork_acquire(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  value_t p_pid = (n_args > 0) ? args[0] : mk_int(0L);
  if (truthy(mk_int((long)(((long)((((objects[self_id].fields[F_Fork_holder]).tag == V_INT ? (objects[self_id].fields[F_Fork_holder]).i : (long)((objects[self_id].fields[F_Fork_holder]).f))) == (0L))))))) {
    objects[self_id].fields[F_Fork_holder] = p_pid;
    enqueue(self_id, sender_id, "fork_granted", 1, (value_t[]){p_pid});
  } else {
    enqueue(self_id, sender_id, "fork_denied", 1, (value_t[]){p_pid});
  }
}

static void Fork_release(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  value_t p_pid = (n_args > 0) ? args[0] : mk_int(0L);
  if (truthy(v_binop("==", mk_int((long)(((objects[self_id].fields[F_Fork_holder]).tag == V_INT ? (objects[self_id].fields[F_Fork_holder]).i : (long)((objects[self_id].fields[F_Fork_holder]).f)))), p_pid))) {
    objects[self_id].fields[F_Fork_holder] = mk_int((long)(0L));
  } else {
  }
}

static void dispatch_Fork(int self_id, int sender_id, const char* method, value_t* args, int n_args) {
  if (strcmp(method, "acquire") == 0) { Fork_acquire(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "release") == 0) { Fork_release(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "init") == 0) return; /* default no-op init */
  fprintf(stderr, "unknown method %s on Fork\n", method);
}

enum { F_Philosopher_my_id, F_Philosopher_f_low, F_Philosopher_f_high, F_Philosopher_meals, F_Philosopher_meal_idx, F_Philosopher_state, F_Philosopher__N };

static void init_fields_Philosopher(int self_id) {
  (void)self_id;
  objects[self_id].fields[F_Philosopher_my_id] = mk_int((long)(0L));
  objects[self_id].fields[F_Philosopher_f_low] = mk_int(0L);
  objects[self_id].fields[F_Philosopher_f_high] = mk_int(0L);
  objects[self_id].fields[F_Philosopher_meals] = mk_int((long)(50L));
  objects[self_id].fields[F_Philosopher_meal_idx] = mk_int((long)(0L));
  objects[self_id].fields[F_Philosopher_state] = mk_int((long)(0L));
}

static void Philosopher_init(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  long p_id_arg = (n_args > 0) ? ((args[0]).tag == V_INT ? (args[0]).i : (long)((args[0]).f)) : (long)(0L);
  int p_lo = (n_args > 1) ? ((args[1]).obj_id) : (int)(-1);
  int p_hi = (n_args > 2) ? ((args[2]).obj_id) : (int)(-1);
  objects[self_id].fields[F_Philosopher_my_id] = mk_int((long)(p_id_arg));
  objects[self_id].fields[F_Philosopher_f_low] = mk_obj((int)(p_lo));
  objects[self_id].fields[F_Philosopher_f_high] = mk_obj((int)(p_hi));
  objects[self_id].fields[F_Philosopher_meals] = mk_int((long)(50L));
  objects[self_id].fields[F_Philosopher_meal_idx] = mk_int((long)(0L));
  objects[self_id].fields[F_Philosopher_state] = mk_int((long)(0L));
  enqueue(self_id, self_id, "try_eat", 0, NULL);
}

static void Philosopher_try_eat(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  int _pid = (int)((objects[self_id].fields[F_Philosopher_my_id]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_my_id]).i : (long)((objects[self_id].fields[F_Philosopher_my_id]).f));
  if (truthy(mk_int((long)(((long)((((objects[self_id].fields[F_Philosopher_meals]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_meals]).i : (long)((objects[self_id].fields[F_Philosopher_meals]).f))) == (0L))))))) {
    abcl_phil_say(_pid, "finished — terminating (suicide)");
    abcl_actor_suicide(self_id);
  } else {
    abcl_phil_say(_pid, "thinking");
    sleep(450);   /* pace so the narration is readable on the console */
    enqueue(self_id, objects[self_id].fields[F_Philosopher_f_low].obj_id, "acquire", 1, (value_t[]){mk_int((long)(((objects[self_id].fields[F_Philosopher_my_id]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_my_id]).i : (long)((objects[self_id].fields[F_Philosopher_my_id]).f))))});
  }
}

static void Philosopher_fork_granted(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  value_t p_pid = (n_args > 0) ? args[0] : mk_int(0L);
  int _pid = (int)((objects[self_id].fields[F_Philosopher_my_id]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_my_id]).i : (long)((objects[self_id].fields[F_Philosopher_my_id]).f));
  if (truthy(mk_int((long)(((long)((((objects[self_id].fields[F_Philosopher_state]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_state]).i : (long)((objects[self_id].fields[F_Philosopher_state]).f))) == (0L))))))) {
    /* got the first (low) fork — go for the second (high) one. */
    objects[self_id].fields[F_Philosopher_state] = mk_int((long)(1L));
    abcl_phil_say(_pid, "took a fork");
    enqueue(self_id, objects[self_id].fields[F_Philosopher_f_high].obj_id, "acquire", 1, (value_t[]){mk_int((long)(((objects[self_id].fields[F_Philosopher_my_id]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_my_id]).i : (long)((objects[self_id].fields[F_Philosopher_my_id]).f))))});
  } else {
    /* got the second fork — both held, so eat, then put them down. */
    abcl_phil_say(_pid, "took a fork");
    abcl_phil_say(_pid, "eating");
    sleep(450);   /* eating (holds both forks) */
    objects[self_id].fields[F_Philosopher_meal_idx] = mk_int((long)(((((objects[self_id].fields[F_Philosopher_meal_idx]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_meal_idx]).i : (long)((objects[self_id].fields[F_Philosopher_meal_idx]).f))) + (1L))));
    objects[self_id].fields[F_Philosopher_meals] = mk_int((long)(((((objects[self_id].fields[F_Philosopher_meals]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_meals]).i : (long)((objects[self_id].fields[F_Philosopher_meals]).f))) - (1L))));
    enqueue(self_id, objects[self_id].fields[F_Philosopher_f_high].obj_id, "release", 1, (value_t[]){mk_int((long)(((objects[self_id].fields[F_Philosopher_my_id]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_my_id]).i : (long)((objects[self_id].fields[F_Philosopher_my_id]).f))))});
    enqueue(self_id, objects[self_id].fields[F_Philosopher_f_low].obj_id, "release", 1, (value_t[]){mk_int((long)(((objects[self_id].fields[F_Philosopher_my_id]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_my_id]).i : (long)((objects[self_id].fields[F_Philosopher_my_id]).f))))});
    abcl_phil_say(_pid, "put down forks");
    objects[self_id].fields[F_Philosopher_state] = mk_int((long)(0L));
    enqueue(self_id, self_id, "try_eat", 0, NULL);
  }
}

static void Philosopher_fork_denied(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  value_t p_pid = (n_args > 0) ? args[0] : mk_int(0L);
  if (truthy(mk_int((long)(((long)((((objects[self_id].fields[F_Philosopher_state]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_state]).i : (long)((objects[self_id].fields[F_Philosopher_state]).f))) == (1L))))))) {
    enqueue(self_id, objects[self_id].fields[F_Philosopher_f_low].obj_id, "release", 1, (value_t[]){mk_int((long)(((objects[self_id].fields[F_Philosopher_my_id]).tag == V_INT ? (objects[self_id].fields[F_Philosopher_my_id]).i : (long)((objects[self_id].fields[F_Philosopher_my_id]).f))))});
    objects[self_id].fields[F_Philosopher_state] = mk_int((long)(0L));
  } else {
  }
  enqueue(self_id, self_id, "try_eat", 0, NULL);
}

static void dispatch_Philosopher(int self_id, int sender_id, const char* method, value_t* args, int n_args) {
  if (strcmp(method, "init") == 0) { Philosopher_init(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "try_eat") == 0) { Philosopher_try_eat(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "fork_granted") == 0) { Philosopher_fork_granted(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "fork_denied") == 0) { Philosopher_fork_denied(self_id, sender_id, args, n_args); return; }
  fprintf(stderr, "unknown method %s on Philosopher\n", method);
}

/* WebReceiver: a minimal demo actor reachable from the web bridge.  Its
 * `recv` method just prints the message it was sent, demonstrating a message
 * flowing  Mac actor -> HTTP -> Xinu web server -> AIPL actor mailbox. */
static void dispatch_WebReceiver(int self_id, int sender_id, const char* method,
                                 value_t* args, int n_args) {
  if (strcmp(method, "recv") == 0) {
    wait(print_mu);
    kprintf("[webactor] actor %d got message from %d: ", self_id, sender_id);
    signal(print_mu);
    if (n_args > 0) v_print(args[0]);   /* v_print takes print_mu itself */
    else { wait(print_mu); kprintf("(empty)\r\n"); signal(print_mu); }
    return;
  }
  /* "init" and anything else: no-op */
}

/* ============================================================
 * Load-balancer: Dispatcher + Worker classes.
 *
 *   Dispatcher.register_workers(w0, w1, w2, w3)
 *     Tell the dispatcher which Worker objects it owns.  Sent once
 *     at boot.  Up to LB_N_WORKERS (4) — extras are ignored.
 *
 *   Dispatcher.submit(work_ms, task_id)
 *     Pick the worker with the LEAST outstanding load, increment its
 *     load counter, and forward compute(work_ms, task_id) to it.
 *     Round-robin would be simpler but doesn't react to existing
 *     backlog; least-loaded keeps the slowest worker from piling up.
 *
 *   Dispatcher.done(worker_idx, task_id, result)
 *     Called by a Worker when it finishes a task.  Decrements that
 *     worker's load counter, bumps the dispatcher's completed counter,
 *     and logs the completion to serial.
 *
 *   Worker.bind(my_idx, dispatcher_id)
 *     Tell the worker its own slot index AND who to send `done` to.
 *
 *   Worker.compute(work_ms, task_id)
 *     Simulate work: sleep work_ms, then `result = work_ms*7 + task_id`
 *     (deterministic, easy to verify Mac-side).  Updates own counters,
 *     then enqueues done(my_idx, task_id, result) to the dispatcher.
 *
 * Priorities: Dispatcher=25 (highest) so routing decisions don't get
 * starved by Workers (22) or Philosophers (20).  Workers run above
 * Philosophers so a /api/loadbal/submit burst gets serviced even when
 * the dining demo is at full chatter.
 * ============================================================ */

enum { F_Disp_n, F_Disp_w0, F_Disp_w1, F_Disp_w2, F_Disp_w3,
       F_Disp_load0, F_Disp_load1, F_Disp_load2, F_Disp_load3,
       F_Disp_submitted, F_Disp_completed,
       F_Disp_rr,                     /* round-robin cursor for submit_jit */
       F_Disp_enabled,                /* bitmask: bit i = worker i enabled */
       F_Disp__N };

/* === Task tracking table ===========================================
 * Production load-balancers let callers fire-and-forget then ask
 * later "what happened to task #N?".  We keep a small circular
 * buffer of the most recent LB_TASKS_MAX submissions plus their
 * outcomes (submit-time, done-time, worker, result).
 *
 * Lookup is a linear scan; with N=64 that's trivially fast at the
 * scale we run.  Bigger N would want a hash, but the actor mailbox
 * is already 64-deep so 64 in-flight is a reasonable working set. */
#define LB_TASKS_MAX 64

/* Per-task state for cancellation tracking.
 *   P = pending / running (worker should run it)
 *   D = done (worker finished and reported back)
 *   C = cancelled (worker should abort if still in chunked sleep) */
#define LB_STATE_PENDING   'P'
#define LB_STATE_DONE      'D'
#define LB_STATE_CANCELLED 'C'

typedef struct {
  int  task_id;        /* monotonic from dispatcher; 0 = empty slot */
  int  worker_obj;     /* dispatched-to worker obj_id, -1 if pending */
  long submit_ms;      /* clkticks*10 when dispatched */
  long done_ms;        /* 0 while pending */
  long result;
  char kind;           /* 's' = sleep compute, 'j' = JIT compute */
  char state;          /* LB_STATE_* */
  int  param;          /* work_ms or prog_id */
} lb_task_t;

/* Per-worker queue cap.  Used by the HTTP layer (via
 * abcl_loadbal_can_accept) to return 429 instead of letting the
 * dispatcher mailbox absorb arbitrary backlog. */
#define LB_WORKER_QUEUE_MAX 8

/* Forward decl — lb_latency_bucket uses F_Worker_hist_K constants
 * which aren't declared until the Worker class definition further
 * down, but Dispatcher_done (earlier in the file) needs to call it. */
static int lb_latency_bucket(long elapsed_ms);

static lb_task_t lb_tasks[LB_TASKS_MAX];
static int       lb_tasks_next = 0;     /* round-robin write cursor */

static lb_task_t* lb_task_alloc(int task_id, char kind, int param,
                                int worker_obj) {
  lb_task_t *t = &lb_tasks[lb_tasks_next];
  lb_tasks_next = (lb_tasks_next + 1) % LB_TASKS_MAX;
  t->task_id    = task_id;
  t->worker_obj = worker_obj;
  t->submit_ms  = (long)(clkticks * 10);
  t->done_ms    = 0;
  t->result     = 0;
  t->kind       = kind;
  t->state      = LB_STATE_PENDING;
  t->param      = param;
  return t;
}

static lb_task_t* lb_task_lookup(int task_id) {
  int i;
  for (i = 0; i < LB_TASKS_MAX; i++) {
    if (lb_tasks[i].task_id == task_id) return &lb_tasks[i];
  }
  return NULL;
}

static void init_fields_Dispatcher(int self_id) {
  int i;
  for (i = 0; i < F_Disp__N; i++)
    objects[self_id].fields[i] = mk_int(0L);
  /* Default: all workers enabled (bit 0..3 set). */
  objects[self_id].fields[F_Disp_enabled] = mk_int(0xFL);
}

static void Dispatcher_register_workers(int self_id, int sender_id,
                                        value_t* args, int n_args) {
  (void)sender_id;
  int count = 0;
  if (n_args > 0) { objects[self_id].fields[F_Disp_w0] = mk_obj(args[0].obj_id); count++; }
  if (n_args > 1) { objects[self_id].fields[F_Disp_w1] = mk_obj(args[1].obj_id); count++; }
  if (n_args > 2) { objects[self_id].fields[F_Disp_w2] = mk_obj(args[2].obj_id); count++; }
  if (n_args > 3) { objects[self_id].fields[F_Disp_w3] = mk_obj(args[3].obj_id); count++; }
  objects[self_id].fields[F_Disp_n] = mk_int((long)count);
  wait(print_mu);
  kprintf("[loadbal] dispatcher %d: registered %d workers\r\n", self_id, count);
  signal(print_mu);
}

/* Pick the lowest-load worker that is also enabled in F_Disp_enabled.
 * Returns slot index (0..n-1), or -1 if no worker is enabled. */
static int dispatcher_pick_least_loaded(int self_id) {
  int n = (int)objects[self_id].fields[F_Disp_n].i;
  long mask = objects[self_id].fields[F_Disp_enabled].i;
  int  best = -1;
  long best_load = 0;
  int  i;
  long loads[4] = {
    objects[self_id].fields[F_Disp_load0].i,
    objects[self_id].fields[F_Disp_load1].i,
    objects[self_id].fields[F_Disp_load2].i,
    objects[self_id].fields[F_Disp_load3].i,
  };
  for (i = 0; i < n && i < 4; i++) {
    if (!((mask >> i) & 1L)) continue;            /* paused — skip */
    if (best < 0 || loads[i] < best_load) {
      best = i; best_load = loads[i];
    }
  }
  return best;
}

static int dispatcher_worker_obj(int self_id, int slot) {
  switch (slot) {
  case 0: return objects[self_id].fields[F_Disp_w0].obj_id;
  case 1: return objects[self_id].fields[F_Disp_w1].obj_id;
  case 2: return objects[self_id].fields[F_Disp_w2].obj_id;
  case 3: return objects[self_id].fields[F_Disp_w3].obj_id;
  }
  return -1;
}

static void dispatcher_bump_load(int self_id, int slot, long delta) {
  switch (slot) {
  case 0: objects[self_id].fields[F_Disp_load0] =
            mk_int(objects[self_id].fields[F_Disp_load0].i + delta); break;
  case 1: objects[self_id].fields[F_Disp_load1] =
            mk_int(objects[self_id].fields[F_Disp_load1].i + delta); break;
  case 2: objects[self_id].fields[F_Disp_load2] =
            mk_int(objects[self_id].fields[F_Disp_load2].i + delta); break;
  case 3: objects[self_id].fields[F_Disp_load3] =
            mk_int(objects[self_id].fields[F_Disp_load3].i + delta); break;
  }
}

static void Dispatcher_submit(int self_id, int sender_id,
                              value_t* args, int n_args) {
  (void)sender_id;
  long work_ms = (n_args > 0) ? args[0].i : 100L;
  long task_id = (n_args > 1) ? args[1].i : 0L;
  if ((int)objects[self_id].fields[F_Disp_n].i <= 0) {
    wait(print_mu);
    kprintf("[loadbal] submit dropped: no workers registered\r\n");
    signal(print_mu);
    return;
  }
  int best = dispatcher_pick_least_loaded(self_id);
  if (best < 0) {
    wait(print_mu);
    kprintf("[loadbal] submit dropped task=%ld: all workers paused\r\n",
            task_id);
    signal(print_mu);
    return;
  }
  int worker_obj = dispatcher_worker_obj(self_id, best);
  long old_load = (best == 0) ? objects[self_id].fields[F_Disp_load0].i
                : (best == 1) ? objects[self_id].fields[F_Disp_load1].i
                : (best == 2) ? objects[self_id].fields[F_Disp_load2].i
                              : objects[self_id].fields[F_Disp_load3].i;
  dispatcher_bump_load(self_id, best, +1);
  objects[self_id].fields[F_Disp_submitted] =
    mk_int(objects[self_id].fields[F_Disp_submitted].i + 1);

  /* Record task for /api/loadbal/task?id= later. */
  lb_task_alloc((int)task_id, 's', (int)work_ms, worker_obj);

  wait(print_mu);
  kprintf("[loadbal] dispatch task=%ld -> worker idx=%d obj=%d (load %ld->%ld)\r\n",
          task_id, best, worker_obj, old_load, old_load + 1);
  signal(print_mu);

  enqueue(self_id, worker_obj, "compute", 2,
          (value_t[]){ mk_int(work_ms), mk_int(task_id) });
}

static void Dispatcher_done(int self_id, int sender_id,
                            value_t* args, int n_args) {
  (void)sender_id;
  long widx    = (n_args > 0) ? args[0].i : -1L;
  long task_id = (n_args > 1) ? args[1].i : -1L;
  long result  = (n_args > 2) ? args[2].i : 0L;
  dispatcher_bump_load(self_id, (int)widx, -1);
  objects[self_id].fields[F_Disp_completed] =
    mk_int(objects[self_id].fields[F_Disp_completed].i + 1);
  /* Stamp the task table + bump that worker's latency-histogram
   * bucket — visible via /api/loadbal/stats. */
  lb_task_t *t = lb_task_lookup((int)task_id);
  long elapsed = 0;
  if (NULL != t) {
    t->done_ms = (long)(clkticks * 10);
    t->result  = result;
    /* Don't overwrite a CANCELLED state — the worker explicitly
     * acknowledged the cancel.  Otherwise mark DONE. */
    if (t->state != LB_STATE_CANCELLED) t->state = LB_STATE_DONE;
    elapsed = t->done_ms - t->submit_ms;
    if (elapsed < 0) elapsed = 0;
    int worker_obj = dispatcher_worker_obj(self_id, (int)widx);
    if (worker_obj >= 0) {
      int bkt = lb_latency_bucket(elapsed);
      objects[worker_obj].fields[bkt] =
        mk_int(objects[worker_obj].fields[bkt].i + 1);
    }
  }
  wait(print_mu);
  kprintf("[loadbal] complete task=%ld worker=%ld result=%ld elapsed=%ld (done=%ld/%ld)\r\n",
          task_id, widx, result, elapsed,
          objects[self_id].fields[F_Disp_completed].i,
          objects[self_id].fields[F_Disp_submitted].i);
  signal(print_mu);
}

/* Mark a task CANCELLED.  The worker's compute loop polls task state
 * during its chunked sleep — see Worker_compute.  For JIT tasks
 * cancel only catches the BEFORE-compile window because the JIT call
 * itself is synchronous.  HTTP: POST /api/loadbal/cancel?id=N */
static void Dispatcher_cancel_task(int self_id, int sender_id,
                                   value_t* args, int n_args) {
  (void)self_id; (void)sender_id;
  if (n_args < 1) return;
  long task_id = args[0].i;
  lb_task_t *t = lb_task_lookup((int)task_id);
  if (NULL == t || t->task_id == 0) {
    wait(print_mu);
    kprintf("[loadbal] cancel: task %ld not in table\r\n", task_id);
    signal(print_mu);
    return;
  }
  if (t->state == LB_STATE_DONE) {
    wait(print_mu);
    kprintf("[loadbal] cancel: task %ld already DONE — ignored\r\n", task_id);
    signal(print_mu);
    return;
  }
  t->state = LB_STATE_CANCELLED;
  wait(print_mu);
  kprintf("[loadbal] cancel: task %ld marked CANCELLED\r\n", task_id);
  signal(print_mu);
}

/* Toggle a worker's enabled bit.  args[0] = slot, args[1] = on (0/1).
 * Paused workers receive no new dispatches; in-flight tasks complete
 * normally (load counter still decrements via done). */
static void Dispatcher_set_enabled(int self_id, int sender_id,
                                   value_t* args, int n_args) {
  (void)sender_id;
  if (n_args < 2) return;
  long slot = args[0].i;
  long on   = args[1].i;
  if (slot < 0 || slot >= LB_N_WORKERS) return;
  long mask = objects[self_id].fields[F_Disp_enabled].i;
  if (on) mask |=  (1L << slot);
  else    mask &= ~(1L << slot);
  objects[self_id].fields[F_Disp_enabled] = mk_int(mask);
  wait(print_mu);
  kprintf("[loadbal] worker idx=%ld %s (mask=0x%lx)\r\n",
          slot, on ? "RESUMED" : "PAUSED", mask);
  signal(print_mu);
}

/* Sticky routing: caller passes a precomputed hash, dispatcher maps
 * hash%n_workers to a slot.  Same hash -> same slot (so a cluster of
 * tasks sharing a key all land on the same worker, useful for cache
 * locality or stateful workers).  If that slot is paused, walk
 * forward to the next enabled one — preserves sticky semantics when
 * the target is alive, gracefully degrades when not. */
static void Dispatcher_submit_sticky(int self_id, int sender_id,
                                     value_t* args, int n_args) {
  (void)sender_id;
  long work_ms  = (n_args > 0) ? args[0].i : 100L;
  long task_id  = (n_args > 1) ? args[1].i : 0L;
  long key_hash = (n_args > 2) ? args[2].i : 0L;
  int n = (int)objects[self_id].fields[F_Disp_n].i;
  if (n <= 0) return;
  long mask = objects[self_id].fields[F_Disp_enabled].i;
  unsigned long h = (unsigned long)(key_hash < 0 ? -key_hash : key_hash);
  int slot = (int)(h % (unsigned long)n);
  int tries;
  for (tries = 0; tries < n; tries++) {
    if ((mask >> slot) & 1L) break;
    slot = (slot + 1) % n;
  }
  if (!((mask >> slot) & 1L)) {
    wait(print_mu);
    kprintf("[loadbal] submit_sticky dropped task=%ld key_hash=%ld: all paused\r\n",
            task_id, key_hash);
    signal(print_mu);
    return;
  }
  int worker_obj = dispatcher_worker_obj(self_id, slot);
  dispatcher_bump_load(self_id, slot, +1);
  objects[self_id].fields[F_Disp_submitted] =
    mk_int(objects[self_id].fields[F_Disp_submitted].i + 1);
  lb_task_alloc((int)task_id, 's', (int)work_ms, worker_obj);
  wait(print_mu);
  kprintf("[loadbal] sticky task=%ld key_hash=%ld -> worker idx=%d obj=%d\r\n",
          task_id, key_hash, slot, worker_obj);
  signal(print_mu);
  enqueue(self_id, worker_obj, "compute", 2,
          (value_t[]){ mk_int(work_ms), mk_int(task_id) });
}

/* N-Queens dispatch — round-robin first_col across workers.  Same
 * routing as submit_jit (uses F_Disp_rr cursor).  Each task receives
 * (n, first_col, task_id), worker calls abcl_nq_count_partial. */
static void Dispatcher_submit_nq(int self_id, int sender_id,
                                 value_t* args, int n_args) {
  (void)sender_id;
  long n         = (n_args > 0) ? args[0].i : 8L;
  long first_col = (n_args > 1) ? args[1].i : 0L;
  long task_id   = (n_args > 2) ? args[2].i : 0L;
  int n_workers = (int)objects[self_id].fields[F_Disp_n].i;
  if (n_workers <= 0) return;
  long mask = objects[self_id].fields[F_Disp_enabled].i;
  int slot = -1, tries;
  for (tries = 0; tries < n_workers; tries++) {
    int s = (int)(objects[self_id].fields[F_Disp_rr].i % n_workers);
    objects[self_id].fields[F_Disp_rr] =
      mk_int(objects[self_id].fields[F_Disp_rr].i + 1);
    if ((mask >> s) & 1L) { slot = s; break; }
  }
  if (slot < 0) return;
  int worker_obj = dispatcher_worker_obj(self_id, slot);
  dispatcher_bump_load(self_id, slot, +1);
  objects[self_id].fields[F_Disp_submitted] =
    mk_int(objects[self_id].fields[F_Disp_submitted].i + 1);
  lb_task_alloc((int)task_id, 'n', (int)first_col, worker_obj);
  enqueue(self_id, worker_obj, "compute_nq", 3,
          (value_t[]){ mk_int(n), mk_int(first_col), mk_int(task_id) });
}

/* Worker restart — kill old thread, allocate fresh actor, swap into
 * the dispatcher's worker slot.  Uses a NEW obj_id (n_objects bumps),
 * which is permanent — restarting all 4 workers consumes 4 of the 9
 * post-boot MAX_OBJECTS=16 budget.  Acceptable for occasional ops
 * use; a long-running restart loop would exhaust the table.
 *
 * Pending tasks in the old worker's mailbox are LOST (still counted
 * as submitted-not-completed forever).  Done-callbacks from the old
 * worker after kill are also lost.  The load counter is reset to 0
 * for the slot to reflect the fresh start. */
static void Dispatcher_submit_nq(int self_id, int sender_id,
                                 value_t* args, int n_args);
int abcl_nq_count_partial(int n, int first_col);

static void Dispatcher_restart_worker(int self_id, int sender_id,
                                      value_t* args, int n_args) {
  (void)sender_id;
  if (n_args < 1) return;
  long slot = args[0].i;
  if (slot < 0 || slot >= LB_N_WORKERS) return;
  if (n_objects >= MAX_OBJECTS) {
    wait(print_mu);
    kprintf("[loadbal] restart_worker idx=%ld FAILED: actor table full (%d/%d)\r\n",
            slot, n_objects, MAX_OBJECTS);
    signal(print_mu);
    return;
  }
  int old_obj = dispatcher_worker_obj(self_id, (int)slot);
  if (old_obj > 0 && objects[old_obj].tid > 0 && !objects[old_obj].dead) {
    kill(objects[old_obj].tid);
    objects[old_obj].dead = 1;
  }
  int new_obj = alloc_obj(CLASS_Worker, 0, NULL);
  abcl_object_protect(new_obj, 1);
  spawn_actor(new_obj);
  /* Swap into dispatcher's slot + reset load counter. */
  switch ((int)slot) {
  case 0: objects[self_id].fields[F_Disp_w0] = mk_obj(new_obj);
          objects[self_id].fields[F_Disp_load0] = mk_int(0L); break;
  case 1: objects[self_id].fields[F_Disp_w1] = mk_obj(new_obj);
          objects[self_id].fields[F_Disp_load1] = mk_int(0L); break;
  case 2: objects[self_id].fields[F_Disp_w2] = mk_obj(new_obj);
          objects[self_id].fields[F_Disp_load2] = mk_int(0L); break;
  case 3: objects[self_id].fields[F_Disp_w3] = mk_obj(new_obj);
          objects[self_id].fields[F_Disp_load3] = mk_int(0L); break;
  }
  g_loadbal_w[(int)slot] = new_obj;
  /* Bind the fresh worker so it knows its slot index + dispatcher. */
  abcl_enqueue(-1, new_obj, "bind", 2,
               (value_t[]){ mk_int(slot), mk_obj(self_id) });
  wait(print_mu);
  kprintf("[loadbal] worker idx=%ld restarted: old_obj=%d -> new_obj=%d\r\n",
          slot, old_obj, new_obj);
  signal(print_mu);
}

/* Routes a JIT job using round-robin instead of least-loaded.
 *
 * For sleep-based `submit` the work is slower than dispatch, so by the
 * time the second submit arrives worker 0 still has load=1 and worker 1
 * wins — distribution looks even.
 *
 * For JIT, each task is fast enough that the worker often finishes (and
 * sends `done` decrementing its load) BEFORE the next submit arrives.
 * Least-loaded then breaks ties in favour of slot 0, so every JIT task
 * lands on the same worker.  Round-robin sidesteps this and gives an
 * honest demonstration of 4 worker threads JIT-compiling in parallel. */
static void Dispatcher_submit_jit(int self_id, int sender_id,
                                  value_t* args, int n_args) {
  (void)sender_id;
  long prog_id = (n_args > 0) ? args[0].i : 0L;
  long task_id = (n_args > 1) ? args[1].i : 0L;
  int n = (int)objects[self_id].fields[F_Disp_n].i;
  if (n <= 0) return;
  long mask = objects[self_id].fields[F_Disp_enabled].i;
  /* Round-robin among ENABLED workers only — paused workers are
   * skipped so a /api/loadbal/pause during a JIT burst still drains
   * cleanly to the survivors. */
  int slot = -1, tries;
  for (tries = 0; tries < n; tries++) {
    int s = (int)(objects[self_id].fields[F_Disp_rr].i % n);
    objects[self_id].fields[F_Disp_rr] =
      mk_int(objects[self_id].fields[F_Disp_rr].i + 1);
    if ((mask >> s) & 1L) { slot = s; break; }
  }
  if (slot < 0) {
    wait(print_mu);
    kprintf("[loadbal] submit_jit dropped task=%ld: all workers paused\r\n",
            task_id);
    signal(print_mu);
    return;
  }
  int worker_obj = dispatcher_worker_obj(self_id, slot);
  dispatcher_bump_load(self_id, slot, +1);
  objects[self_id].fields[F_Disp_submitted] =
    mk_int(objects[self_id].fields[F_Disp_submitted].i + 1);

  lb_task_alloc((int)task_id, 'j', (int)prog_id, worker_obj);

  enqueue(self_id, worker_obj, "compute_jit", 2,
          (value_t[]){ mk_int(prog_id), mk_int(task_id) });
}

static void dispatch_Dispatcher(int self_id, int sender_id, const char* method,
                                value_t* args, int n_args) {
  if (strcmp(method, "register_workers") == 0)
  { Dispatcher_register_workers(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "submit") == 0)
  { Dispatcher_submit(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "submit_jit") == 0)
  { Dispatcher_submit_jit(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "submit_sticky") == 0)
  { Dispatcher_submit_sticky(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "done") == 0)
  { Dispatcher_done(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "set_enabled") == 0)
  { Dispatcher_set_enabled(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "cancel_task") == 0)
  { Dispatcher_cancel_task(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "restart_worker") == 0)
  { Dispatcher_restart_worker(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "submit_nq") == 0)
  { Dispatcher_submit_nq(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "init") == 0) return;
}

enum { F_Worker_my_idx, F_Worker_dispatcher, F_Worker_done,
       F_Worker_busy_ms, F_Worker_last_n, F_Worker_last_result,
       /* Latency histogram: 7 buckets.  Caller (Dispatcher_done)
        * computes elapsed_ms = done_ms - submit_ms and bumps the
        * right bucket on the worker.  These give a coarse p50/p95
        * read without a full sorted list. */
       F_Worker_hist_0,   /* <50 ms */
       F_Worker_hist_1,   /* 50-99 ms */
       F_Worker_hist_2,   /* 100-199 ms */
       F_Worker_hist_3,   /* 200-499 ms */
       F_Worker_hist_4,   /* 500-999 ms */
       F_Worker_hist_5,   /* 1000-1999 ms */
       F_Worker_hist_6,   /* >=2000 ms */
       F_Worker__N };

/* Map elapsed_ms to a histogram bucket field index (F_Worker_hist_0..6). */
static int lb_latency_bucket(long elapsed_ms) {
  if (elapsed_ms < 50)   return F_Worker_hist_0;
  if (elapsed_ms < 100)  return F_Worker_hist_1;
  if (elapsed_ms < 200)  return F_Worker_hist_2;
  if (elapsed_ms < 500)  return F_Worker_hist_3;
  if (elapsed_ms < 1000) return F_Worker_hist_4;
  if (elapsed_ms < 2000) return F_Worker_hist_5;
  return F_Worker_hist_6;
}

static void init_fields_Worker(int self_id) {
  int i;
  for (i = 0; i < F_Worker__N; i++)
    objects[self_id].fields[i] = mk_int(0L);
}

static void Worker_bind(int self_id, int sender_id,
                        value_t* args, int n_args) {
  (void)sender_id;
  if (n_args < 2) return;
  objects[self_id].fields[F_Worker_my_idx]     = mk_int(args[0].i);
  objects[self_id].fields[F_Worker_dispatcher] = mk_obj(args[1].obj_id);
  wait(print_mu);
  kprintf("[loadbal] worker %d bound idx=%ld dispatcher=%d\r\n",
          self_id, args[0].i, args[1].obj_id);
  signal(print_mu);
}

static void Worker_compute(int self_id, int sender_id,
                           value_t* args, int n_args) {
  (void)sender_id;
  long work_ms = (n_args > 0) ? args[0].i : 0L;
  long task_id = (n_args > 1) ? args[1].i : 0L;
  long idx     = objects[self_id].fields[F_Worker_my_idx].i;
  int  disp    = objects[self_id].fields[F_Worker_dispatcher].obj_id;

  wait(print_mu);
  kprintf("[loadbal] worker idx=%ld obj=%d task=%ld start (%ld ms)\r\n",
          idx, self_id, task_id, work_ms);
  signal(print_mu);

  /* Chunked sleep so /api/loadbal/cancel?id=N can interrupt mid-task.
   * 50 ms chunks give cancel a worst-case ~50 ms reaction time, which
   * is fine alongside our HTTP RTT of ~5 ms.  task_aborted flips to 1
   * if the task table entry is marked CANCELLED. */
  int  task_aborted = 0;
  long slept = 0;
  while (slept < work_ms) {
    lb_task_t *t = lb_task_lookup((int)task_id);
    if (NULL != t && t->state == LB_STATE_CANCELLED) {
      task_aborted = 1;
      break;
    }
    long chunk = (work_ms - slept) > 50 ? 50 : (work_ms - slept);
    sleep((uint)chunk);
    slept += chunk;
  }

  long result = task_aborted ? -1L : (work_ms * 7L + task_id);
  objects[self_id].fields[F_Worker_last_n]      = mk_int(work_ms);
  objects[self_id].fields[F_Worker_last_result] = mk_int(result);
  objects[self_id].fields[F_Worker_done] =
    mk_int(objects[self_id].fields[F_Worker_done].i + 1);
  objects[self_id].fields[F_Worker_busy_ms] =
    mk_int(objects[self_id].fields[F_Worker_busy_ms].i + slept);

  wait(print_mu);
  if (task_aborted) {
    kprintf("[loadbal] worker idx=%ld task=%ld CANCELLED at %ld/%ld ms\r\n",
            idx, task_id, slept, work_ms);
  } else {
    kprintf("[loadbal] worker idx=%ld task=%ld done result=%ld\r\n",
            idx, task_id, result);
  }
  signal(print_mu);

  enqueue(self_id, disp, "done", 3,
          (value_t[]){ mk_int(idx), mk_int(task_id), mk_int(result) });
}

/* Static JIT-test programs.  cc_mvp doesn't have local vars yet so the
 * source must be self-contained (literals + builtin calls only).
 * Each worker JIT-compiles+executes one of these per task, exercising
 * memget/memfree, codegen, and the builtin-call ABI under load. */
static const char *jit_progs[] = {
  "int main() { return 42; }",                              /* trivial */
  "int main() { return 100 + 23; }",                        /* binop  */
  "int main() { return now_ms(); }",                        /* builtin */
  "int main() { return actor_count() + 1000; }",            /* builtin+binop */
  "int main() { return now_ms() + actor_count(); }",        /* 2 builtins */
};
#define N_JIT_PROGS ((int)(sizeof(jit_progs) / sizeof(jit_progs[0])))

static void Worker_compute_jit(int self_id, int sender_id,
                               value_t* args, int n_args) {
  (void)sender_id;
  long prog_id = (n_args > 0) ? args[0].i : 0L;
  long task_id = (n_args > 1) ? args[1].i : 0L;
  if (prog_id < 0 || prog_id >= N_JIT_PROGS) prog_id = 0;
  long idx  = objects[self_id].fields[F_Worker_my_idx].i;
  int  disp = objects[self_id].fields[F_Worker_dispatcher].obj_id;

  /* Cancellation window: only effective BEFORE the JIT call.  The
   * compile+execute itself is synchronous and cannot be interrupted
   * from the dispatcher side. */
  {
    lb_task_t *t = lb_task_lookup((int)task_id);
    if (NULL != t && t->state == LB_STATE_CANCELLED) {
      wait(print_mu);
      kprintf("[loadbal/jit] worker idx=%ld task=%ld CANCELLED before run\r\n",
              idx, task_id);
      signal(print_mu);
      enqueue(self_id, disp, "done", 3,
              (value_t[]){ mk_int(idx), mk_int(task_id), mk_int(-1L) });
      return;
    }
  }

  extern int cc_mvp_compile_and_run(const char *, long *, int *);
  long result = 0;
  int  code_size = 0;
  int  rc = cc_mvp_compile_and_run(jit_progs[(int)prog_id],
                                   &result, &code_size);

  wait(print_mu);
  if (rc == 0) {
    kprintf("[loadbal/jit] worker idx=%ld task=%ld prog=%ld result=%ld bytes=%d\r\n",
            idx, task_id, prog_id, result, code_size);
  } else {
    kprintf("[loadbal/jit] worker idx=%ld task=%ld prog=%ld FAILED rc=%d\r\n",
            idx, task_id, prog_id, rc);
    result = -1;
  }
  signal(print_mu);

  objects[self_id].fields[F_Worker_last_n]      = mk_int(prog_id);
  objects[self_id].fields[F_Worker_last_result] = mk_int(result);
  objects[self_id].fields[F_Worker_done] =
    mk_int(objects[self_id].fields[F_Worker_done].i + 1);
  /* For JIT tasks, busy_ms re-purposed as cumulative code bytes emitted. */
  objects[self_id].fields[F_Worker_busy_ms] =
    mk_int(objects[self_id].fields[F_Worker_busy_ms].i + code_size);

  enqueue(self_id, disp, "done", 3,
          (value_t[]){ mk_int(idx), mk_int(task_id), mk_int(result) });
}

/* N-Queens partial work — computes nqueens_count_partial(n, first_col)
 * on this worker thread, reports the solution count as `result` via
 * done.  Used by the /api/loadbal/nqueens demo / benchmark. */
static void Worker_compute_nq(int self_id, int sender_id,
                              value_t* args, int n_args) {
  (void)sender_id;
  long n         = (n_args > 0) ? args[0].i : 8L;
  long first_col = (n_args > 1) ? args[1].i : 0L;
  long task_id   = (n_args > 2) ? args[2].i : 0L;
  long idx       = objects[self_id].fields[F_Worker_my_idx].i;
  int  disp      = objects[self_id].fields[F_Worker_dispatcher].obj_id;

  long t0 = (long)(clkticks * 10);
  long result = (long)abcl_nq_count_partial((int)n, (int)first_col);
  long elapsed = (long)(clkticks * 10) - t0;

  objects[self_id].fields[F_Worker_last_n]      = mk_int(n);
  objects[self_id].fields[F_Worker_last_result] = mk_int(result);
  objects[self_id].fields[F_Worker_done] =
    mk_int(objects[self_id].fields[F_Worker_done].i + 1);
  objects[self_id].fields[F_Worker_busy_ms] =
    mk_int(objects[self_id].fields[F_Worker_busy_ms].i + elapsed);

  wait(print_mu);
  kprintf("[loadbal/nq] worker idx=%ld task=%ld nq(%ld,%ld)=%ld in %ldms\r\n",
          idx, task_id, n, first_col, result, elapsed);
  signal(print_mu);

  enqueue(self_id, disp, "done", 3,
          (value_t[]){ mk_int(idx), mk_int(task_id), mk_int(result) });
}

static void dispatch_Worker(int self_id, int sender_id, const char* method,
                            value_t* args, int n_args) {
  if (strcmp(method, "bind") == 0)
  { Worker_bind(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "compute") == 0)
  { Worker_compute(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "compute_jit") == 0)
  { Worker_compute_jit(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "compute_nq") == 0)
  { Worker_compute_nq(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "init") == 0) return;
}

/* === accessors used by apps/webactor.c =================================== */
int abcl_loadbal_dispatcher_id(void) { return g_loadbal_disp; }
int abcl_loadbal_worker_count(void)  { return LB_N_WORKERS; }
int abcl_loadbal_worker_id(int idx) {
  if (idx < 0 || idx >= LB_N_WORKERS) return -1;
  return g_loadbal_w[idx];
}
void abcl_loadbal_submit(int work_ms, int task_id) {
  if (g_loadbal_disp < 0) return;
  abcl_enqueue(-1, g_loadbal_disp, "submit", 2,
               (value_t[]){ mk_int((long)work_ms), mk_int((long)task_id) });
}

void abcl_loadbal_submit_jit(int prog_id, int task_id) {
  if (g_loadbal_disp < 0) return;
  abcl_enqueue(-1, g_loadbal_disp, "submit_jit", 2,
               (value_t[]){ mk_int((long)prog_id), mk_int((long)task_id) });
}
int abcl_loadbal_n_progs(void) { return N_JIT_PROGS; }

void abcl_loadbal_set_enabled(int worker_idx, int on) {
  if (g_loadbal_disp < 0) return;
  abcl_enqueue(-1, g_loadbal_disp, "set_enabled", 2,
               (value_t[]){ mk_int((long)worker_idx), mk_int((long)on) });
}

void abcl_loadbal_cancel(int task_id) {
  if (g_loadbal_disp < 0) return;
  abcl_enqueue(-1, g_loadbal_disp, "cancel_task", 1,
               (value_t[]){ mk_int((long)task_id) });
}

void abcl_loadbal_submit_sticky(int work_ms, int task_id, int key_hash) {
  if (g_loadbal_disp < 0) return;
  abcl_enqueue(-1, g_loadbal_disp, "submit_sticky", 3,
               (value_t[]){ mk_int((long)work_ms),
                            mk_int((long)task_id),
                            mk_int((long)key_hash) });
}

void abcl_loadbal_restart_worker(int slot) {
  if (g_loadbal_disp < 0) return;
  abcl_enqueue(-1, g_loadbal_disp, "restart_worker", 1,
               (value_t[]){ mk_int((long)slot) });
}

void abcl_loadbal_submit_nq(int n, int first_col, int task_id) {
  if (g_loadbal_disp < 0) return;
  abcl_enqueue(-1, g_loadbal_disp, "submit_nq", 3,
               (value_t[]){ mk_int((long)n), mk_int((long)first_col),
                            mk_int((long)task_id) });
}

/* N-Queens partial counter — counts solutions where the FIRST row's
 * queen is placed at column `first_col`.  Caller sums over
 * first_col=0..n-1 to get the total count.  Embarrassingly parallel:
 * each first_col job is independent and they're roughly equal in
 * cost (modulo board symmetry), making it a clean load-balance demo.
 *
 * O(n!) worst case, but the branching prune cuts it sharply.  For
 * n=8 the total is 92 solutions and the work fits in < 100 ms even
 * on a single Pi 3 worker.  For n=11 it's 2680 solutions and starts
 * to be visible (~seconds). */
static int nq_recurse(int n, int row, int *cols) {
  if (row == n) return 1;
  int count = 0, c;
  for (c = 0; c < n; c++) {
    int ok = 1;
    int r;
    for (r = 0; r < row; r++) {
      int dc = cols[r] - c;
      if (dc < 0) dc = -dc;
      if (cols[r] == c || dc == row - r) { ok = 0; break; }
    }
    if (ok) {
      cols[row] = c;
      count += nq_recurse(n, row + 1, cols);
    }
  }
  return count;
}

int abcl_nq_count_partial(int n, int first_col) {
  int cols[16];
  if (n < 1 || n > 16) return 0;
  if (first_col < 0 || first_col >= n) return 0;
  cols[0] = first_col;
  return nq_recurse(n, 1, cols);
}

/* djb2 — small portable string hash for sticky routing keys.
 * Caller passes the resulting int to abcl_loadbal_submit_sticky. */
int abcl_loadbal_hash_key(const char *s, int len) {
  unsigned long h = 5381;
  int i;
  for (i = 0; i < len; i++)
    h = (h * 33u) + (unsigned char)s[i];
  return (int)(h & 0x7FFFFFFFu);
}

/* Token-bucket rate limit.  Refills based on elapsed clkticks (100 Hz)
 * since the last call; bucket caps at capacity.  Returns 1 if a token
 * was deducted (caller may proceed) or 0 if throttled.  Bypassed
 * entirely when caller passes priority="high" — see webactor's /submit
 * route.  All state in file-scope statics above. */
int abcl_loadbal_rate_check(void) {
  long now = (long)clkticks;
  long elapsed = now - g_rate_last_refill;
  if (elapsed > 0) {
    /* refill = (elapsed_ticks * per_sec) / 100  [ticks-per-second] */
    long refill = (elapsed * g_rate_per_sec) / 100;
    if (refill > 0) {
      g_rate_tokens += refill;
      if (g_rate_tokens > g_rate_capacity) g_rate_tokens = g_rate_capacity;
      g_rate_last_refill = now;
    }
  }
  if (g_rate_tokens >= 1) {
    g_rate_tokens--;
    return 1;
  }
  g_rate_throttled++;
  return 0;
}

void abcl_loadbal_rate_set(int per_sec, int capacity) {
  if (per_sec  > 0) g_rate_per_sec  = (long)per_sec;
  if (capacity > 0) g_rate_capacity = (long)capacity;
  if (g_rate_tokens > g_rate_capacity) g_rate_tokens = g_rate_capacity;
}

int abcl_loadbal_rate_stats(char *buf, int cap) {
  if (cap < 200) return 0;
  return sprintf(buf,
    "rate_limit: per_sec=%ld capacity=%ld tokens=%ld throttled=%ld\n"
    "task_timeout: ms=%ld auto_cancelled=%ld\n",
    g_rate_per_sec, g_rate_capacity, g_rate_tokens, g_rate_throttled,
    g_task_timeout_ms, g_task_timed_out);
}

void abcl_loadbal_timeout_set(int ms) {
  if (ms >= 0) g_task_timeout_ms = (long)ms;
}

/* Walk the task table; any PENDING task whose age exceeds
 * g_task_timeout_ms gets marked CANCELLED.  Called from Collector.tick
 * so it shares the GC actor's heartbeat (no extra thread).  Returns
 * the number freshly expired this call (for kprintf in caller). */
int abcl_loadbal_timeout_scan(void) {
  if (g_task_timeout_ms <= 0) return 0;
  long now_ms = (long)(clkticks * 10);
  int  expired = 0;
  int  i;
  for (i = 0; i < LB_TASKS_MAX; i++) {
    lb_task_t *t = &lb_tasks[i];
    if (t->task_id == 0) continue;
    if (t->state != LB_STATE_PENDING) continue;
    long age = now_ms - t->submit_ms;
    if (age < g_task_timeout_ms) continue;
    t->state = LB_STATE_CANCELLED;
    expired++;
    g_task_timed_out++;
  }
  return expired;
}

/* HTTP backpressure check.  Returns 1 if at least one enabled worker
 * has load < LB_WORKER_QUEUE_MAX, else 0.  Inherently racey (load may
 * change between this check and the subsequent submit) but good
 * enough to refuse runaway submitters at the HTTP boundary. */
int abcl_loadbal_can_accept(void) {
  if (g_loadbal_disp < 0) return 0;
  int  d = g_loadbal_disp;
  int  n = (int)objects[d].fields[F_Disp_n].i;
  long mask = objects[d].fields[F_Disp_enabled].i;
  long loads[4] = {
    objects[d].fields[F_Disp_load0].i,
    objects[d].fields[F_Disp_load1].i,
    objects[d].fields[F_Disp_load2].i,
    objects[d].fields[F_Disp_load3].i,
  };
  int i;
  for (i = 0; i < n && i < 4; i++) {
    if (!((mask >> i) & 1L)) continue;
    if (loads[i] < LB_WORKER_QUEUE_MAX) return 1;
  }
  return 0;
}

/* Fill `buf` with one task row for /api/loadbal/task?id=N.
 * Returns bytes written, or 0 if no such task. */
int abcl_loadbal_task_info(int task_id, char *buf, int cap) {
  if (cap < 200) return 0;
  lb_task_t *t = lb_task_lookup(task_id);
  if (NULL == t || t->task_id == 0) {
    return sprintf(buf, "task_id=%d not_found\n", task_id);
  }
  long elapsed = (t->done_ms > 0) ? (t->done_ms - t->submit_ms) : -1L;
  const char *state_str =
    (t->state == LB_STATE_CANCELLED) ? "CANCELLED" :
    (t->state == LB_STATE_DONE     ) ? "DONE"      :
                                       "PENDING";
  return sprintf(buf,
    "task_id=%d worker_obj=%d kind=%c param=%d\n"
    "submit_ms=%ld done_ms=%ld elapsed_ms=%ld\n"
    "state=%s result=%ld\n",
    t->task_id, t->worker_obj, t->kind, t->param,
    t->submit_ms, t->done_ms, elapsed,
    state_str, t->result);
}

/* One-shot bootstrap of the load-balancer actors.  Safe to call multiple
 * times — only the first call creates the actors.  Called from
 * webactor_autostart so the load-balancer exists even when aipl_main
 * isn't running (the default boot path runs only the AIPL-RPC + HTTP
 * servers, not the full dining demo). */
static int g_loadbal_inited = 0;
void abcl_loadbal_init(void) {
  if (g_loadbal_inited) return;
  /* Runtime mutexes must already be up — abcl_web_init() / abcl_rt_init()
   * sets them.  We don't call abcl_rt_init_once() here so the caller
   * controls ordering. */
  int i;
  g_loadbal_disp = alloc_obj(CLASS_Dispatcher, 0, NULL);
  for (i = 0; i < LB_N_WORKERS; i++)
    g_loadbal_w[i] = alloc_obj(CLASS_Worker, 0, NULL);
  /* Spawn the threads BEFORE sending bind/register so the messages
   * have a thread to consume them. */
  spawn_actor(g_loadbal_disp);
  for (i = 0; i < LB_N_WORKERS; i++) spawn_actor(g_loadbal_w[i]);
  /* Bind each worker to its slot + dispatcher (queued) */
  for (i = 0; i < LB_N_WORKERS; i++) {
    abcl_enqueue(-1, g_loadbal_w[i], "bind", 2,
                 (value_t[]){ mk_int((long)i), mk_obj(g_loadbal_disp) });
  }
  /* Register all workers in dispatcher */
  abcl_enqueue(-1, g_loadbal_disp, "register_workers", LB_N_WORKERS,
               (value_t[]){ mk_obj(g_loadbal_w[0]), mk_obj(g_loadbal_w[1]),
                            mk_obj(g_loadbal_w[2]), mk_obj(g_loadbal_w[3]) });
  g_loadbal_inited = 1;
  wait(print_mu);
  kprintf("[loadbal] init done: dispatcher=%d workers=%d,%d,%d,%d\r\n",
          g_loadbal_disp,
          g_loadbal_w[0], g_loadbal_w[1], g_loadbal_w[2], g_loadbal_w[3]);
  signal(print_mu);
}

/* Fills `buf` with a one-line-per-row snapshot of dispatcher + workers.
 * Caller-allocated; cap must be >= 512.  Returns bytes written. */
int abcl_loadbal_stats(char *buf, int cap) {
  if (cap < 512 || g_loadbal_disp < 0) {
    return sprintf(buf, "loadbal not initialized\n");
  }
  int d = g_loadbal_disp;
  int n = (int)objects[d].fields[F_Disp_n].i;
  long mask = objects[d].fields[F_Disp_enabled].i;
  int blen = sprintf(buf,
    "dispatcher obj=%d workers=%d submitted=%ld completed=%ld\n"
    "enabled_mask=0x%lx (1=on per worker bit; 0xF = all four enabled)\n"
    "loads: w0=%ld w1=%ld w2=%ld w3=%ld\n",
    d, n,
    objects[d].fields[F_Disp_submitted].i,
    objects[d].fields[F_Disp_completed].i,
    mask,
    objects[d].fields[F_Disp_load0].i,
    objects[d].fields[F_Disp_load1].i,
    objects[d].fields[F_Disp_load2].i,
    objects[d].fields[F_Disp_load3].i);
  int i;
  blen += sprintf(buf + blen,
                  "idx obj done busy_ms last_n last_result\n");
  for (i = 0; i < LB_N_WORKERS; i++) {
    int wid = g_loadbal_w[i];
    if (wid < 0) continue;
    blen += sprintf(buf + blen, "%d %d %ld %ld %ld %ld\n",
      i, wid,
      objects[wid].fields[F_Worker_done].i,
      objects[wid].fields[F_Worker_busy_ms].i,
      objects[wid].fields[F_Worker_last_n].i,
      objects[wid].fields[F_Worker_last_result].i);
  }
  /* Latency histograms — bucket counts per worker.  Buckets:
   *   b0:<50  b1:50-99  b2:100-199  b3:200-499  b4:500-999
   *   b5:1000-1999  b6:>=2000  (all ms) */
  blen += sprintf(buf + blen,
    "idx hist_lt50 50-99 100-199 200-499 500-999 1k-2k ge2k\n");
  for (i = 0; i < LB_N_WORKERS; i++) {
    int wid = g_loadbal_w[i];
    if (wid < 0) continue;
    blen += sprintf(buf + blen, "%d %ld %ld %ld %ld %ld %ld %ld\n", i,
      objects[wid].fields[F_Worker_hist_0].i,
      objects[wid].fields[F_Worker_hist_1].i,
      objects[wid].fields[F_Worker_hist_2].i,
      objects[wid].fields[F_Worker_hist_3].i,
      objects[wid].fields[F_Worker_hist_4].i,
      objects[wid].fields[F_Worker_hist_5].i,
      objects[wid].fields[F_Worker_hist_6].i);
  }
  return blen;
}

/* ============================================================
 * Collector — periodic actor garbage-collector, implemented as
 * an AIPL actor (not a kernel thread).  It owns a mailbox; an
 * external heartbeat thread (abcl_gc_heartbeat) sends it "tick"
 * messages at GC_HEARTBEAT_MS granularity, and tick handler
 * decides whether `period_ms` has elapsed since the last sweep.
 * When it does, it calls the existing abcl_gc_sweep() (which
 * was already used by the manual /gc HTTP route).
 *
 * Configuration: tweak via Dispatcher-style messages —
 *   configure(period_ms, threshold_ms)
 *   enable(on)
 *   sweep_now
 * Reachable from Mac via /api/gc-actor/* HTTP routes.
 *
 * Critical actors (WebReceiver, Dispatcher, Workers, Collector)
 * are explicitly protected_from_gc=1 right after creation so a
 * mis-configured threshold can't reap the runtime itself.
 * ============================================================ */

enum { F_GC_period_ms, F_GC_threshold_ms, F_GC_enabled,
       F_GC_last_sweep_ticks, F_GC_sweep_count, F_GC_swept_total,
       F_GC_last_swept_n, F_GC_last_scanned_n,
       F_GC__N };

static void init_fields_Collector(int self_id) {
  int i;
  for (i = 0; i < F_GC__N; i++)
    objects[self_id].fields[i] = mk_int(0L);
  /* Reasonable defaults — Mac can override via /api/gc-actor/configure. */
  objects[self_id].fields[F_GC_period_ms]    = mk_int(5000L);   /* 5 s */
  objects[self_id].fields[F_GC_threshold_ms] = mk_int(30000L);  /* 30 s idle */
  objects[self_id].fields[F_GC_enabled]      = mk_int(1L);
}

static void Collector_do_sweep(int self_id) {
  long threshold = objects[self_id].fields[F_GC_threshold_ms].i;
  if (threshold < 1000L) threshold = 1000L;     /* sanity floor */
  int scanned = 0;
  int killed  = abcl_gc_sweep(threshold, 0, &scanned);
  objects[self_id].fields[F_GC_last_sweep_ticks] = mk_int((long)clkticks);
  objects[self_id].fields[F_GC_sweep_count] =
    mk_int(objects[self_id].fields[F_GC_sweep_count].i + 1);
  objects[self_id].fields[F_GC_last_swept_n]  = mk_int((long)killed);
  objects[self_id].fields[F_GC_last_scanned_n] = mk_int((long)scanned);
  objects[self_id].fields[F_GC_swept_total] =
    mk_int(objects[self_id].fields[F_GC_swept_total].i + killed);
  wait(print_mu);
  kprintf("[gc-actor] sweep killed=%d/%d threshold=%ldms (sweeps=%ld, total_killed=%ld)\r\n",
          killed, scanned, threshold,
          objects[self_id].fields[F_GC_sweep_count].i,
          objects[self_id].fields[F_GC_swept_total].i);
  signal(print_mu);
}

static void Collector_tick(int self_id, int sender_id,
                           value_t* args, int n_args) {
  (void)sender_id; (void)args; (void)n_args;
  if (!objects[self_id].fields[F_GC_enabled].i) return;
  /* Task-timeout scan runs EVERY tick (1 s), independent of the
   * actor-GC period — Mac wants prompt deadline enforcement even
   * if actor GC is set to a long interval. */
  {
    int expired = abcl_loadbal_timeout_scan();
    if (expired > 0) {
      wait(print_mu);
      kprintf("[gc-actor] timeout: %d pending task(s) auto-cancelled\r\n", expired);
      signal(print_mu);
    }
  }
  long now_ticks = (long)clkticks;
  long last      = objects[self_id].fields[F_GC_last_sweep_ticks].i;
  long period_ticks = objects[self_id].fields[F_GC_period_ms].i / 10;
  if (period_ticks <= 0) period_ticks = 500;     /* 5 s floor */
  if (now_ticks - last < period_ticks) return;   /* not yet */
  Collector_do_sweep(self_id);
}

static void Collector_configure(int self_id, int sender_id,
                                value_t* args, int n_args) {
  (void)sender_id;
  if (n_args >= 1 && args[0].i > 0)
    objects[self_id].fields[F_GC_period_ms] = mk_int(args[0].i);
  if (n_args >= 2 && args[1].i > 0)
    objects[self_id].fields[F_GC_threshold_ms] = mk_int(args[1].i);
  wait(print_mu);
  kprintf("[gc-actor] configured period=%ldms threshold=%ldms\r\n",
          objects[self_id].fields[F_GC_period_ms].i,
          objects[self_id].fields[F_GC_threshold_ms].i);
  signal(print_mu);
}

static void Collector_enable(int self_id, int sender_id,
                             value_t* args, int n_args) {
  (void)sender_id;
  if (n_args < 1) return;
  objects[self_id].fields[F_GC_enabled] = mk_int(args[0].i ? 1L : 0L);
  wait(print_mu);
  kprintf("[gc-actor] %s\r\n", args[0].i ? "ENABLED" : "PAUSED");
  signal(print_mu);
}

static void Collector_sweep_now(int self_id, int sender_id,
                                value_t* args, int n_args) {
  (void)sender_id; (void)args; (void)n_args;
  Collector_do_sweep(self_id);
}

static void dispatch_Collector(int self_id, int sender_id, const char* method,
                               value_t* args, int n_args) {
  if (strcmp(method, "tick") == 0)
  { Collector_tick(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "configure") == 0)
  { Collector_configure(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "enable") == 0)
  { Collector_enable(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "sweep_now") == 0)
  { Collector_sweep_now(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "init") == 0) return;
}

/* GC heartbeat thread — sends "tick" to the Collector at
 * GC_HEARTBEAT_MS intervals.  The actor itself decides whether
 * to actually sweep, based on F_GC_period_ms vs time since last
 * sweep.  Splitting heartbeat-frequency from sweep-frequency
 * means re-configure() takes effect at the next heartbeat
 * (no thread restart needed). */
thread abcl_gc_heartbeat(void) {
  while (!global_shutdown) {
    sleep(GC_HEARTBEAT_MS);
    if (g_gc_actor < 0) continue;
    abcl_enqueue(-1, g_gc_actor, "tick", 0, NULL);
  }
  return OK;
}

/* === accessors for /api/gc-actor/* HTTP routes ========================= */
int abcl_gc_actor_id(void) { return g_gc_actor; }

void abcl_gc_actor_configure(int period_ms, int threshold_ms) {
  if (g_gc_actor < 0) return;
  abcl_enqueue(-1, g_gc_actor, "configure", 2,
               (value_t[]){ mk_int((long)period_ms),
                            mk_int((long)threshold_ms) });
}

void abcl_gc_actor_enable(int on) {
  if (g_gc_actor < 0) return;
  abcl_enqueue(-1, g_gc_actor, "enable", 1,
               (value_t[]){ mk_int((long)on) });
}

void abcl_gc_actor_sweep_now(void) {
  if (g_gc_actor < 0) return;
  abcl_enqueue(-1, g_gc_actor, "sweep_now", 0, NULL);
}

int abcl_gc_actor_stats(char *buf, int cap) {
  if (cap < 300) return 0;
  if (g_gc_actor < 0) return sprintf(buf, "gc-actor not initialized\n");
  int g = g_gc_actor;
  long now_ms  = (long)(clkticks * 10);
  long last_ms = objects[g].fields[F_GC_last_sweep_ticks].i * 10;
  long since   = last_ms > 0 ? now_ms - last_ms : -1L;
  return sprintf(buf,
    "gc-actor obj=%d enabled=%ld\n"
    "period_ms=%ld threshold_ms=%ld\n"
    "sweep_count=%ld swept_total=%ld\n"
    "last_swept=%ld last_scanned=%ld\n"
    "now_ms=%ld last_sweep_ms=%ld since_last_ms=%ld\n",
    g,
    objects[g].fields[F_GC_enabled].i,
    objects[g].fields[F_GC_period_ms].i,
    objects[g].fields[F_GC_threshold_ms].i,
    objects[g].fields[F_GC_sweep_count].i,
    objects[g].fields[F_GC_swept_total].i,
    objects[g].fields[F_GC_last_swept_n].i,
    objects[g].fields[F_GC_last_scanned_n].i,
    now_ms, last_ms, since);
}

/* Bootstrap.  Called from webactor_autostart alongside abcl_loadbal_init.
 * Creates the Collector actor, protects all critical actors from being
 * reaped, and spawns the heartbeat thread. */
static int g_gc_inited = 0;
void abcl_gc_actor_init(void) {
  if (g_gc_inited) return;
  g_gc_actor = alloc_obj(CLASS_Collector, 0, NULL);
  spawn_actor(g_gc_actor);
  /* Protect every infrastructure actor so a mis-set threshold cannot
   * sweep the runtime itself.  We protect the Collector too — recursive
   * suicide would be funny but unhelpful. */
  abcl_object_protect(g_gc_actor, 1);
  {
    int i;
    extern int abcl_loadbal_dispatcher_id(void);
    extern int abcl_loadbal_worker_id(int);
    int d = abcl_loadbal_dispatcher_id();
    if (d >= 0) abcl_object_protect(d, 1);
    for (i = 0; i < LB_N_WORKERS; i++) {
      int w = abcl_loadbal_worker_id(i);
      if (w >= 0) abcl_object_protect(w, 1);
    }
    /* WebReceiver is allocated by abcl_web_init in webactor.  It's
     * always object 0 in the boot path so a hardcoded protect is fine. */
    if (n_objects > 0 && objects[0].class_id == CLASS_WebReceiver)
      abcl_object_protect(0, 1);
  }
  /* Spawn the heartbeat thread */
  tid_typ htid = create((void*)abcl_gc_heartbeat, 4096, INITPRIO,
                        "aipl-gc-hb", 0);
  if (htid != SYSERR) ready(htid, RESCHED_NO);
  g_gc_inited = 1;
  wait(print_mu);
  kprintf("[gc-actor] init done: obj=%d heartbeat tid=%d period=5000ms threshold=30000ms\r\n",
          g_gc_actor, (int)htid);
  signal(print_mu);
}

static void dispatch(int self_id, int sender_id, const char* method, value_t* args, int n_args) {
  switch (objects[self_id].class_id) {
  case CLASS_Fork: dispatch_Fork(self_id, sender_id, method, args, n_args); break;
  case CLASS_Philosopher: dispatch_Philosopher(self_id, sender_id, method, args, n_args); break;
  case CLASS_WebReceiver: dispatch_WebReceiver(self_id, sender_id, method, args, n_args); break;
  case CLASS_Dispatcher: dispatch_Dispatcher(self_id, sender_id, method, args, n_args); break;
  case CLASS_Worker:     dispatch_Worker(self_id, sender_id, method, args, n_args); break;
  case CLASS_Collector:  dispatch_Collector(self_id, sender_id, method, args, n_args); break;
  default: kprintf("unknown class %d\r\n", objects[self_id].class_id);
  }
}

static void init_fields(int class_id, int self_id) {
  switch (class_id) {
  case CLASS_Fork: init_fields_Fork(self_id); break;
  case CLASS_Philosopher: init_fields_Philosopher(self_id); break;
  case CLASS_WebReceiver: break;   /* no fields */
  case CLASS_Dispatcher: init_fields_Dispatcher(self_id); break;
  case CLASS_Worker:     init_fields_Worker(self_id);     break;
  case CLASS_Collector:  init_fields_Collector(self_id);  break;
  default: break;
  }
}

static int alloc_obj(int class_id, int n_args, value_t* args) {
  int id;
  int i;
  wait(objects_mu);
  id = n_objects++;
  signal(objects_mu);
  objects[id].class_id = class_id;
  for (i = 0; i < MAX_FIELDS; i++) objects[id].fields[i] = mk_int(0L);
  init_fields(class_id, id);
  mailbox_init(&objects[id].mbox);
  objects[id].started = 0;
  objects[id].dead = 0;
  objects[id].birth_ticks    = clkticks;
  objects[id].last_enq_ticks = clkticks;
  objects[id].last_deq_ticks = clkticks;
  objects[id].protected_from_gc = 0;
  enqueue(-1, id, "init", n_args, args);
  return id;
}

static void spawn_actor(int id) {
  int prio;
  int actual;
  if (objects[id].started) return;
  objects[id].started = 1;
  prio = abcl_class_prio(objects[id].class_id);
  objects[id].tid = create((void*)abcl_actor_main, 4096, prio,
                            "abcl-actor", 1, id);
  actual = getprio(objects[id].tid);
  wait(print_mu);
  kprintf("[aipl] prio class=%s id=%d tid=%d want=%d got=%d\r\n",
          abcl_class_name(objects[id].class_id), id,
          (int)objects[id].tid, prio, actual);
  signal(print_mu);
  ready(objects[id].tid, RESCHED_NO);
}

int create_obj(int class_id, int n_args, value_t* args) {
  int id = alloc_obj(class_id, n_args, args);
  spawn_actor(id);
  return id;
}

static int _abcl_streq(const char *a, const char *b) {
  while (*a && *b && *a == *b) { a++; b++; }
  return (*a == 0 && *b == 0) ? 1 : 0;
}
int abcl_lookup_class_id(const char *name) {
  if (_abcl_streq(name, "Fork")) return CLASS_Fork;
  if (_abcl_streq(name, "Philosopher")) return CLASS_Philosopher;
  return -1;
}

thread abcl_actor_main(int self_id) {
  mailbox_t* mb = &objects[self_id].mbox;
  for (;;) {
    message_t m;
    int idx;
    uint32_t pos;
    uint32_t slot;
    int spin;
    if (global_shutdown) break;
    wait(mb->items);
    if (global_shutdown) break;
    if (objects[self_id].dead) break;   /* woken by suicide / abcl_rt_reset */
#ifdef _XINU_PLATFORM_ARM_RPI3_
    /* Pi3: strongly-ordered memory, no LDREX/STREX — dequeue under a short
     * interrupt-disabled critical section (matches the producer above). */
    {
      irqmask im = disable();
      (void)spin;
      pos  = mb->deq;
      slot = pos % (uint32_t)MAX_MAILBOX;
      if (mb->slot_seq[slot] != pos + 1) {
        restore(im);
        continue;   /* spurious wake (e.g. shutdown) */
      }
      m = mb->msgs[slot];
      mb->slot_seq[slot] = pos + (uint32_t)MAX_MAILBOX;
      mb->deq = pos + 1;
      restore(im);
    }
#else
    /* P3: lock-free MPSC consumer.  We are the only consumer for
       this mailbox, so deq does not need CAS.  Spin on slot_seq
       until the producer's release-store of pos+1 is visible. */
    pos  = __atomic_load_n(&mb->deq, __ATOMIC_ACQUIRE);
    slot = pos % (uint32_t)MAX_MAILBOX;
    for (spin = 0; spin < 1024; spin++) {
      if (__atomic_load_n(&mb->slot_seq[slot], __ATOMIC_ACQUIRE) == pos + 1) break;
    }
    if (__atomic_load_n(&mb->slot_seq[slot], __ATOMIC_ACQUIRE) != pos + 1) {
      /* Spurious wake (e.g. wake_all_actors during shutdown) — retry. */
      continue;
    }
    m = mb->msgs[slot];
    /* Recycle slot for producer at pos+MAX_MAILBOX. */
    __atomic_store_n(&mb->slot_seq[slot], pos + (uint32_t)MAX_MAILBOX, __ATOMIC_RELEASE);
    __atomic_store_n(&mb->deq, pos + 1, __ATOMIC_RELEASE);
#endif
    /* Stamp dequeue activity (GC sweep "age" = now - max(last_enq, last_deq)). */
    objects[self_id].last_deq_ticks = clkticks;
    abcl_log_first_recv(self_id, m.method);
    wait(counter_mu);
    idx = ++messages_processed;
    signal(counter_mu);
    /* P1: every 25 dispatches print one liveness marker.
       P3: also include clkticks so a smoke can compute throughput. */
    if (idx % 25 == 0) {
      wait(print_mu);
      kprintf("[aipl] alive msg=%d tick=%d\r\n", idx, (int)clkticks);
      signal(print_mu);
    }
    if (_abcl_cap > 0 && idx > _abcl_cap) {
      wait(print_mu);
      kprintf("[abcl] message cap reached (%d)\r\n", _abcl_cap);
      signal(print_mu);
      abcl_shutdown();
      break;
    }
    dispatch(self_id, m.sender, m.method, m.args, m.n_args);
    if (objects[self_id].dead) {
      wait(print_mu);
      kprintf("[aipl] actor %d terminated (suicide)\r\n", self_id);
      signal(print_mu);
      break;            /* self-terminate: leave the loop, thread exits */
    }
  }
  return OK;
}

/* === Web bridge ============================================================
 * Lets an external (HTTP) message be delivered into the AIPL actor system,
 * used by apps/webactor.c so a Mac-side actor can message a Xinu AIPL actor.
 * abcl_web_init() registers a WebReceiver actor (idempotent) and returns its
 * object id; abcl_web_deliver() enqueues a string message to it. */
int abcl_rt_ready = 0;

static void abcl_rt_init_once(void) {
  if (abcl_rt_ready) return;
  counter_mu = semcreate(1);
  print_mu   = semcreate(1);
  objects_mu = semcreate(1);
  abcl_rt_ready = 1;
}

int abcl_web_init(void) {
  static int web_id = -1;
  abcl_rt_init_once();
  if (web_id < 0) web_id = create_obj(CLASS_WebReceiver, 0, NULL);
  return web_id;
}

/* Initialise the AIPL runtime (mutexes) WITHOUT instantiating any actors.
 * Used for the "dynamic" dining demo where Xinu boots with zero dining
 * actors and the Mac spawns the Forks/Philosophers at runtime via SPAWN RPC,
 * so the first SPAWN'd Fork deterministically becomes actor id 0. */
void abcl_rt_init(void) {
  abcl_rt_init_once();
}

void abcl_web_deliver(int receiver, const char *method, const char *str) {
  /* Copy into a small rotating pool so the value_t string survives until the
   * actor thread consumes it.  `method` must be a string literal / static. */
  static char pool[8][240];
  static int  slot = 0;
  int s = slot;
  int i = 0;
  value_t a;
  slot = (slot + 1) & 7;
  while (str[i] && i < 239) { pool[s][i] = str[i]; i++; }
  pool[s][i] = '\0';
  a = mk_str(pool[s]);
  abcl_enqueue(-1, receiver, method, 1, &a);
}

thread aipl_main(void) {
  abcl_rt_init_once();
  kprintf("\r\n[abcl] starting...\r\n");
  kprintf("[aipl] start tick=%d\r\n", (int)clkticks);
  /* phase 1: alloc all globals */
  g_f0 = alloc_obj(CLASS_Fork, 0, NULL);
  g_f1 = alloc_obj(CLASS_Fork, 0, NULL);
  g_f2 = alloc_obj(CLASS_Fork, 0, NULL);
  g_f3 = alloc_obj(CLASS_Fork, 0, NULL);
  g_f4 = alloc_obj(CLASS_Fork, 0, NULL);
  g_p4 = alloc_obj(CLASS_Philosopher, 3, (value_t[]){mk_int((long)(4L)), mk_obj((int)(g_f2)), mk_obj((int)(g_f3))});
  g_p5 = alloc_obj(CLASS_Philosopher, 3, (value_t[]){mk_int((long)(5L)), mk_obj((int)(g_f3)), mk_obj((int)(g_f4))});
  /* Load-balancer (Dispatcher + Workers) is bootstrapped separately
   * by abcl_loadbal_init() — invoked from webactor_autostart so it
   * exists even when aipl_main isn't part of the boot path. */
  /* phase 2: spawn actors */
  {
    int i, total;
    wait(objects_mu); total = n_objects; signal(objects_mu);
    for (i = 0; i < total; i++) spawn_actor(i);
    /* P1: stable thread-count marker. */
    kprintf("[aipl] spawned=%d\r\n", total);
  }
  /* P1: heartbeat thread — proves the Xinu scheduler isn't
     starved by busy-looping actors.  Five ticks over ~5 sec
     should always land if mailboxes use blocking semaphores. */
  {
    extern thread abcl_heartbeat(void);
    tid_typ htid = create((void*)abcl_heartbeat, 4096, INITPRIO,
                          "aipl-hb", 0);
    if (htid != SYSERR) ready(htid, RESCHED_NO);
  }
  /* phase 3: any non-VarDecl top-level */
  /* wait for shutdown */
  while (!global_shutdown) sleep(50);
  /* P3: aggregate mailbox-drop counters across all objects so a
     smoke can verify lock-free MPSC didn't lose messages. */
  {
    int i;
    uint32_t total_drops = 0;
    for (i = 0; i < n_objects; i++)
      total_drops += objects[i].mbox.drops;
    kprintf("[abcl] done; messages=%d drops=%u tick=%d\r\n",
            messages_processed, (unsigned)total_drops, (int)clkticks);
  }
  return OK;
}
