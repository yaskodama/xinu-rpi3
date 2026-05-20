/**
 * @file abcl_xinu_rpc.c
 *
 * H1+H2+H3 of AIPL_XinuRazPi_RemoteRPC.aice — host PC to Xinu actor
 * RPC channel.  Uses PL011 UART1 (0x101F2000) so the existing xsh
 * console on UART0 (0x101F1000) is untouched.
 *
 * Protocol (LF-terminated text lines):
 *
 *   Request                          Reply
 *   --------------------------       ----------------------------------
 *   PING                             OK pong=1
 *   SEND <id> <method> [<a> [<b>]]   OK queued method=<method> id=<id>
 *   QUERY <id> <field_idx>           OK value=<v>   (or ERR oob)
 *   LIST                             OK n_actors=<n>
 *   (anything else)                  ERR <reason>
 *
 * Arg parsing: decimal int only.  Method name <= 31 chars; stored in
 * a rotating 8-slot pool so the abcl_enqueue() pointer stays valid
 * for the brief moment between dispatch and message-consume.
 *
 * Build via apps/Makerules += abcl_xinu_rpc.c, and start the thread
 * from system/main.c (or any boot-time hook).  The thread is
 * `rpc_dispatcher_main`; it spins in a polling read loop with a
 * 10-ms sleep when the UART is empty so it does not starve other
 * threads.
 */

#include <stddef.h>
#include <kernel.h>
#include <thread.h>

/* Value tag layout must mirror c_translator.ml's runtime_prelude_xinu
   and apps/abcl_xinu_gui.c. */
typedef enum { V_NIL, V_INT, V_FLOAT, V_STR, V_OBJ } vtag_t;
typedef struct {
    vtag_t      tag;
    long        i;
    double      f;
    const char *s;
    int         obj_id;
} value_t;

/* Hooks into the AIPL runtime (defined by c_translator's prelude). */
extern void abcl_enqueue(int sender, int receiver, const char *method,
                         int n_args, value_t *args);
extern int  abcl_n_objects(void);
extern int  abcl_object_class_id(int obj_id);
extern int  abcl_object_field_get(int obj_id, int field_idx, value_t *out);

/* ============================================================
 *  PL011 UART1 driver (memory-mapped at 0x101F2000).
 *
 *  We deliberately bypass Xinu's `uarttab[]` device layer so this
 *  module is self-contained — no xinu.conf change, no IRQ wiring.
 *  Polling at ~100 Hz is plenty for an interactive RPC console.
 * ============================================================ */

#define UART1_BASE 0x101F2000UL

struct pl011_regs {
    volatile unsigned int dr;          /* 0x00 */
    volatile unsigned int rsrecr;      /* 0x04 */
    volatile unsigned int pad08;       /* 0x08 */
    volatile unsigned int pad0c;       /* 0x0C */
    volatile unsigned int pad10;       /* 0x10 */
    volatile unsigned int pad14;       /* 0x14 */
    volatile unsigned int fr;          /* 0x18 */
    volatile unsigned int pad1c;       /* 0x1C */
    volatile unsigned int ilpr;        /* 0x20 */
    volatile unsigned int ibrd;        /* 0x24 */
    volatile unsigned int fbrd;        /* 0x28 */
    volatile unsigned int lcrh;        /* 0x2C */
    volatile unsigned int cr;          /* 0x30 */
};

#define PL011_FR_TXFF (1u << 5)
#define PL011_FR_RXFE (1u << 4)
#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE    (1u << 8)
#define PL011_CR_RXE    (1u << 9)

static struct pl011_regs *uart1 = (struct pl011_regs *)UART1_BASE;

static void uart1_init(void)
{
    /* lcrh = 8N1, FIFO enabled */
    uart1->lcrh = (3u << 5) | (1u << 4);
    /* cr = UART enabled, TX enabled, RX enabled */
    uart1->cr   = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;
}

static void uart1_putc(char c)
{
    while (uart1->fr & PL011_FR_TXFF) { /* spin */ }
    uart1->dr = (unsigned int)(unsigned char)c;
}

static void uart1_puts(const char *s)
{
    while (*s) uart1_putc(*s++);
}

/* Non-blocking probe.  Returns 1 if a byte is available, else 0. */
static int uart1_has_rx(void)
{
    return (uart1->fr & PL011_FR_RXFE) ? 0 : 1;
}

/* Blocking read of one byte. */
static int uart1_getc(void)
{
    while (uart1->fr & PL011_FR_RXFE) {
        /* yield 10ms so other threads run. */
        sleep(10);
    }
    return (int)(uart1->dr & 0xFF);
}

/* ============================================================
 *  Line buffering + tokenisation.
 * ============================================================ */

#define MAX_RPC_LINE   256
#define MAX_RPC_TOKS   8

static int rpc_readline(char *out, int cap)
{
    int n = 0;
    int c;
    for (;;) {
        c = uart1_getc();
        if (c < 0) continue;
        if (c == '\r') continue;
        if (c == '\n') { out[n] = '\0'; return n; }
        if (n + 1 < cap) {
            out[n++] = (char)c;
        }
        /* If line overflows, keep eating until LF then truncate. */
    }
}

static int rpc_split(char *line, char *toks[], int max_toks)
{
    int n = 0;
    int i = 0;
    while (line[i] && n < max_toks) {
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (!line[i]) break;
        toks[n++] = &line[i];
        while (line[i] && line[i] != ' ' && line[i] != '\t') i++;
        if (line[i]) { line[i] = '\0'; i++; }
    }
    return n;
}

static int parse_decimal(const char *s, long *out)
{
    long v = 0;
    int neg = 0;
    int i = 0;
    if (!s || !s[0]) return 0;
    if (s[0] == '-') { neg = 1; i = 1; if (!s[1]) return 0; }
    while (s[i]) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        i++;
    }
    *out = neg ? -v : v;
    return 1;
}

static int strn_eq(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static int str_eq_short(const char *a, const char *b)
{
    int i;
    for (i = 0; i < MAX_RPC_LINE; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 0;
}

/* ============================================================
 *  Method-name pool — abcl_enqueue() stores the pointer, so we
 *  keep N method names alive in a static rotating buffer.
 * ============================================================ */

#define METHOD_NAME_CAP   32
#define METHOD_POOL_SLOTS 8

static char  g_method_pool[METHOD_POOL_SLOTS][METHOD_NAME_CAP];
static int   g_method_pool_idx = 0;

static const char *intern_method(const char *src)
{
    char *dst = g_method_pool[g_method_pool_idx];
    int   i;
    g_method_pool_idx = (g_method_pool_idx + 1) % METHOD_POOL_SLOTS;
    for (i = 0; i < METHOD_NAME_CAP - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
    return dst;
}

/* ============================================================
 *  Reply helpers.
 * ============================================================ */

static void send_ok(const char *body)
{
    uart1_puts("OK ");
    uart1_puts(body);
    uart1_puts("\r\n");
}

static void send_err(const char *reason)
{
    uart1_puts("ERR ");
    uart1_puts(reason);
    uart1_puts("\r\n");
}

/* Build "key=N" / "key=string" into a tiny stack buffer.  No printf
 * pulled in to keep the binary small. */
static int append_str(char *buf, int pos, int cap, const char *s)
{
    while (*s && pos + 1 < cap) buf[pos++] = *s++;
    return pos;
}

static int append_int(char *buf, int pos, int cap, long n)
{
    char tmp[24];
    int  i = 0, j;
    int  neg = (n < 0);
    if (neg) n = -n;
    if (n == 0) tmp[i++] = '0';
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    if (neg && pos + 1 < cap) buf[pos++] = '-';
    for (j = i - 1; j >= 0; j--) {
        if (pos + 1 < cap) buf[pos++] = tmp[j];
    }
    return pos;
}

/* ============================================================
 *  Opcode handlers.
 * ============================================================ */

static value_t v_int(long n)
{
    value_t v; v.tag = V_INT; v.i = n; v.f = 0; v.s = 0; v.obj_id = 0;
    return v;
}

static void handle_send(char *toks[], int n_toks)
{
    long id_l;
    int  id;
    const char *method;
    value_t args[2];
    int  n_args = 0;
    char buf[64];
    int  pos = 0;

    if (n_toks < 3) { send_err("send needs id method"); return; }
    if (!parse_decimal(toks[1], &id_l)) { send_err("bad id"); return; }
    id = (int)id_l;
    if (id < 0 || id >= abcl_n_objects()) {
        send_err("oob id");
        return;
    }
    method = intern_method(toks[2]);

    if (n_toks >= 4) {
        long a;
        if (!parse_decimal(toks[3], &a)) { send_err("bad arg1"); return; }
        args[n_args++] = v_int(a);
    }
    if (n_toks >= 5) {
        long b;
        if (!parse_decimal(toks[4], &b)) { send_err("bad arg2"); return; }
        args[n_args++] = v_int(b);
    }

    abcl_enqueue(-1, id, method, n_args, args);

    pos = append_str(buf, pos, sizeof buf, "queued method=");
    pos = append_str(buf, pos, sizeof buf, method);
    pos = append_str(buf, pos, sizeof buf, " id=");
    pos = append_int(buf, pos, sizeof buf, id);
    buf[pos < (int)sizeof(buf) ? pos : (int)sizeof(buf) - 1] = '\0';
    send_ok(buf);
    kprintf("[rpc] SEND id=%d method=%s n_args=%d\r\n",
            id, method, n_args);
}

static void handle_query(char *toks[], int n_toks)
{
    long id_l, fi_l;
    int  id, fi;
    value_t v;
    char buf[48];
    int  pos = 0;

    if (n_toks < 3) { send_err("query needs id field"); return; }
    if (!parse_decimal(toks[1], &id_l)) { send_err("bad id"); return; }
    if (!parse_decimal(toks[2], &fi_l)) { send_err("bad field"); return; }
    id = (int)id_l;
    fi = (int)fi_l;
    if (!abcl_object_field_get(id, fi, &v)) {
        send_err("oob");
        return;
    }
    pos = append_str(buf, pos, sizeof buf, "value=");
    if (v.tag == V_INT)         pos = append_int(buf, pos, sizeof buf, v.i);
    else if (v.tag == V_OBJ)    pos = append_int(buf, pos, sizeof buf, v.obj_id);
    else                        pos = append_str(buf, pos, sizeof buf, "0");
    buf[pos < (int)sizeof(buf) ? pos : (int)sizeof(buf) - 1] = '\0';
    send_ok(buf);
    kprintf("[rpc] QUERY id=%d field=%d\r\n", id, fi);
}

static void handle_list(void)
{
    char buf[32];
    int  pos = 0;
    pos = append_str(buf, pos, sizeof buf, "n_actors=");
    pos = append_int(buf, pos, sizeof buf, abcl_n_objects());
    buf[pos < (int)sizeof(buf) ? pos : (int)sizeof(buf) - 1] = '\0';
    send_ok(buf);
    kprintf("[rpc] LIST n=%d\r\n", abcl_n_objects());
}

static void handle_ping(void)
{
    send_ok("pong=1");
    kprintf("[rpc] PING\r\n");
}

/* ============================================================
 *  Dispatcher thread main loop.
 * ============================================================ */

thread rpc_dispatcher_main(void)
{
    char line[MAX_RPC_LINE];
    char *toks[MAX_RPC_TOKS];
    int   n_toks;

    uart1_init();
    kprintf("[rpc] dispatcher up — UART1 ready on 0x%lx\r\n",
            (unsigned long)UART1_BASE);
    /* Greet anyone who happens to be connected at boot. */
    uart1_puts("OK ready=1\r\n");

    for (;;) {
        (void)rpc_readline(line, sizeof line);
        if (line[0] == '\0') continue;
        n_toks = rpc_split(line, toks, MAX_RPC_TOKS);
        if (n_toks == 0) continue;

        if (str_eq_short(toks[0], "PING")) {
            handle_ping();
        } else if (str_eq_short(toks[0], "SEND")) {
            handle_send(toks, n_toks);
        } else if (str_eq_short(toks[0], "QUERY")) {
            handle_query(toks, n_toks);
        } else if (str_eq_short(toks[0], "LIST")) {
            handle_list();
        } else {
            send_err("unknown opcode");
            kprintf("[rpc] unknown op=%s\r\n", toks[0]);
        }
    }
    return OK;
}

/* External-facing starter — call this from system/main.c (or any boot
 * hook) AFTER abcl actors have been instantiated, so n_objects is
 * already set by the time the first SEND arrives. */
void abcl_rpc_start(void)
{
    tid_typ tid;
    tid = create((void *)rpc_dispatcher_main, 4096, INITPRIO,
                 "aipl-rpc", 0);
    if (tid != SYSERR) {
        ready(tid, RESCHED_NO);
        kprintf("[rpc] dispatcher thread created tid=%d\r\n", (int)tid);
    } else {
        kprintf("[rpc] FAILED to create dispatcher thread\r\n");
    }
}
