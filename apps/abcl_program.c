#include <stddef.h>
#include <kernel.h>
#include <thread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>

#define MAX_MAILBOX 16
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

static void v_print(value_t v) {
  wait(print_mu);
  switch (v.tag) {
  case V_STR: kprintf("%s\r\n", v.s ? v.s : ""); break;
  case V_INT: kprintf("%d\r\n", (int)v.i);       break;
  case V_OBJ: kprintf("<obj %d>\r\n", v.obj_id); break;
  default:    kprintf("<nil>\r\n");              break;
  }
  signal(print_mu);
}

typedef struct {
  int         sender;
  int         receiver;
  const char *method;
  int         n_args;
  value_t     args[MAX_ARGS];
} message_t;

typedef struct {
  message_t msgs[MAX_MAILBOX];
  int       head, tail;
  semaphore mu;
  semaphore items;
} mailbox_t;

typedef struct {
  int       class_id;
  value_t   fields[MAX_FIELDS];
  mailbox_t mbox;
  tid_typ   tid;
  int       started;
} object_t;

static object_t objects[MAX_OBJECTS];
static int      n_objects = 0;
static semaphore objects_mu;

static void mailbox_init(mailbox_t *mb) {
  mb->head = 0;
  mb->tail = 0;
  mb->mu    = semcreate(1);
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

/* Xinu の queue.h にある enqueue() と名前が衝突するのでリネーム。
   以降 abcl 側のコードでは enqueue マクロで本関数を呼ぶ。 */
void abcl_enqueue(int sender, int receiver, const char *method,
                  int n_args, value_t *args) {
  if (receiver < 0 || receiver >= n_objects) return;
  mailbox_t *mb = &objects[receiver].mbox;
  wait(mb->mu);
  if (mb->tail - mb->head < MAX_MAILBOX) {
    int idx = mb->tail % MAX_MAILBOX;
    int i;
    mb->msgs[idx].sender   = sender;
    mb->msgs[idx].receiver = receiver;
    mb->msgs[idx].method   = method;
    mb->msgs[idx].n_args   = n_args;
    for (i = 0; i < n_args && i < MAX_ARGS; i++)
      mb->msgs[idx].args[i] = args[i];
    mb->tail++;
    signal(mb->mu);
    signal(mb->items);
  } else {
    signal(mb->mu);
  }
}

/* 以降の生成コードでは abcl_enqueue を enqueue として書く */
#define enqueue abcl_enqueue

/* runtime cap override */
static int _abcl_cap = 0;

/* extern built-ins */
extern value_t xinu_gui_buf_put(int n_args, value_t* args);
extern value_t xinu_gui_buf_take(int n_args, value_t* args);
extern value_t xinu_gui_set_actor(int n_args, value_t* args);
extern value_t xinu_gui_slider_value(int n_args, value_t* args);
extern value_t xinu_gui_buf_setup(int n_args, value_t* args);
extern value_t xinu_gui_register_ticker(int n_args, value_t* args);
extern value_t xinu_gui_add_slider(int n_args, value_t* args);
extern value_t xinu_gui_add_button(int n_args, value_t* args);

#define CLASS_Buffer 0
#define CLASS_Producer 1
#define CLASS_Consumer 2
#define CLASS_Controller 3

static void dispatch_Buffer(int, int, const char*, value_t*, int);
static void dispatch_Producer(int, int, const char*, value_t*, int);
static void dispatch_Consumer(int, int, const char*, value_t*, int);
static void dispatch_Controller(int, int, const char*, value_t*, int);
static void dispatch(int, int, const char*, value_t*, int);
static int  alloc_obj(int class_id, int n_args, value_t* args);
static void spawn_actor(int id);
static int  create_obj(int class_id, int n_args, value_t* args);
thread      abcl_actor_main(int self_id);

static int g_ctrl = -1;

enum { F_Buffer_capacity, F_Buffer_count, F_Buffer__N };

static void init_fields_Buffer(int self_id) {
  (void)self_id;
  objects[self_id].fields[F_Buffer_capacity] = mk_int(20L);
  objects[self_id].fields[F_Buffer_count] = mk_int(0L);
}

static void Buffer_init(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  value_t p_cap = (n_args > 0) ? args[0] : mk_int(0L);
  objects[self_id].fields[F_Buffer_capacity] = p_cap;
  objects[self_id].fields[F_Buffer_count] = mk_int(0L);
}

static void Buffer_put(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  value_t p_producerId = (n_args > 0) ? args[0] : mk_int(0L);
  if (truthy(v_binop("<", objects[self_id].fields[F_Buffer_count], objects[self_id].fields[F_Buffer_capacity]))) {
    objects[self_id].fields[F_Buffer_count] = v_binop("+", objects[self_id].fields[F_Buffer_count], mk_int(1L));
    xinu_gui_buf_put(1, (value_t[]){p_producerId});
    enqueue(self_id, sender_id, "put_ok", 0, NULL);
  } else {
    enqueue(self_id, sender_id, "put_full", 0, NULL);
  }
}

static void Buffer_take(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  value_t p_consumerId = (n_args > 0) ? args[0] : mk_int(0L);
  if (truthy(v_binop(">", objects[self_id].fields[F_Buffer_count], mk_int(0L)))) {
    objects[self_id].fields[F_Buffer_count] = v_binop("-", objects[self_id].fields[F_Buffer_count], mk_int(1L));
    xinu_gui_buf_take(1, (value_t[]){p_consumerId});
    enqueue(self_id, sender_id, "take_ok", 0, NULL);
  } else {
    enqueue(self_id, sender_id, "take_empty", 0, NULL);
  }
}

static void dispatch_Buffer(int self_id, int sender_id, const char* method, value_t* args, int n_args) {
  if (strcmp(method, "init") == 0) { Buffer_init(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "put") == 0) { Buffer_put(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "take") == 0) { Buffer_take(self_id, sender_id, args, n_args); return; }
  fprintf(stderr, "unknown method %s on Buffer\n", method);
}

enum { F_Producer_idx, F_Producer_state, F_Producer_counter, F_Producer_buffer, F_Producer_running, F_Producer__N };

static void init_fields_Producer(int self_id) {
  (void)self_id;
  objects[self_id].fields[F_Producer_idx] = mk_int(0L);
  objects[self_id].fields[F_Producer_state] = mk_int(0L);
  objects[self_id].fields[F_Producer_counter] = mk_int(0L);
  objects[self_id].fields[F_Producer_buffer] = mk_int(0L);
  objects[self_id].fields[F_Producer_running] = mk_int(0L);
}

static void Producer_init(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  value_t p_i = (n_args > 0) ? args[0] : mk_int(0L);
  value_t p_buf = (n_args > 1) ? args[1] : mk_int(0L);
  objects[self_id].fields[F_Producer_idx] = p_i;
  objects[self_id].fields[F_Producer_buffer] = p_buf;
  objects[self_id].fields[F_Producer_state] = mk_int(0L);
  objects[self_id].fields[F_Producer_counter] = v_binop("+", mk_int(30L), v_binop("*", p_i, mk_int(12L)));
  objects[self_id].fields[F_Producer_running] = mk_int(0L);
  xinu_gui_set_actor(3, (value_t[]){objects[self_id].fields[F_Producer_idx], mk_int(0L), mk_int(0L)});
}

static void Producer_start(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  objects[self_id].fields[F_Producer_running] = mk_int(1L);
}

static void Producer_stop(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  objects[self_id].fields[F_Producer_running] = mk_int(0L);
}

static void Producer_tick(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  if (truthy(v_binop("==", objects[self_id].fields[F_Producer_running], mk_int(1L)))) {
    if (truthy(v_binop("==", objects[self_id].fields[F_Producer_state], mk_int(0L)))) {
      objects[self_id].fields[F_Producer_counter] = v_binop("-", objects[self_id].fields[F_Producer_counter], mk_int(1L));
      if (truthy(v_binop("<", objects[self_id].fields[F_Producer_counter], mk_int(1L)))) {
        objects[self_id].fields[F_Producer_state] = mk_int(1L);
        objects[self_id].fields[F_Producer_counter] = mk_int(18L);
        xinu_gui_set_actor(3, (value_t[]){objects[self_id].fields[F_Producer_idx], mk_int(0L), mk_int(1L)});
      } else {
      }
    } else {
      if (truthy(v_binop("==", objects[self_id].fields[F_Producer_state], mk_int(1L)))) {
        objects[self_id].fields[F_Producer_counter] = v_binop("-", objects[self_id].fields[F_Producer_counter], mk_int(1L));
        if (truthy(v_binop("<", objects[self_id].fields[F_Producer_counter], mk_int(1L)))) {
          objects[self_id].fields[F_Producer_state] = mk_int(2L);
          xinu_gui_set_actor(3, (value_t[]){objects[self_id].fields[F_Producer_idx], mk_int(0L), mk_int(2L)});
          enqueue(self_id, objects[self_id].fields[F_Producer_buffer].obj_id, "put", 1, (value_t[]){objects[self_id].fields[F_Producer_idx]});
        } else {
        }
      } else {
      }
    }
  } else {
  }
}

static void Producer_put_ok(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  if (truthy(v_binop("==", objects[self_id].fields[F_Producer_state], mk_int(2L)))) {
    objects[self_id].fields[F_Producer_state] = mk_int(0L);
    objects[self_id].fields[F_Producer_counter] = v_binop("-", mk_int(110L), v_binop("*", xinu_gui_slider_value(1, (value_t[]){mk_int(0L)}), mk_int(10L)));
    xinu_gui_set_actor(3, (value_t[]){objects[self_id].fields[F_Producer_idx], mk_int(0L), mk_int(0L)});
  } else {
  }
}

static void Producer_put_full(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  if (truthy(v_binop("==", objects[self_id].fields[F_Producer_state], mk_int(2L)))) {
    objects[self_id].fields[F_Producer_state] = mk_int(1L);
    objects[self_id].fields[F_Producer_counter] = mk_int(8L);
  } else {
  }
}

static void dispatch_Producer(int self_id, int sender_id, const char* method, value_t* args, int n_args) {
  if (strcmp(method, "init") == 0) { Producer_init(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "start") == 0) { Producer_start(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "stop") == 0) { Producer_stop(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "tick") == 0) { Producer_tick(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "put_ok") == 0) { Producer_put_ok(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "put_full") == 0) { Producer_put_full(self_id, sender_id, args, n_args); return; }
  fprintf(stderr, "unknown method %s on Producer\n", method);
}

enum { F_Consumer_idx, F_Consumer_state, F_Consumer_counter, F_Consumer_buffer, F_Consumer_running, F_Consumer__N };

static void init_fields_Consumer(int self_id) {
  (void)self_id;
  objects[self_id].fields[F_Consumer_idx] = mk_int(0L);
  objects[self_id].fields[F_Consumer_state] = mk_int(0L);
  objects[self_id].fields[F_Consumer_counter] = mk_int(0L);
  objects[self_id].fields[F_Consumer_buffer] = mk_int(0L);
  objects[self_id].fields[F_Consumer_running] = mk_int(0L);
}

static void Consumer_init(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  value_t p_i = (n_args > 0) ? args[0] : mk_int(0L);
  value_t p_buf = (n_args > 1) ? args[1] : mk_int(0L);
  objects[self_id].fields[F_Consumer_idx] = p_i;
  objects[self_id].fields[F_Consumer_buffer] = p_buf;
  objects[self_id].fields[F_Consumer_state] = mk_int(0L);
  objects[self_id].fields[F_Consumer_counter] = v_binop("+", mk_int(50L), v_binop("*", p_i, mk_int(10L)));
  objects[self_id].fields[F_Consumer_running] = mk_int(0L);
  xinu_gui_set_actor(3, (value_t[]){objects[self_id].fields[F_Consumer_idx], mk_int(1L), mk_int(0L)});
}

static void Consumer_start(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  objects[self_id].fields[F_Consumer_running] = mk_int(1L);
}

static void Consumer_stop(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  objects[self_id].fields[F_Consumer_running] = mk_int(0L);
}

static void Consumer_tick(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  if (truthy(v_binop("==", objects[self_id].fields[F_Consumer_running], mk_int(1L)))) {
    if (truthy(v_binop("==", objects[self_id].fields[F_Consumer_state], mk_int(0L)))) {
      objects[self_id].fields[F_Consumer_counter] = v_binop("-", objects[self_id].fields[F_Consumer_counter], mk_int(1L));
      if (truthy(v_binop("<", objects[self_id].fields[F_Consumer_counter], mk_int(1L)))) {
        objects[self_id].fields[F_Consumer_state] = mk_int(2L);
        xinu_gui_set_actor(3, (value_t[]){objects[self_id].fields[F_Consumer_idx], mk_int(1L), mk_int(2L)});
        enqueue(self_id, objects[self_id].fields[F_Consumer_buffer].obj_id, "take", 1, (value_t[]){objects[self_id].fields[F_Consumer_idx]});
      } else {
      }
    } else {
      if (truthy(v_binop("==", objects[self_id].fields[F_Consumer_state], mk_int(1L)))) {
        objects[self_id].fields[F_Consumer_counter] = v_binop("-", objects[self_id].fields[F_Consumer_counter], mk_int(1L));
        if (truthy(v_binop("<", objects[self_id].fields[F_Consumer_counter], mk_int(1L)))) {
          objects[self_id].fields[F_Consumer_state] = mk_int(0L);
          objects[self_id].fields[F_Consumer_counter] = v_binop("-", mk_int(110L), v_binop("*", xinu_gui_slider_value(1, (value_t[]){mk_int(1L)}), mk_int(10L)));
          xinu_gui_set_actor(3, (value_t[]){objects[self_id].fields[F_Consumer_idx], mk_int(1L), mk_int(0L)});
        } else {
        }
      } else {
      }
    }
  } else {
  }
}

static void Consumer_take_ok(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  if (truthy(v_binop("==", objects[self_id].fields[F_Consumer_state], mk_int(2L)))) {
    objects[self_id].fields[F_Consumer_state] = mk_int(1L);
    objects[self_id].fields[F_Consumer_counter] = mk_int(18L);
    xinu_gui_set_actor(3, (value_t[]){objects[self_id].fields[F_Consumer_idx], mk_int(1L), mk_int(1L)});
  } else {
  }
}

static void Consumer_take_empty(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  if (truthy(v_binop("==", objects[self_id].fields[F_Consumer_state], mk_int(2L)))) {
    objects[self_id].fields[F_Consumer_state] = mk_int(0L);
    objects[self_id].fields[F_Consumer_counter] = mk_int(8L);
  } else {
  }
}

static void dispatch_Consumer(int self_id, int sender_id, const char* method, value_t* args, int n_args) {
  if (strcmp(method, "init") == 0) { Consumer_init(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "start") == 0) { Consumer_start(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "stop") == 0) { Consumer_stop(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "tick") == 0) { Consumer_tick(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "take_ok") == 0) { Consumer_take_ok(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "take_empty") == 0) { Consumer_take_empty(self_id, sender_id, args, n_args); return; }
  fprintf(stderr, "unknown method %s on Consumer\n", method);
}

enum { F_Controller_buf, F_Controller_p0, F_Controller_p1, F_Controller_p2, F_Controller_c0, F_Controller_c1, F_Controller_c2, F_Controller__N };

static void init_fields_Controller(int self_id) {
  (void)self_id;
  objects[self_id].fields[F_Controller_buf] = mk_int(0L);
  objects[self_id].fields[F_Controller_p0] = mk_int(0L);
  objects[self_id].fields[F_Controller_p1] = mk_int(0L);
  objects[self_id].fields[F_Controller_p2] = mk_int(0L);
  objects[self_id].fields[F_Controller_c0] = mk_int(0L);
  objects[self_id].fields[F_Controller_c1] = mk_int(0L);
  objects[self_id].fields[F_Controller_c2] = mk_int(0L);
}

static void Controller_init(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  objects[self_id].fields[F_Controller_buf] = mk_obj(create_obj(CLASS_Buffer, 1, (value_t[]){mk_int(20L)}));
  objects[self_id].fields[F_Controller_p0] = mk_obj(create_obj(CLASS_Producer, 2, (value_t[]){mk_int(0L), objects[self_id].fields[F_Controller_buf]}));
  objects[self_id].fields[F_Controller_p1] = mk_obj(create_obj(CLASS_Producer, 2, (value_t[]){mk_int(1L), objects[self_id].fields[F_Controller_buf]}));
  objects[self_id].fields[F_Controller_p2] = mk_obj(create_obj(CLASS_Producer, 2, (value_t[]){mk_int(2L), objects[self_id].fields[F_Controller_buf]}));
  objects[self_id].fields[F_Controller_c0] = mk_obj(create_obj(CLASS_Consumer, 2, (value_t[]){mk_int(0L), objects[self_id].fields[F_Controller_buf]}));
  objects[self_id].fields[F_Controller_c1] = mk_obj(create_obj(CLASS_Consumer, 2, (value_t[]){mk_int(1L), objects[self_id].fields[F_Controller_buf]}));
  objects[self_id].fields[F_Controller_c2] = mk_obj(create_obj(CLASS_Consumer, 2, (value_t[]){mk_int(2L), objects[self_id].fields[F_Controller_buf]}));
  xinu_gui_buf_setup(3, (value_t[]){mk_int(20L), mk_int(3L), mk_int(3L)});
  xinu_gui_register_ticker(1, (value_t[]){objects[self_id].fields[F_Controller_p0]});
  xinu_gui_register_ticker(1, (value_t[]){objects[self_id].fields[F_Controller_p1]});
  xinu_gui_register_ticker(1, (value_t[]){objects[self_id].fields[F_Controller_p2]});
  xinu_gui_register_ticker(1, (value_t[]){objects[self_id].fields[F_Controller_c0]});
  xinu_gui_register_ticker(1, (value_t[]){objects[self_id].fields[F_Controller_c1]});
  xinu_gui_register_ticker(1, (value_t[]){objects[self_id].fields[F_Controller_c2]});
  xinu_gui_add_slider(12, (value_t[]){mk_int(0L), mk_int(90L), mk_int(370L), mk_int(460L), mk_int(18L), mk_int(1L), mk_int(10L), mk_int(5L), mk_int(220L), mk_int(90L), mk_int(90L), mk_str("IN")});
  xinu_gui_add_slider(12, (value_t[]){mk_int(1L), mk_int(90L), mk_int(410L), mk_int(460L), mk_int(18L), mk_int(1L), mk_int(10L), mk_int(5L), mk_int(90L), mk_int(200L), mk_int(130L), mk_str("OUT")});
  xinu_gui_add_button(7, (value_t[]){mk_str("Start"), mk_int(40L), mk_int(26L), mk_int(90L), mk_int(30L), mk_obj(self_id), mk_str("start")});
  xinu_gui_add_button(7, (value_t[]){mk_str("Stop"), mk_int(510L), mk_int(26L), mk_int(90L), mk_int(30L), mk_obj(self_id), mk_str("stop")});
  enqueue(self_id, self_id, "start", 0, NULL);
}

static void Controller_start(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  enqueue(self_id, objects[self_id].fields[F_Controller_p0].obj_id, "start", 0, NULL);
  enqueue(self_id, objects[self_id].fields[F_Controller_p1].obj_id, "start", 0, NULL);
  enqueue(self_id, objects[self_id].fields[F_Controller_p2].obj_id, "start", 0, NULL);
  enqueue(self_id, objects[self_id].fields[F_Controller_c0].obj_id, "start", 0, NULL);
  enqueue(self_id, objects[self_id].fields[F_Controller_c1].obj_id, "start", 0, NULL);
  enqueue(self_id, objects[self_id].fields[F_Controller_c2].obj_id, "start", 0, NULL);
}

static void Controller_stop(int self_id, int sender_id, value_t* args, int n_args) {
  (void)args; (void)n_args; (void)sender_id;
  enqueue(self_id, objects[self_id].fields[F_Controller_p0].obj_id, "stop", 0, NULL);
  enqueue(self_id, objects[self_id].fields[F_Controller_p1].obj_id, "stop", 0, NULL);
  enqueue(self_id, objects[self_id].fields[F_Controller_p2].obj_id, "stop", 0, NULL);
  enqueue(self_id, objects[self_id].fields[F_Controller_c0].obj_id, "stop", 0, NULL);
  enqueue(self_id, objects[self_id].fields[F_Controller_c1].obj_id, "stop", 0, NULL);
  enqueue(self_id, objects[self_id].fields[F_Controller_c2].obj_id, "stop", 0, NULL);
}

static void dispatch_Controller(int self_id, int sender_id, const char* method, value_t* args, int n_args) {
  if (strcmp(method, "init") == 0) { Controller_init(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "start") == 0) { Controller_start(self_id, sender_id, args, n_args); return; }
  if (strcmp(method, "stop") == 0) { Controller_stop(self_id, sender_id, args, n_args); return; }
  fprintf(stderr, "unknown method %s on Controller\n", method);
}

static void dispatch(int self_id, int sender_id, const char* method, value_t* args, int n_args) {
  switch (objects[self_id].class_id) {
  case CLASS_Buffer: dispatch_Buffer(self_id, sender_id, method, args, n_args); break;
  case CLASS_Producer: dispatch_Producer(self_id, sender_id, method, args, n_args); break;
  case CLASS_Consumer: dispatch_Consumer(self_id, sender_id, method, args, n_args); break;
  case CLASS_Controller: dispatch_Controller(self_id, sender_id, method, args, n_args); break;
  default: kprintf("unknown class %d\r\n", objects[self_id].class_id);
  }
}

static void init_fields(int class_id, int self_id) {
  switch (class_id) {
  case CLASS_Buffer: init_fields_Buffer(self_id); break;
  case CLASS_Producer: init_fields_Producer(self_id); break;
  case CLASS_Consumer: init_fields_Consumer(self_id); break;
  case CLASS_Controller: init_fields_Controller(self_id); break;
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
  enqueue(-1, id, "init", n_args, args);
  return id;
}

static void spawn_actor(int id) {
  if (objects[id].started) return;
  objects[id].started = 1;
  objects[id].tid = create((void*)abcl_actor_main, 4096, INITPRIO,
                            "abcl-actor", 1, id);
  ready(objects[id].tid, RESCHED_NO);
}

static int create_obj(int class_id, int n_args, value_t* args) {
  int id = alloc_obj(class_id, n_args, args);
  spawn_actor(id);
  return id;
}

thread abcl_actor_main(int self_id) {
  mailbox_t* mb = &objects[self_id].mbox;
  for (;;) {
    message_t m;
    int idx;
    if (global_shutdown) break;
    wait(mb->items);
    if (global_shutdown) break;
    wait(mb->mu);
    if (mb->head == mb->tail) { signal(mb->mu); continue; }
    m = mb->msgs[mb->head % MAX_MAILBOX];
    mb->head++;
    signal(mb->mu);
    wait(counter_mu);
    idx = ++messages_processed;
    signal(counter_mu);
    if (_abcl_cap > 0 && idx > _abcl_cap) {
      wait(print_mu);
      kprintf("[abcl] message cap reached (%d)\r\n", _abcl_cap);
      signal(print_mu);
      abcl_shutdown();
      break;
    }
    dispatch(self_id, m.sender, m.method, m.args, m.n_args);
  }
  return OK;
}

thread abcl_main(void) {
  counter_mu = semcreate(1);
  print_mu   = semcreate(1);
  objects_mu = semcreate(1);
  kprintf("\r\n[abcl] starting...\r\n");
  /* phase 1: alloc all globals */
  g_ctrl = alloc_obj(CLASS_Controller, 0, NULL);
  /* phase 2: spawn actors */
  {
    int i, total;
    wait(objects_mu); total = n_objects; signal(objects_mu);
    for (i = 0; i < total; i++) spawn_actor(i);
  }
  /* phase 3: any non-VarDecl top-level */
  /* wait for shutdown */
  while (!global_shutdown) sleep(50);
  kprintf("[abcl] done; messages=%d\r\n", messages_processed);
  return OK;
}
