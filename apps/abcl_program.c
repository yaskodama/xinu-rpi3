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

/* P2: AIPL class -> Xinu priority */
#define ABCL_PRIO_Fork 20
#define ABCL_PRIO_Philosopher 20
#define ABCL_PRIO_WebReceiver 20
static int abcl_class_prio(int class_id) {
  switch (class_id) {
  case CLASS_Fork: return ABCL_PRIO_Fork;
  case CLASS_Philosopher: return ABCL_PRIO_Philosopher;
  case CLASS_WebReceiver: return ABCL_PRIO_WebReceiver;
  default: return INITPRIO;
  }
}
const char* abcl_class_name(int class_id) {
  switch (class_id) {
  case CLASS_Fork: return "Fork";
  case CLASS_Philosopher: return "Philosopher";
  case CLASS_WebReceiver: return "WebReceiver";
  default: return "?";
  }
}

static void dispatch_Fork(int, int, const char*, value_t*, int);
static void dispatch_Philosopher(int, int, const char*, value_t*, int);
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

static void dispatch(int self_id, int sender_id, const char* method, value_t* args, int n_args) {
  switch (objects[self_id].class_id) {
  case CLASS_Fork: dispatch_Fork(self_id, sender_id, method, args, n_args); break;
  case CLASS_Philosopher: dispatch_Philosopher(self_id, sender_id, method, args, n_args); break;
  case CLASS_WebReceiver: dispatch_WebReceiver(self_id, sender_id, method, args, n_args); break;
  default: kprintf("unknown class %d\r\n", objects[self_id].class_id);
  }
}

static void init_fields(int class_id, int self_id) {
  switch (class_id) {
  case CLASS_Fork: init_fields_Fork(self_id); break;
  case CLASS_Philosopher: init_fields_Philosopher(self_id); break;
  case CLASS_WebReceiver: break;   /* no fields */
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
