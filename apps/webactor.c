/**
 * @file webactor.c
 *
 * Minimal HTTP server that bridges an incoming web request to an AIPL actor.
 *
 * A Mac-side actor sends an HTTP POST (or GET) carrying a message; this server
 * accepts the TCP connection on WEBACTOR_PORT, extracts the message, and hands
 * it to a "WebReceiver" AIPL actor (registered in apps/abcl_program.c) via
 * abcl_web_deliver().  The AIPL actor then processes the message in its own
 * thread/mailbox, just like any other AIPL actor message.
 *
 * Flow:   Mac actor --HTTP--> webactor_server --abcl_enqueue--> AIPL actor
 *
 * Start it from the shell (after `netup`):   webactor
 */

#include <kernel.h>
#include <stddef.h>
#include <stdint.h>    /* uint32_t for /api/mmu route */
#include <thread.h>
#include <device.h>
#include <ether.h>
#include <network.h>
#include <ipv4.h>
#include <tcp.h>
#include <string.h>
#include <stdio.h>
#include <clock.h>
#include <watchdog.h>  /* watchdogset() for /reboot route */
#include <thread.h>    /* thrtab + NTHREAD for /api/threads route */
#include <memory.h>    /* memlist for /api/memstat */
#include "sd_block.h"  /* sd_init / sd_read_block for /sd-test route */

/* Latest /upload payload — file-scope so /api/upload-info can read it
 * back.  Single slot (last upload wins).  64 KB is generous enough for
 * stage-1 testing; the kernel.img is ~290 KB so a future commit needs
 * a streamed upload path that doesn't pre-allocate. */
static char           upload_slot_name[64];
/* 512 KB — bumped from 64 KB so a full xinu.boot (~305 KB) can be
 * uploaded for the /kexec network-update path.  Lives in .bss so it
 * doesn't inflate the on-disk binary, just runtime footprint.
 *
 * MUST be word-aligned: the kexec stub uses LDR r3, [r0], #4 to copy
 * the buffer to 0x8000 with the MMU disabled (treats memory as
 * Strongly Ordered, so any misalignment data-aborts regardless of
 * SCTLR.A).  Without this attribute the linker may park the array at
 * an odd offset and only some kernel sizes survive — silent boot
 * failure with `!EXC A` at PC=0x7fec. */
static unsigned char  upload_slot_data[524288] __attribute__((aligned(16)));
static int            upload_slot_size = 0;

void _upload_set(const char *name, const unsigned char *data, int size)
{
    int i;
    for (i = 0; i < 63 && name[i]; i++) upload_slot_name[i] = name[i];
    upload_slot_name[i] = '\0';
    if (size > (int)sizeof(upload_slot_data)) size = sizeof(upload_slot_data);
    for (i = 0; i < size; i++) upload_slot_data[i] = data[i];
    upload_slot_size = size;
}
const char           *_upload_name(void) { return upload_slot_name; }
const unsigned char  *_upload_data(void) { return upload_slot_data; }
int                   _upload_size(void) { return upload_slot_size; }

/* Convert a state code to a short name (mirrors Pi 4 wm_actors window
 * + the in-kernel ps shell command). */
static const char *state_name(uchar s)
{
    switch (s) {
        case THRFREE:    return "FREE";
        case THRCURR:    return "CURR";
        case THRREADY:   return "READY";
        case THRRECV:    return "RECV";
        case THRSLEEP:   return "SLEEP";
        case THRSUSP:    return "SUSP";
        case THRWAIT:    return "WAIT";
        case THRTMOUT:   return "TMOUT";
        case THRMIGRATE: return "MIGR";
        default:         return "?";
    }
}

/* AIPL bridge (apps/abcl_program.c) */
extern int  abcl_web_init(void);
extern void abcl_web_deliver(int receiver, const char *method, const char *str);

#define WEBACTOR_PORT 8080
#define WEB_BUFSZ     524288   /* 512 KB — was 64 KB; bumped to fit a full
                                 * xinu.boot upload for /kexec network update.
                                 * Earlier note (kept for context):
                                 * was 1024 — bumped for /upload bodies (up
                                * to ~64 KB).  Stack-resident in the server
                                * thread; the thread is created with 64 KB
                                * stack so this allocates the stack itself. */

/* Static network config used by the boot auto-start (webactor_autostart). */
#define WEBACTOR_IP   "192.168.3.50"
#define WEBACTOR_MASK "255.255.255.0"
#define WEBACTOR_GW   "192.168.3.1"

static int     web_receiver_id = -1;
static tid_typ web_server_tid   = BADTID;  /* running server thread, or BADTID */
static short   web_cur_tcpdev   = -1;      /* TCP dev the server is parked on  */

/* Pull the message out of the HTTP request: the body after the blank line if
 * present (POST), otherwise the path after "GET /".  Result is NUL-terminated.
 */
static void extract_message(const char *req, int reqlen, char *out, int outsz)
{
    const char *body = NULL;
    int i, j;

    out[0] = '\0';

    /* locate end-of-headers ("\r\n\r\n") */
    for (i = 0; i + 3 < reqlen; i++)
    {
        if (req[i] == '\r' && req[i + 1] == '\n' &&
            req[i + 2] == '\r' && req[i + 3] == '\n')
        {
            body = req + i + 4;
            break;
        }
    }

    if (body != NULL && body[0] != '\0')
    {
        for (j = 0; body[j] != '\0' && j < outsz - 1; j++)
            out[j] = body[j];
        out[j] = '\0';
        return;
    }

    /* No body: fall back to the request target of "GET /<msg> ..." */
    if (0 == strncmp(req, "GET /", 5))
    {
        const char *p = req + 5;
        for (j = 0; *p != '\0' && *p != ' ' && *p != '\r' && j < outsz - 1; j++)
            out[j] = *p++;
        out[j] = '\0';
    }
}

/* HTTP server thread: accept one connection at a time, deliver the message to
 * the AIPL actor, answer with a tiny HTTP 200, then accept the next. */
thread webactor_server(void)
{
    struct netif   *interface;
    struct netaddr *host;
    ushort tcpdev;
    /* Static (not on the 8 KB thread stack) so a 64 KB upload body fits.
     * Single-instance webactor_server thread means no concurrency on this
     * buffer — every request is read+dispatched serially. */
    static char reqbuf[WEB_BUFSZ];
    char   msg[256];
    int    n;
    static const char resp[] =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 18\r\n"
        "\r\n"
        "delivered to actor";

    interface = netLookup((ethertab[0].dev)->num);
    if (NULL == interface)
    {
        kprintf("[webactor] ETH0 has no IP — run `netup ETH0 ...` first\r\n");
        return SYSERR;
    }
    host = &(interface->ip);
    kprintf("[webactor] HTTP server up on port %d -> AIPL actor %d\r\n",
            WEBACTOR_PORT, web_receiver_id);

    while (TRUE)
    {
        tcpdev = tcpAlloc();
        if (SYSERR == (short)tcpdev)
        {
            sleep(100);
            continue;
        }
        web_cur_tcpdev = (short)tcpdev;   /* so webactor_stop() can free it */
        /* TCP_PASSIVE blocks until a client connects */
        if (open(tcpdev, host, NULL, WEBACTOR_PORT, NULL, TCP_PASSIVE) < 0)
        {
            close(tcpdev);
            web_cur_tcpdev = -1;
            sleep(100);
            continue;
        }

        /* Read the whole HTTP request.  Xinu's tcpRead() blocks until it has
         * the full requested length (or the peer closes) — see the
         * `while (count < len)` loop in device/tcp/tcpRead.c — so a single
         * read(.., WEB_BUFSZ-1) deadlocks: the client sends a short request
         * and then waits for the response without closing, so the rest of the
         * 1023 bytes never arrive.
         *
         * Strategy: two-phase reader.
         *   - header phase: ONE BYTE AT A TIME.  A small GET like
         *                   "GET /api/mmu HTTP/1.1\r\nHost: ...\r\n\r\n"
         *                   is ~85 B.  If we ask for 128 in one go,
         *                   Xinu's tcpRead blocks waiting for the
         *                   missing 43 bytes that the client never
         *                   sends (it's done) — deadlocks webactor.
         *                   Per-byte read lets us detect "\r\n\r\n"
         *                   the instant it arrives and exit.  Header
         *                   is bounded (a few hundred bytes), so the
         *                   syscall overhead is small.
         *   - body phase  : 4 KB chunks once Content-Length is known.
         *                   Cap stays well under TCP_IBLEN (16 KB) so
         *                   buffer can refill while we're parsing.
         *                   The last chunk is sized to exactly the
         *                   remaining body so we never overshoot.
         *
         * Previous version was 1-byte reads through EVERYTHING — O(n^2)
         * over the whole reqbuf because of per-byte strstr.  A 305 KB
         * /upload took 92 s; the chunked-body version below drops it
         * to a few seconds end-to-end. */
        n = 0;
        {
            int header_end = -1, content_len = 0, have_cl = 0;
            while (n < WEB_BUFSZ - 1)
            {
                int want;
                if (header_end < 0)
                {
                    want = 1;               /* 1 byte at a time — see big
                                             * comment above re GET deadlock */
                }
                else
                {
                    int remaining = (header_end + content_len) - n;
                    if (remaining <= 0) break;          /* body complete */
                    /* Cap at 4 KB — Xinu's TCP input buffer is
                     * TCP_IBLEN=16384, so asking for the whole IBLEN
                     * forces a producer/consumer ping-pong with the
                     * client's TCP window: the first 16 KB read of a
                     * /upload wedged webactor (kernel still alive,
                     * pings, just no HTTP).  4 KB is well under IBLEN
                     * so the buffer can refill while we're parsing. */
                    want = remaining < 4096 ? remaining : 4096;
                }
                if (n + want >= WEB_BUFSZ) want = WEB_BUFSZ - 1 - n;
                if (want <= 0) break;
                int r = read(tcpdev, reqbuf + n, want);
                if (r <= 0)
                {
                    break;                  /* peer closed / error */
                }
                n += r;
                reqbuf[n] = '\0';
                if (header_end < 0)
                {
                    char *p = strstr(reqbuf, "\r\n\r\n");
                    if (NULL != p)
                    {
                        char *cl;
                        header_end = (int)(p - reqbuf) + 4;
                        cl = strstr(reqbuf, "Content-Length:");
                        if (NULL == cl)
                        {
                            cl = strstr(reqbuf, "content-length:");
                        }
                        if (NULL != cl)
                        {
                            cl += 15;
                            while (' ' == *cl)
                            {
                                cl++;
                            }
                            while (*cl >= '0' && *cl <= '9')
                            {
                                content_len = content_len * 10 + (*cl - '0');
                                cl++;
                            }
                            have_cl = 1;
                        }
                        if (!have_cl || content_len <= 0)
                        {
                            break;          /* no body (e.g. GET) */
                        }
                        if (n >= header_end + content_len)
                        {
                            break;          /* short body fit in header read */
                        }
                    }
                }
            }
        }
        if (n > 0)
        {
            reqbuf[n] = '\0';
            /* POST /compile — body is C source.  MVP: only the trivial
             * program "int main() { return N; }".  Emits ARM32 (movw/movt
             * + bx lr) into a fresh kmalloc buffer, calls it, returns
             * "rc=<status> ret=<value> size=<bytes>".  Pi 4 equivalent
             * is /compile with a much richer cc.c.  Pi 3 codegen will
             * grow over future commits. */
            if (0 == strncmp(reqbuf, "POST /compile", 13) ||
                0 == strncmp(reqbuf, "GET /compile",  12))
            {
                /* body starts after header_end */
                int hend = -1;
                {
                    char *p = strstr(reqbuf, "\r\n\r\n");
                    if (NULL != p) hend = (int)(p - reqbuf) + 4;
                }
                const char *body = (hend >= 0) ? reqbuf + hend : reqbuf;
                extern int cc_mvp_compile_and_run(const char *, long *, int *);
                long ret = 0;
                int  sz = 0;
                int  rc = cc_mvp_compile_and_run(body, &ret, &sz);
                static char cresp[300];
                int blen = sprintf(cresp + 100,
                                   "rc=%d ret=%ld code_size=%d\n", rc, ret, sz);
                int hlen = sprintf(cresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(cresp + hlen, cresp + 100, blen);
                write(tcpdev, cresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/actor-age?id=N — ms since the actor's last mailbox
             * activity (enq or deq).  Pi 4 equivalent: cc_actor_age. */
            if (0 == strncmp(reqbuf, "GET /api/actor-age", 18) ||
                0 == strncmp(reqbuf, "POST /api/actor-age", 19))
            {
                int aid = -1;
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url)
                {
                    const char *p = strstr(url, "id=");
                    if (NULL != p)
                    {
                        p += 3;
                        aid = 0;
                        while (*p >= '0' && *p <= '9')
                            aid = aid * 10 + (*p++ - '0');
                    }
                }
                extern long abcl_object_age_ms(int);
                extern int  abcl_n_objects(void);
                static char ageresp[200];
                int blen;
                if (aid < 0)
                    blen = sprintf(ageresp + 100, "usage: /api/actor-age?id=N\r\n");
                else if (aid >= abcl_n_objects())
                    blen = sprintf(ageresp + 100, "no such actor %d\r\n", aid);
                else
                    blen = sprintf(ageresp + 100,
                                   "actor=%d age_ms=%ld\r\n",
                                   aid, abcl_object_age_ms(aid));
                int hlen = sprintf(ageresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(ageresp + hlen, ageresp + 100, blen);
                write(tcpdev, ageresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /gc?threshold_ms=N[&dry=0|1] — sweep actors older than
             * threshold.  Pi 4 equivalent: /gc with same query params. */
            if (0 == strncmp(reqbuf, "GET /gc", 7) ||
                0 == strncmp(reqbuf, "POST /gc", 8))
            {
                long threshold = 5000;
                int  dry = 0;
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url)
                {
                    const char *p = strstr(url, "threshold_ms=");
                    if (NULL != p)
                    {
                        p += 13; threshold = 0;
                        while (*p >= '0' && *p <= '9')
                            threshold = threshold * 10 + (*p++ - '0');
                    }
                    const char *q = strstr(url, "dry=");
                    if (NULL != q)
                    {
                        q += 4;
                        if (*q >= '0' && *q <= '9') dry = (*q - '0');
                    }
                }
                extern int abcl_gc_sweep(long, int, int *);
                int scanned = 0;
                int killed  = abcl_gc_sweep(threshold, dry, &scanned);
                static char gcresp[300];
                int blen = sprintf(gcresp + 100,
                                   "killed=%d scanned=%d threshold_ms=%ld%s\r\n",
                                   killed, scanned, threshold,
                                   dry ? " (dry-run)" : "");
                int hlen = sprintf(gcresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(gcresp + hlen, gcresp + 100, blen);
                write(tcpdev, gcresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/actor-kill?id=N — force-kill AIPL actor N by killing
             * its Xinu worker thread.  Pi 4 equivalent: cc_actor_kill /
             * /gc with low threshold.  Pi 3 AIPL actor slots don't
             * recycle (n_objects monotonically grows), so this only
             * tears down the thread; the slot stays as "dead".  Useful
             * for cleaning up wedged actors without rebooting. */
            if (0 == strncmp(reqbuf, "GET /api/actor-kill", 19) ||
                0 == strncmp(reqbuf, "POST /api/actor-kill", 20))
            {
                int kid = -1;
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url)
                {
                    const char *p = strstr(url, "id=");
                    if (NULL != p)
                    {
                        p += 3;
                        kid = 0;
                        while (*p >= '0' && *p <= '9')
                        {
                            kid = kid * 10 + (*p - '0');
                            p++;
                        }
                    }
                }
                extern int abcl_n_objects(void);
                extern int abcl_object_tid(int);
                static char kresp[200];
                int blen;
                if (kid < 0)
                {
                    blen = sprintf(kresp + 100,
                                   "usage: /api/actor-kill?id=N\r\n");
                }
                else if (kid >= abcl_n_objects())
                {
                    blen = sprintf(kresp + 100, "no such actor %d\r\n", kid);
                }
                else
                {
                    int tid = abcl_object_tid(kid);
                    int rc = (tid >= 0) ? kill((tid_typ)tid) : -1;
                    blen = sprintf(kresp + 100,
                                   "actor=%d tid=%d kill_rc=%d\r\n",
                                   kid, tid, rc);
                }
                int hlen = sprintf(kresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(kresp + hlen, kresp + 100, blen);
                write(tcpdev, kresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /mmio-read?addr=0xN — peek a 32-bit register.  No fault
             * catching on Pi 3 (no setjmp/longjmp safety net like Pi 4's
             * safe_mmio_read32) so a bad address will hard-fault the
             * kernel — only use for known-mapped registers.  Default
             * addr 0x3F202000 (SDHOST CMD register on BCM2837). */
            if (0 == strncmp(reqbuf, "GET /mmio-read", 14) ||
                0 == strncmp(reqbuf, "POST /mmio-read", 15))
            {
                unsigned long addr = 0x3F202000UL;
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url)
                {
                    const char *p = strstr(url, "addr=");
                    if (NULL != p)
                    {
                        p += 5;
                        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
                        addr = 0;
                        while (*p && *p != '&' && *p != ' ' &&
                               *p != '\r' && *p != '\n')
                        {
                            char c = *p++;
                            unsigned int d;
                            if (c >= '0' && c <= '9') d = c - '0';
                            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                            else break;
                            addr = (addr << 4) | d;
                        }
                    }
                }
                volatile unsigned int *p = (volatile unsigned int *)addr;
                unsigned int v = *p;
                static char mresp[300];
                int blen = sprintf(mresp + 100,
                                   "addr=0x%08lx val=0x%08x\r\n",
                                   addr, v);
                int hlen = sprintf(mresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(mresp + hlen, mresp + 100, blen);
                write(tcpdev, mresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /upload?dst=NAME — receive a binary file POST body and
             * store in the upload slot.  Pi 4 has nothing equivalent;
             * this is the Pi 3-side groundwork for eventual kernel.img
             * network update.  Maximum size = WEB_BUFSZ (64 KB) minus
             * the headers — sufficient for small payloads.  Larger
             * (kernel-sized) uploads will need a separate streamed
             * reader, deferred to a future commit. */
            if (0 == strncmp(reqbuf, "POST /upload", 12) ||
                0 == strncmp(reqbuf, "GET /upload",  11))
            {
                /* Parse ?dst=NAME from URL */
                static char upload_name[64];
                /* 512 KB so a full xinu.boot fits for /kexec network update */
                static unsigned char upload_data[524288];
                static int  upload_size = 0;
                upload_name[0] = '\0';
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url)
                {
                    const char *q = strchr(url, '?');
                    if (NULL != q)
                    {
                        const char *d = strstr(q, "dst=");
                        if (NULL != d)
                        {
                            d += 4;
                            int i = 0;
                            while (*d && *d != '&' && *d != ' ' &&
                                   *d != '\r' && *d != '\n' && i < 63)
                            {
                                upload_name[i++] = *d++;
                            }
                            upload_name[i] = '\0';
                        }
                    }
                }

                /* Find header_end + body span in reqbuf */
                int header_end_local = -1;
                {
                    char *p = strstr(reqbuf, "\r\n\r\n");
                    if (NULL != p) header_end_local = (int)(p - reqbuf) + 4;
                }
                int body_len = 0;
                if (header_end_local >= 0 && header_end_local <= n)
                {
                    body_len = n - header_end_local;
                    if (body_len > (int)sizeof(upload_data))
                        body_len = sizeof(upload_data);
                    memcpy(upload_data, reqbuf + header_end_local, body_len);
                }
                upload_size = body_len;

                static char uresp[256];
                int blen = sprintf(uresp + 100,
                                   "uploaded dst=\"%s\" size=%d\r\n",
                                   upload_name[0] ? upload_name : "(unnamed)",
                                   upload_size);
                int hlen = sprintf(uresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(uresp + hlen, uresp + 100, blen);
                write(tcpdev, uresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;

                /* Make the slot visible to /api/upload-info by promoting
                 * the locals to file-scope.  Forward declarations live
                 * outside the request loop — see _upload_meta() below. */
                extern void _upload_set(const char *name, const unsigned char *data, int size);
                _upload_set(upload_name, upload_data, upload_size);
                continue;
            }
            /* /api/upload-info — name + size + first 32 bytes hex of the
             * most recently received /upload payload.  Quick sanity check
             * that what Mac sent is what Pi 3 received intact. */
            if (0 == strncmp(reqbuf, "GET /api/upload-info", 20) ||
                0 == strncmp(reqbuf, "POST /api/upload-info", 21))
            {
                extern const char *_upload_name(void);
                extern const unsigned char *_upload_data(void);
                extern int   _upload_size(void);
                const unsigned char *d = _upload_data();
                int sz = _upload_size();
                int show = sz < 32 ? sz : 32;
                static char iresp[700];
                int blen = sprintf(iresp + 100,
                                   "name=\"%s\"\nsize=%d\nfirst%d=",
                                   _upload_name(), sz, show);
                int i;
                for (i = 0; i < show; i++)
                {
                    blen += sprintf(iresp + 100 + blen, "%02x", d[i]);
                }
                blen += sprintf(iresp + 100 + blen, "\n");
                int hlen = sprintf(iresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(iresp + hlen, iresp + 100, blen);
                write(tcpdev, iresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/object-field?id=N&field=K — read actor N's field K.
             * Pi 3 AIPL fields are value_t (int|float|string|...).  We
             * render as decimal int (most common case); other types
             * print as hex tag for now. */
            if (0 == strncmp(reqbuf, "GET /api/object-field", 21) ||
                0 == strncmp(reqbuf, "POST /api/object-field", 22))
            {
                int field_id = -1, field_idx = -1;
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url)
                {
                    const char *q = strchr(url, '?');
                    if (NULL != q)
                    {
                        const char *p = q + 1;
                        while (*p && *p != ' ' && *p != '\r' && *p != '\n')
                        {
                            const char *eq = p;
                            while (*eq && *eq != '=' && *eq != '&') eq++;
                            const char *amp = (*eq == '=') ? eq + 1 : eq;
                            while (*amp && *amp != '&' && *amp != ' ' &&
                                   *amp != '\r' && *amp != '\n') amp++;
                            int klen = eq - p;
                            int *target = NULL;
                            if (klen == 2 && 0 == strncmp(p, "id", 2))
                                target = &field_id;
                            else if (klen == 5 && 0 == strncmp(p, "field", 5))
                                target = &field_idx;
                            if (target && *eq == '=')
                            {
                                *target = 0;
                                const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                {
                                    *target = (*target) * 10 + (*d - '0');
                                    d++;
                                }
                            }
                            p = (*amp == '&') ? amp + 1 : amp;
                        }
                    }
                }
                static char fresp[400];
                int blen;
                if (field_id < 0 || field_idx < 0)
                {
                    blen = sprintf(fresp + 100,
                                   "usage: /api/object-field?id=N&field=K\r\n");
                }
                else
                {
                    extern int abcl_object_field_render(int, int, char *, int);
                    char fval[160];
                    int rc = abcl_object_field_render(field_id, field_idx,
                                                     fval, sizeof fval);
                    if (rc < 0)
                    {
                        blen = sprintf(fresp + 100,
                                       "id=%d field=%d: render error\r\n",
                                       field_id, field_idx);
                    }
                    else
                    {
                        blen = sprintf(fresp + 100,
                                       "id=%d field=%d %s\r\n",
                                       field_id, field_idx, fval);
                    }
                }
                int hlen = sprintf(fresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(fresp + hlen, fresp + 100, blen);
                write(tcpdev, fresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/actor-send?to=N&m=METHOD[&v=STRING]:
             * deliver one message to any AIPL actor (not just the
             * single web_receiver actor that the bare /).  Pi 4
             * equivalent: /actor/send?to=...&m=...&arg=... — Pi 4
             * passes int arg, here we pass string (Pi 3's
             * abcl_web_deliver is string-only). */
            if (0 == strncmp(reqbuf, "GET /api/actor-send", 19) ||
                0 == strncmp(reqbuf, "POST /api/actor-send", 20))
            {
                /* Find the URL portion: between the first space and the
                 * second.  e.g. "GET /api/actor-send?to=0&m=tick HTTP/1.0" */
                const char *url = strchr(reqbuf, ' ');
                int to_id = -1;
                char meth[32] = "recv";
                char val[200]  = "";
                if (NULL != url)
                {
                    const char *q = strchr(url, '?');
                    if (NULL != q)
                    {
                        /* Tiny query-string parser: key=value & key=value */
                        const char *p = q + 1;
                        while (*p && *p != ' ' && *p != '\r' && *p != '\n')
                        {
                            const char *eq = p;
                            while (*eq && *eq != '=' && *eq != '&' &&
                                   *eq != ' ' && *eq != '\r' && *eq != '\n') eq++;
                            const char *amp = (*eq == '=') ? eq + 1 : eq;
                            while (*amp && *amp != '&' && *amp != ' ' &&
                                   *amp != '\r' && *amp != '\n') amp++;
                            int klen = eq - p;
                            int vlen = (*eq == '=') ? (amp - (eq + 1)) : 0;
                            if (klen == 2 && 0 == strncmp(p, "to", 2))
                            {
                                to_id = 0;
                                const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                {
                                    to_id = to_id * 10 + (*d - '0');
                                    d++;
                                }
                            }
                            else if (klen == 1 && *p == 'm')
                            {
                                int ml = vlen < 31 ? vlen : 31;
                                memcpy(meth, eq + 1, ml); meth[ml] = '\0';
                            }
                            else if (klen == 1 && *p == 'v')
                            {
                                int vl = vlen < 199 ? vlen : 199;
                                memcpy(val, eq + 1, vl); val[vl] = '\0';
                            }
                            p = (*amp == '&') ? amp + 1 : amp;
                        }
                    }
                }
                static char sresp[300];
                int blen;
                if (to_id < 0)
                {
                    blen = sprintf(sresp + 100,
                                   "usage: /api/actor-send?to=N&m=METHOD[&v=STRING]\r\n");
                }
                else
                {
                    extern int  abcl_n_objects(void);
                    extern void abcl_web_deliver(int, const char *, const char *);
                    if (to_id >= abcl_n_objects())
                    {
                        blen = sprintf(sresp + 100, "no such actor %d\r\n", to_id);
                    }
                    else
                    {
                        abcl_web_deliver(to_id, meth, val);
                        blen = sprintf(sresp + 100,
                                       "delivered to=%d m=%s v=\"%s\"\r\n",
                                       to_id, meth, val);
                    }
                }
                int hlen = sprintf(sresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(sresp + hlen, sresp + 100, blen);
                write(tcpdev, sresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/actors: AIPL actor inventory.  Each row:
             *   obj_id class_id tid started dead enq deq drops
             * (enq-deq = current mailbox backlog; drops = lost to full mbox).
             * Pi 4 equivalent: /api/actors-gc (which also has GC marker).
             * Pi 3 doesn't yet have the GC sweep so this is the read-only
             * inventory only. */
            if (0 == strncmp(reqbuf, "GET /api/actors", 15) ||
                0 == strncmp(reqbuf, "POST /api/actors", 16))
            {
                extern int abcl_n_objects(void);
                extern int abcl_object_class_id(int);
                extern int abcl_object_tid(int);
                extern int abcl_object_enq(int);
                extern int abcl_object_deq(int);
                extern int abcl_object_drops(int);
                extern int abcl_object_started(int);
                extern int abcl_object_dead(int);
                static char aresp[2400];
                int n = abcl_n_objects();
                int blen = sprintf(aresp + 200,
                                   "n_objects=%d\n"
                                   "obj_id class_id tid started dead enq deq drops\n",
                                   n);
                int i;
                for (i = 0; i < n; i++)
                {
                    blen += sprintf(aresp + 200 + blen,
                                    "%d %d %d %d %d %d %d %d\n",
                                    i, abcl_object_class_id(i),
                                    abcl_object_tid(i),
                                    abcl_object_started(i),
                                    abcl_object_dead(i),
                                    abcl_object_enq(i),
                                    abcl_object_deq(i),
                                    abcl_object_drops(i));
                    if (blen > 1900) break;
                }
                int hlen = sprintf(aresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(aresp + hlen, aresp + 200, blen);
                write(tcpdev, aresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/threads: dump thrtab as plain text (id state prio name).
             * Useful Mac-side introspection — mirrors the in-kernel `ps`
             * shell command (which only reaches the serial console). */
            if (0 == strncmp(reqbuf, "GET /api/threads", 16) ||
                0 == strncmp(reqbuf, "POST /api/threads", 17))
            {
                static char tresp[2400];
                int tlen = sprintf(tresp + 200, "tid state prio name\n");
                int i;
                for (i = 0; i < NTHREAD; i++)
                {
                    if (thrtab[i].state == THRFREE) continue;
                    tlen += sprintf(tresp + 200 + tlen, "%d %s %d %s\n",
                                    i, state_name(thrtab[i].state),
                                    thrtab[i].prio, thrtab[i].name);
                    if (tlen > 1900) break;     /* one MTU cap */
                }
                int hlen = sprintf(tresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", tlen);
                memcpy(tresp + hlen, tresp + 200, tlen);
                write(tcpdev, tresp, hlen + tlen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/memstat: free memory + total reachable.  Lightweight,
             * fits in one packet. */
            if (0 == strncmp(reqbuf, "GET /api/memstat", 16) ||
                0 == strncmp(reqbuf, "POST /api/memstat", 17))
            {
                static char mresp[400];
                ulong free_mem = (ulong)memlist.length;
                int blen = sprintf(mresp + 100,
                                   "free_bytes=%lu\nfree_kb=%lu\n",
                                   free_mem, free_mem / 1024);
                int hlen = sprintf(mresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(mresp + hlen, mresp + 100, blen);
                write(tcpdev, mresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/submit?n=K&ms=M:
             *   Enqueue K compute tasks via the Dispatcher; each takes
             *   ms milliseconds.  Dispatcher routes to the least-loaded
             *   Worker, so K tasks across W workers takes roughly
             *   ceil(K/W)*ms wall-clock instead of K*ms serial.
             *   K defaults to LB_N_WORKERS, ms defaults to 200. */
            /* Match exact "submit" — reject "submit-sticky" / "submit_jit"
             * prefix-collisions by checking the char after "submit" is a
             * URL boundary (? or space).  Without this, /submit-sticky
             * silently fell through to non-sticky distribution. */
            if (((0 == strncmp(reqbuf, "GET /api/loadbal/submit",  23) &&
                  (reqbuf[23] == '?' || reqbuf[23] == ' ')) ||
                 (0 == strncmp(reqbuf, "POST /api/loadbal/submit", 24) &&
                  (reqbuf[24] == '?' || reqbuf[24] == ' '))))
            {
                extern void abcl_loadbal_submit(int, int);
                extern int  abcl_loadbal_worker_count(void);
                const char *url = strchr(reqbuf, ' ');
                int n_tasks = abcl_loadbal_worker_count();
                int work_ms = 200;
                if (NULL != url)
                {
                    const char *q = strchr(url, '?');
                    if (NULL != q)
                    {
                        const char *p = q + 1;
                        while (*p && *p != ' ' && *p != '\r' && *p != '\n')
                        {
                            const char *eq = p;
                            while (*eq && *eq != '=' && *eq != '&' &&
                                   *eq != ' ' && *eq != '\r' && *eq != '\n') eq++;
                            const char *amp = (*eq == '=') ? eq + 1 : eq;
                            while (*amp && *amp != '&' && *amp != ' ' &&
                                   *amp != '\r' && *amp != '\n') amp++;
                            int klen = eq - p;
                            if (klen == 1 && *p == 'n' && *eq == '=')
                            {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                n_tasks = v;
                            }
                            else if (klen == 2 && 0 == strncmp(p, "ms", 2) && *eq == '=')
                            {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                work_ms = v;
                            }
                            p = (*amp == '&') ? amp + 1 : amp;
                        }
                    }
                }
                if (n_tasks < 1)   n_tasks = 1;
                if (n_tasks > 64)  n_tasks = 64;     /* cap so we don't flood */
                if (work_ms < 0)   work_ms = 0;
                if (work_ms > 5000)work_ms = 5000;
                /* Backpressure: refuse with HTTP 429 if every enabled
                 * worker is already at the per-worker queue cap.  Lets
                 * the caller back off instead of letting the dispatcher
                 * mailbox absorb unbounded backlog. */
                extern int abcl_loadbal_can_accept(void);
                if (!abcl_loadbal_can_accept())
                {
                    static char busy[200];
                    int bb = sprintf(busy + 100,
                        "loadbal saturated (all workers at queue cap)\r\n");
                    int hh = sprintf(busy,
                        "HTTP/1.0 429 Too Many Requests\r\n"
                        "Content-Type: text/plain\r\n"
                        "Retry-After: 1\r\n"
                        "Content-Length: %d\r\n"
                        "\r\n", bb);
                    memcpy(busy + hh, busy + 100, bb);
                    write(tcpdev, busy, hh + bb);
                    close(tcpdev);
                    web_cur_tcpdev = -1;
                    continue;
                }
                /* Rate limit — bypassed for prio=high.  Priority is
                 * just "skip rate-limit + skip queue cap" here, which
                 * is the simplest meaningful priority semantic in a
                 * FIFO-mailbox actor system (true priority queues
                 * would need per-prio mailboxes, deferred to Tier 3). */
                {
                    int is_high = (NULL != strstr(reqbuf, "prio=high"));
                    extern int abcl_loadbal_rate_check(void);
                    if (!is_high && !abcl_loadbal_rate_check())
                    {
                        static char rl[200];
                        int bb = sprintf(rl + 100,
                            "loadbal rate-limited (token bucket empty; "
                            "use prio=high to bypass)\r\n");
                        int hh = sprintf(rl,
                            "HTTP/1.0 429 Too Many Requests\r\n"
                            "Content-Type: text/plain\r\n"
                            "Retry-After: 1\r\n"
                            "X-LB-Reason: rate-limit\r\n"
                            "Content-Length: %d\r\n"
                            "\r\n", bb);
                        memcpy(rl + hh, rl + 100, bb);
                        write(tcpdev, rl, hh + bb);
                        close(tcpdev);
                        web_cur_tcpdev = -1;
                        continue;
                    }
                }
                int i;
                for (i = 1; i <= n_tasks; i++)
                {
                    abcl_loadbal_submit(work_ms, i);
                }
                static char lresp[300];
                int blen = sprintf(lresp + 100,
                                   "submitted n=%d ms=%d (see /api/loadbal/stats)\r\n",
                                   n_tasks, work_ms);
                int hlen = sprintf(lresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(lresp + hlen, lresp + 100, blen);
                write(tcpdev, lresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /kexec — jump into the most recent /upload payload as a new
             * kernel.  This is the "network kernel update" loop:
             *
             *   1. Mac:  curl --data-binary @xinu.boot \
             *               http://192.168.3.50:8080/upload
             *   2. Mac:  curl -X POST http://192.168.3.50:8080/kexec
             *   3. Pi 3: webactor responds 200, drains TCP, disables MMU,
             *            calls kexec() which copies upload payload into
             *            place at 0x8000 and jumps there.
             *   4. New kernel boots fresh, re-enables MMU in platforminit,
             *            comes back up on the same IP.
             *
             * Sanity-checks: upload slot must be non-empty and at least
             * 32 KB (smaller is almost certainly not a kernel image).
             * Bad uploads just hang Pi 3 (recovery via SD swap), same as
             * a bricked /reboot — that's the cost of being a real
             * network-update path.
             *
             * MMU disable BEFORE kexec because the next kernel's start.S
             * expects MMU off; without it the second mmu_init double-
             * configures TTBR0 from an already-enabled state and would
             * almost certainly fault. */
            if (0 == strncmp(reqbuf, "POST /kexec", 11) ||
                0 == strncmp(reqbuf, "GET /kexec", 10))
            {
                extern syscall kexec(const void *, uint);
                extern void mmu_disable(void);
                int sz = upload_slot_size;
                if (sz < 32 * 1024)
                {
                    static char kresp[200];
                    int blen = sprintf(kresp + 100,
                        "kexec refused: upload too small (%d B, need >= 32 KB)\r\n"
                        "POST a kernel image to /upload first.\r\n", sz);
                    int hlen = sprintf(kresp,
                        "HTTP/1.0 400 Bad Request\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: %d\r\n"
                        "\r\n", blen);
                    memcpy(kresp + hlen, kresp + 100, blen);
                    write(tcpdev, kresp, hlen + blen);
                    close(tcpdev);
                    web_cur_tcpdev = -1;
                    continue;
                }
                /* Respond OK BEFORE we kill ourselves — Mac needs the 200 */
                static char kok[160];
                int blen = sprintf(kok + 100,
                    "kexec %d bytes — jumping...\r\n", sz);
                int hlen = sprintf(kok,
                    "HTTP/1.0 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %d\r\n"
                    "\r\n", blen);
                memcpy(kok + hlen, kok + 100, blen);
                write(tcpdev, kok, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                /* Drain TCP to the wire so Mac sees the 200, then commit. */
                sleep(50);
                kprintf("[kexec] disabling MMU, jumping to upload (%d B)\r\n", sz);
                mmu_disable();
                kexec(upload_slot_data, (uint)sz);
                /* defensive — kexec returns SYSERR if jump failed */
                while (1) { }
            }
            /* /api/mmu:
             *   Report MMU enable state + SCTLR + TTBR0 + page-table base.
             *   Lets a Mac-side script confirm the MMU stayed up after
             *   each kernel update (a bricked MMU enable would not
             *   respond at all, so seeing JSON means we won). */
            if (0 == strncmp(reqbuf, "GET /api/mmu", 12) ||
                0 == strncmp(reqbuf, "POST /api/mmu", 13))
            {
                extern int      mmu_is_enabled(void);
                extern uint32_t mmu_read_sctlr(void);
                extern uint32_t mmu_read_ttbr0(void);
                extern uint32_t mmu_table_base(void);
                static char mresp2[500];
                uint32_t sctlr = mmu_read_sctlr();
                int blen = sprintf(mresp2 + 100,
                    "enabled=%d\n"
                    "sctlr=0x%08x M=%d C=%d I=%d Z=%d\n"
                    "ttbr0=0x%08x\n"
                    "table_base=0x%08x\n",
                    mmu_is_enabled(), sctlr,
                    (sctlr >> 0)  & 1,
                    (sctlr >> 2)  & 1,
                    (sctlr >> 12) & 1,
                    (sctlr >> 11) & 1,
                    mmu_read_ttbr0(),
                    mmu_table_base());
                int hlen = sprintf(mresp2,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(mresp2 + hlen, mresp2 + 100, blen);
                write(tcpdev, mresp2, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/jit?n=K&prog=P:
             *   Like /api/loadbal/submit but the worker JIT-compiles a
             *   small C source and executes it (no sleep — wall-clock
             *   is dominated by JIT codegen + execution).  P selects
             *   from a static program table; default 4 (now_ms +
             *   actor_count, two builtin calls).  Round-robins task_id
             *   1..K across the workers via the least-loaded picker.
             *
             *   Same caps as /submit: n<=64, prog>=0. */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/jit", 20) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/jit", 21))
            {
                extern void abcl_loadbal_submit_jit(int, int);
                extern int  abcl_loadbal_worker_count(void);
                extern int  abcl_loadbal_n_progs(void);
                const char *url = strchr(reqbuf, ' ');
                int n_tasks = abcl_loadbal_worker_count();
                int prog_id = 4;       /* default: 2-builtin program */
                if (NULL != url)
                {
                    const char *q = strchr(url, '?');
                    if (NULL != q)
                    {
                        const char *p = q + 1;
                        while (*p && *p != ' ' && *p != '\r' && *p != '\n')
                        {
                            const char *eq = p;
                            while (*eq && *eq != '=' && *eq != '&' &&
                                   *eq != ' ' && *eq != '\r' && *eq != '\n') eq++;
                            const char *amp = (*eq == '=') ? eq + 1 : eq;
                            while (*amp && *amp != '&' && *amp != ' ' &&
                                   *amp != '\r' && *amp != '\n') amp++;
                            int klen = eq - p;
                            if (klen == 1 && *p == 'n' && *eq == '=')
                            {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                n_tasks = v;
                            }
                            else if (klen == 4 && 0 == strncmp(p, "prog", 4)
                                     && *eq == '=')
                            {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                prog_id = v;
                            }
                            p = (*amp == '&') ? amp + 1 : amp;
                        }
                    }
                }
                if (n_tasks < 1)  n_tasks = 1;
                if (n_tasks > 64) n_tasks = 64;
                if (prog_id < 0)                              prog_id = 0;
                if (prog_id >= abcl_loadbal_n_progs())        prog_id = 0;
                /* Same backpressure path as /api/loadbal/submit. */
                extern int abcl_loadbal_can_accept(void);
                if (!abcl_loadbal_can_accept())
                {
                    static char jbusy[200];
                    int bb = sprintf(jbusy + 100,
                        "loadbal saturated (all workers at queue cap)\r\n");
                    int hh = sprintf(jbusy,
                        "HTTP/1.0 429 Too Many Requests\r\n"
                        "Content-Type: text/plain\r\n"
                        "Retry-After: 1\r\n"
                        "Content-Length: %d\r\n"
                        "\r\n", bb);
                    memcpy(jbusy + hh, jbusy + 100, bb);
                    write(tcpdev, jbusy, hh + bb);
                    close(tcpdev);
                    web_cur_tcpdev = -1;
                    continue;
                }
                /* Rate-limit (same prio=high bypass as /submit). */
                {
                    int is_high = (NULL != strstr(reqbuf, "prio=high"));
                    extern int abcl_loadbal_rate_check(void);
                    if (!is_high && !abcl_loadbal_rate_check())
                    {
                        static char rl[200];
                        int bb = sprintf(rl + 100,
                            "loadbal rate-limited (use prio=high to bypass)\r\n");
                        int hh = sprintf(rl,
                            "HTTP/1.0 429 Too Many Requests\r\n"
                            "Content-Type: text/plain\r\n"
                            "Retry-After: 1\r\n"
                            "X-LB-Reason: rate-limit\r\n"
                            "Content-Length: %d\r\n"
                            "\r\n", bb);
                        memcpy(rl + hh, rl + 100, bb);
                        write(tcpdev, rl, hh + bb);
                        close(tcpdev);
                        web_cur_tcpdev = -1;
                        continue;
                    }
                }
                int i;
                for (i = 1; i <= n_tasks; i++)
                {
                    abcl_loadbal_submit_jit(prog_id, i);
                }
                static char jresp[300];
                int blen = sprintf(jresp + 100,
                                   "jit-submitted n=%d prog=%d (see /api/loadbal/stats)\r\n",
                                   n_tasks, prog_id);
                int hlen = sprintf(jresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(jresp + hlen, jresp + 100, blen);
                write(tcpdev, jresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/pause?w=N  — pause worker N (drain mode)
             * /api/loadbal/resume?w=N — resume worker N
             *   Paused worker takes no new dispatches; in-flight tasks
             *   still complete normally (load decrement via done).
             *   Use to gracefully retire a worker for an experiment or
             *   to test the dispatcher's "all paused" branch. */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/pause",   22) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/pause",  23) ||
                0 == strncmp(reqbuf, "GET /api/loadbal/resume",  23) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/resume", 24))
            {
                extern void abcl_loadbal_set_enabled(int, int);
                int on = (NULL != strstr(reqbuf, "resume")) ? 1 : 0;
                int widx = -1;
                const char *q = strstr(reqbuf, "?w=");
                if (NULL != q) {
                    q += 3;
                    widx = 0;
                    while (*q >= '0' && *q <= '9') {
                        widx = widx * 10 + (*q - '0');
                        q++;
                    }
                }
                static char presp[200];
                int blen;
                if (widx < 0 || widx >= 4) {
                    blen = sprintf(presp + 100,
                        "usage: /api/loadbal/{pause,resume}?w=0..3\r\n");
                } else {
                    abcl_loadbal_set_enabled(widx, on);
                    blen = sprintf(presp + 100,
                        "worker idx=%d set to %s\r\n",
                        widx, on ? "ENABLED" : "PAUSED");
                }
                int hlen = sprintf(presp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(presp + hlen, presp + 100, blen);
                write(tcpdev, presp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/submit-sticky?n=K&ms=M&key=NAME
             *   Sticky routing: all K tasks of the same key hash to the
             *   SAME worker (slot = djb2(key) % n_workers).  Useful for
             *   stateful workers or cache-locality patterns.  If the
             *   target slot is paused, walks forward to the next enabled
             *   worker (best-effort sticky).  Per-task task_id = 1..K
             *   like /submit, but all share one key_hash. */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/submit-sticky",  30) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/submit-sticky", 31))
            {
                extern int  abcl_loadbal_worker_count(void);
                extern void abcl_loadbal_submit_sticky(int, int, int);
                extern int  abcl_loadbal_hash_key(const char *, int);
                extern int  abcl_loadbal_can_accept(void);
                int n_tasks = abcl_loadbal_worker_count();
                int work_ms = 200;
                char key[64] = "default";
                int  klen = 7;
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url) {
                    const char *q = strchr(url, '?');
                    if (NULL != q) {
                        const char *p = q + 1;
                        while (*p && *p != ' ' && *p != '\r' && *p != '\n') {
                            const char *eq = p;
                            while (*eq && *eq != '=' && *eq != '&' &&
                                   *eq != ' ' && *eq != '\r' && *eq != '\n') eq++;
                            const char *amp = (*eq == '=') ? eq + 1 : eq;
                            while (*amp && *amp != '&' && *amp != ' ' &&
                                   *amp != '\r' && *amp != '\n') amp++;
                            int kl = eq - p;
                            if (kl == 1 && *p == 'n' && *eq == '=') {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                n_tasks = v;
                            } else if (kl == 2 && 0 == strncmp(p, "ms", 2) && *eq == '=') {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                work_ms = v;
                            } else if (kl == 3 && 0 == strncmp(p, "key", 3) && *eq == '=') {
                                int kvlen = amp - (eq + 1);
                                if (kvlen > 63) kvlen = 63;
                                memcpy(key, eq + 1, kvlen);
                                key[kvlen] = '\0';
                                klen = kvlen;
                            }
                            p = (*amp == '&') ? amp + 1 : amp;
                        }
                    }
                }
                if (n_tasks < 1)   n_tasks = 1;
                if (n_tasks > 64)  n_tasks = 64;
                if (work_ms < 0)   work_ms = 0;
                if (work_ms > 5000)work_ms = 5000;
                if (!abcl_loadbal_can_accept()) {
                    static char busy3[200];
                    int bb = sprintf(busy3 + 100,
                        "loadbal saturated (all workers at queue cap)\r\n");
                    int hh = sprintf(busy3,
                        "HTTP/1.0 429 Too Many Requests\r\n"
                        "Content-Type: text/plain\r\n"
                        "Retry-After: 1\r\n"
                        "Content-Length: %d\r\n"
                        "\r\n", bb);
                    memcpy(busy3 + hh, busy3 + 100, bb);
                    write(tcpdev, busy3, hh + bb);
                    close(tcpdev);
                    web_cur_tcpdev = -1;
                    continue;
                }
                int hash = abcl_loadbal_hash_key(key, klen);
                int i;
                for (i = 1; i <= n_tasks; i++) {
                    abcl_loadbal_submit_sticky(work_ms, i, hash);
                }
                static char sresp2[300];
                int blen = sprintf(sresp2 + 100,
                    "sticky-submitted n=%d ms=%d key=\"%s\" hash=%d "
                    "(routed to slot=%d)\r\n",
                    n_tasks, work_ms, key, hash,
                    hash % (n_tasks > 0 ? abcl_loadbal_worker_count() : 1));
                int hlen = sprintf(sresp2,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(sresp2 + hlen, sresp2 + 100, blen);
                write(tcpdev, sresp2, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/dining/init — spin up DiningBench orchestrator actor.
             *   POST /api/dining/start?mode=N&meals=M
             *   GET  /api/dining/status — n_done / elapsed_ms / max_phil_ms
             *
             *   mode 0: 5 philosophers in parallel (classical)
             *   mode 1: 3+2 staggered  (P0..P2 first, then P3+P4)
             *   mode 2: sequential     (one philosopher at a time)
             *
             * Each mode spawns 5 Forks + 5 Philosophers, so the
             * DiningBench needs the GC actor to reap the previous run's
             * residue (or accept the n_objects accumulation up to
             * MAX_OBJECTS=32). */
            if (0 == strncmp(reqbuf, "GET /api/dining/init",  20) ||
                0 == strncmp(reqbuf, "POST /api/dining/init", 21))
            {
                extern void abcl_dining_init(void);
                extern int  abcl_dining_actor_id(void);
                abcl_dining_init();
                static char diresp[200];
                int blen = sprintf(diresp + 100,
                    "dining initialized (obj=%d)\r\n",
                    abcl_dining_actor_id());
                int hlen = sprintf(diresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(diresp + hlen, diresp + 100, blen);
                write(tcpdev, diresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            if (0 == strncmp(reqbuf, "GET /api/dining/start",  21) ||
                0 == strncmp(reqbuf, "POST /api/dining/start", 22))
            {
                extern void abcl_dining_start(int, int);
                int mode = 0, meals = 50;
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url) {
                    const char *q = strchr(url, '?');
                    if (NULL != q) {
                        const char *p = q + 1;
                        while (*p && *p != ' ' && *p != '\r' && *p != '\n') {
                            const char *eq = p;
                            while (*eq && *eq != '=' && *eq != '&' &&
                                   *eq != ' ' && *eq != '\r' && *eq != '\n') eq++;
                            const char *amp = (*eq == '=') ? eq + 1 : eq;
                            while (*amp && *amp != '&' && *amp != ' ' &&
                                   *amp != '\r' && *amp != '\n') amp++;
                            int kl = eq - p;
                            if (kl == 4 && 0 == strncmp(p, "mode", 4)
                                && *eq == '=') {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                mode = v;
                            } else if (kl == 5 && 0 == strncmp(p, "meals", 5)
                                       && *eq == '=') {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                meals = v;
                            }
                            p = (*amp == '&') ? amp + 1 : amp;
                        }
                    }
                }
                if (mode < 0)  mode = 0;
                if (mode > 3)  mode = 3;     /* 0=par 1=stag 2=seq 3=CM */
                if (meals < 1) meals = 1;
                if (meals > 100) meals = 100;
                abcl_dining_start(mode, meals);
                static char dsresp[200];
                int blen = sprintf(dsresp + 100,
                    "dining start mode=%d meals=%d (poll /api/dining/status)\r\n",
                    mode, meals);
                int hlen = sprintf(dsresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(dsresp + hlen, dsresp + 100, blen);
                write(tcpdev, dsresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            if (0 == strncmp(reqbuf, "GET /api/dining/status",  22) ||
                0 == strncmp(reqbuf, "POST /api/dining/status", 23))
            {
                extern int abcl_dining_status(char *, int);
                static char dst[400];
                int blen = abcl_dining_status(dst + 100, 300);
                int hlen = sprintf(dst,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(dst + hlen, dst + 100, blen);
                write(tcpdev, dst, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/wifi/probe — Stage 1+2 WiFi (BCM43455) SDIO bring-up:
             *   power the chip, enumerate SDIO, read back the silicon
             *   chip-id.  All detail is on the serial console ([wifi] ...);
             *   the HTTP reply only carries the final rc. */
            if (0 == strncmp(reqbuf, "GET /api/wifi/probe",  19) ||
                0 == strncmp(reqbuf, "POST /api/wifi/probe", 20))
            {
                extern int wifi_probe(void);
                extern const char *wifi_trace(void);
                int rc = wifi_probe();
                const char *tr = wifi_trace();
                static char whdr[160];
                int blen, hlen;
                blen = 0; while (tr[blen] && blen < 3900) blen++;
                hlen = sprintf(whdr,
                               "HTTP/1.0 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "X-Wifi-RC: %d\r\n"
                               "Content-Length: %d\r\n"
                               "\r\n", rc, blen);
                write(tcpdev, whdr, hlen);
                write(tcpdev, (void *)tr, blen);   /* full [wifi] trace */
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/wifi/scan — scan and return the AP list as JSON (for a
             *   selection UI). */
            if (0 == strncmp(reqbuf, "GET /api/wifi/scan",  18) ||
                0 == strncmp(reqbuf, "POST /api/wifi/scan", 19))
            {
                extern int wifi_scan_json(char *, int);
                static char wjson[4096];
                static char wjhdr[120];
                int blen = wifi_scan_json(wjson, sizeof(wjson));
                int hlen = sprintf(wjhdr,
                               "HTTP/1.0 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %d\r\n\r\n", blen);
                write(tcpdev, wjhdr, hlen);
                write(tcpdev, wjson, blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/wifi/trace — return the last wifi trace buffer verbatim,
             *   WITHOUT running anything.  This responds instantly, so the TCP
             *   body delivers reliably (unlike /join, whose ~150 s delay leaves
             *   the large body undeliverable).  Write in <=1024-B chunks. */
            if (0 == strncmp(reqbuf, "GET /api/wifi/trace",  19) ||
                0 == strncmp(reqbuf, "POST /api/wifi/trace", 20))
            {
                extern const char *wifi_trace(void);
                const char *tr = wifi_trace();
                static char thdr[96];
                int blen = 0, off = 0, hlen;
                while (tr[blen]) blen++;
                hlen = sprintf(thdr, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                                     "Content-Length: %d\r\n\r\n", blen);
                write(tcpdev, thdr, hlen);
                while (off < blen) {
                    int n = blen - off; if (n > 1024) n = 1024;
                    write(tcpdev, (void *)(tr + off), n);
                    off += n;
                }
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/wifi/ping?ip=A.B.C.D — ICMP echo client (default 8.8.8.8).
             * /api/wifi/ntp?server=A.B.C.D — NTP time client (default Cloudflare
             *   162.159.200.123).  Both route off-subnet via the gateway, so they
             *   reach the internet through e.g. an iPhone hotspot.  Full log in
             *   /api/wifi/trace; result summary in headers. */
            if (0 == strncmp(reqbuf, "GET /api/wifi/ping", 18)) {
                extern int wifi_ping(const unsigned char*, int);
                unsigned char ip[4] = {8,8,8,8};
                const char *q = strstr(reqbuf, "ip=");
                static char ph[160]; int rc, hlen;
                if (q) { int o=0,v=0; const char *p=q+3;
                    for (; *p && *p!=' '&&*p!='&'; p++) {
                        if (*p=='.') { if(o<4) ip[o]=v; o++; v=0; }
                        else if (*p>='0'&&*p<='9') v=v*10+(*p-'0');
                    }
                    if (o<4) ip[o]=v;
                }
                rc = wifi_ping(ip, 4);
                hlen = sprintf(ph, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                    "X-Wifi-PingReplies: %d\r\nContent-Length: 0\r\n\r\n", rc);
                write(tcpdev, ph, hlen); close(tcpdev); web_cur_tcpdev = -1; continue;
            }
            if (0 == strncmp(reqbuf, "GET /api/wifi/ntp", 17)) {
                extern unsigned long wifi_ntp(const unsigned char*);
                unsigned char srv[4] = {162,159,200,123};   /* time.cloudflare.com (anycast) */
                const char *q = strstr(reqbuf, "server=");
                static char nh[160]; unsigned long t; int hlen;
                if (q) { int o=0,v=0; const char *p=q+7;
                    for (; *p && *p!=' '&&*p!='&'; p++) {
                        if (*p=='.') { if(o<4) srv[o]=v; o++; v=0; }
                        else if (*p>='0'&&*p<='9') v=v*10+(*p-'0');
                    }
                    if (o<4) srv[o]=v;
                }
                t = wifi_ntp(srv);
                hlen = sprintf(nh, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                    "X-Wifi-NtpUnix: %lu\r\nContent-Length: 0\r\n\r\n", t);
                write(tcpdev, nh, hlen); close(tcpdev); web_cur_tcpdev = -1; continue;
            }
            /* /api/wifi/browse?ip=&host= — fetch http://host and render it as a
             *   window on the HDMI framebuffer (Xinu's screen). */
            if (0 == strncmp(reqbuf, "GET /api/wifi/browse", 20)) {
                extern int wifi_browse_xy(const unsigned char*, const char*, int, int, int, int);
                extern int atoi(const char *);
                unsigned char ip[4] = {160,251,151,122};
                static char host[64] = "kodamay.org";
                const char *qi = strstr(reqbuf, "ip="), *qh = strstr(reqbuf, "host=");
                const char *qwx=strstr(reqbuf,"wx="), *qwy=strstr(reqbuf,"wy=");
                const char *qww=strstr(reqbuf,"ww="), *qwh=strstr(reqbuf,"wh=");
                int wx = qwx?atoi(qwx+3):160, wy = qwy?atoi(qwy+3):140;
                int ww = qww?atoi(qww+3):704, wh = qwh?atoi(qwh+3):480;
                static char bh[120]; int n, hlen;
                if (qi) { int o=0,v=0; const char *p=qi+3;
                    for (; *p && *p!=' '&&*p!='&'; p++) {
                        if (*p=='.') { if(o<4) ip[o]=v; o++; v=0; }
                        else if (*p>='0'&&*p<='9') v=v*10+(*p-'0');
                    } if (o<4) ip[o]=v;
                }
                if (qh) { int k=0; const char *p=qh+5;
                    for (; *p && *p!=' '&&*p!='&' && k<(int)sizeof(host)-1; p++) host[k++]=*p;
                    host[k]='\0';
                }
                n = wifi_browse_xy(ip, host, wx, wy, ww, wh);
                hlen = sprintf(bh, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                    "X-Wifi-BrowseBytes: %d\r\nContent-Length: 0\r\n\r\n", n);
                write(tcpdev, bh, hlen); close(tcpdev); web_cur_tcpdev = -1; continue;
            }
            /* /api/wifi/http?ip=A.B.C.D&host=NAME — minimal HTTP/1.0 GET over a
             *   hand-rolled TCP client (default kodamay.org 160.251.151.122).
             *   Returns the fetched page as the body; also printed to serial. */
            if (0 == strncmp(reqbuf, "GET /api/wifi/http", 18)) {
                extern int wifi_http(const unsigned char*, const char*);
                extern int wifi_http_get_buf(char**);
                unsigned char ip[4] = {160,251,151,122};
                static char host[64] = "kodamay.org";
                const char *qi = strstr(reqbuf, "ip="), *qh = strstr(reqbuf, "host=");
                static char hh[96]; char *body; int blen, off=0, hlen, n;
                if (qi) { int o=0,v=0; const char *p=qi+3;
                    for (; *p && *p!=' '&&*p!='&'; p++) {
                        if (*p=='.') { if(o<4) ip[o]=v; o++; v=0; }
                        else if (*p>='0'&&*p<='9') v=v*10+(*p-'0');
                    } if (o<4) ip[o]=v;
                }
                if (qh) { int k=0; const char *p=qh+5;
                    for (; *p && *p!=' '&&*p!='&' && k<(int)sizeof(host)-1; p++) host[k++]=*p;
                    host[k]='\0';
                }
                n = wifi_http(ip, host);
                blen = wifi_http_get_buf(&body);
                hlen = sprintf(hh, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                    "X-Wifi-HttpBytes: %d\r\nContent-Length: %d\r\n\r\n", n, blen > 0 ? blen : 0);
                write(tcpdev, hh, hlen);
                while (off < blen) { int c = blen-off; if (c>1024) c=1024;
                    write(tcpdev, body+off, c); off += c; }
                close(tcpdev); web_cur_tcpdev = -1; continue;
            }
            /* /api/wifi/dhcp — run a DHCP DISCOVER/REQUEST over the associated
             *   link and report the leased IP.  (Read /api/wifi/trace for the
             *   full DHCP log.) */
            if (0 == strncmp(reqbuf, "GET /api/wifi/dhcp",  18) ||
                0 == strncmp(reqbuf, "POST /api/wifi/dhcp", 19))
            {
                extern int  wifi_dhcp(void);
                extern void wifi_dhcp_diag(unsigned char*, unsigned char*, int*);
                extern void wifi_net_service(void);
                extern int  wifi_net_active(void);
                static char dhdr[256], dbody[160];
                static int  net_started = 0;
                unsigned char ip[4], gw[4]; int have, rc, blen, hlen;
                rc = wifi_dhcp();
                wifi_dhcp_diag(ip, gw, &have);
                /* once we have an IP, start the ARP/ICMP responder so the host
                 * can ping/reach us over wlan (idempotent) */
                if (rc == 0 && have && !net_started && !wifi_net_active()) {
                    tid_typ nt = create((void *)wifi_net_service, 8192, INITPRIO,
                                        "wifi-net", 0);
                    if (nt != SYSERR) { ready(nt, RESCHED_NO); net_started = 1; }
                }
                blen = sprintf(dbody, "dhcp rc=%d have_ip=%d ip=%d.%d.%d.%d gw=%d.%d.%d.%d\r\n",
                               rc, have, ip[0],ip[1],ip[2],ip[3], gw[0],gw[1],gw[2],gw[3]);
                hlen = sprintf(dhdr,
                               "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                               "X-Wifi-DhcpRC: %d\r\nX-Wifi-HaveIP: %d\r\n"
                               "X-Wifi-IP: %d.%d.%d.%d\r\nX-Wifi-GW: %d.%d.%d.%d\r\n"
                               "Content-Length: %d\r\n\r\n",
                               rc, have, ip[0],ip[1],ip[2],ip[3], gw[0],gw[1],gw[2],gw[3], blen);
                write(tcpdev, dhdr, hlen);
                write(tcpdev, dbody, blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/wifi/join?ssid=NAME&pass=PASS — connect to an AP (WPA2-PSK
             *   if pass given, else open).  Full [wifi] trace in the reply. */
            if (0 == strncmp(reqbuf, "GET /api/wifi/join",  18) ||
                0 == strncmp(reqbuf, "POST /api/wifi/join", 19))
            {
                extern int wifi_join(const char *, const char *);
                extern const char *wifi_trace(void);
                static char ssid[40], pass[68];
                const char *url = strchr(reqbuf, ' ');
                const char *q = url ? strchr(url, '?') : NULL;
                int rc, blen, hlen; const char *tr;
                static char jhdr[480];
                ssid[0] = pass[0] = '\0';
                if (q) {
                    const char *p = q + 1;
                    while (*p && *p != ' ' && *p != '\r' && *p != '\n') {
                        char *dst = NULL; int dcap = 0, k = 0;
                        if (0 == strncmp(p, "ssid=", 5)) { dst = ssid; dcap = sizeof(ssid)-1; p += 5; }
                        else if (0 == strncmp(p, "pass=", 5)) { dst = pass; dcap = sizeof(pass)-1; p += 5; }
                        while (*p && *p != '&' && *p != ' ' && *p != '\r' && *p != '\n') {
                            char c = *p;
                            if (c == '+') {                 /* urldecode: + -> space */
                                c = ' ';
                            } else if (c == '%' && p[1] && p[2]) {  /* %XX -> byte */
                                char h1 = p[1], h2 = p[2]; int v1, v2;
                                v1 = (h1>='0'&&h1<='9')?h1-'0':(h1>='a'&&h1<='f')?h1-'a'+10:(h1>='A'&&h1<='F')?h1-'A'+10:-1;
                                v2 = (h2>='0'&&h2<='9')?h2-'0':(h2>='a'&&h2<='f')?h2-'a'+10:(h2>='A'&&h2<='F')?h2-'A'+10:-1;
                                if (v1 >= 0 && v2 >= 0) { c = (char)((v1<<4)|v2); p += 2; }
                            }
                            if (dst && k < dcap) dst[k++] = c;
                            p++;
                        }
                        if (dst) dst[k] = '\0';
                        if (*p == '&') p++;
                    }
                }
                rc = wifi_join(ssid, pass);
                tr = wifi_trace(); (void)tr;
                /* Do NOT send the large trace as the body: a multi-KB write
                 * right after the long join can block Xinu's TCP and wedge the
                 * webactor for the next request.  Diagnostics go in the headers;
                 * fetch the full log separately via GET /api/wifi/trace. */
                blen = 0;
                {
                    /* Surface the join diagnostics in headers — the small header
                     * block always reaches the client even when the long join
                     * leaves the large trace body undeliverable over TCP. */
                    extern void wifi_diag(int*,int*,int*,int*,int*,int*,int*);
                    extern int  wifi_diag_seq(int*, int);
                    extern int  wifi_tgt_diag(int*, int*);
                    int sup, pmk, nev, eapol, link, lastev, laststat;
                    int seq[16], sn, si, sl = 0; char seqbuf[96];
                    int tfound = 0, tchsp = 0;
                    wifi_diag(&sup,&pmk,&nev,&eapol,&link,&lastev,&laststat);
                    wifi_tgt_diag(&tfound, &tchsp);
                    sn = wifi_diag_seq(seq, 16);
                    for (si = 0; si < sn && sl < (int)sizeof(seqbuf)-8; si++)
                        sl += sprintf(seqbuf + sl, si ? ",%d" : "%d", seq[si]);
                    seqbuf[sl] = '\0';
                    hlen = sprintf(jhdr,
                               "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                               "X-Wifi-RC: %d\r\n"
                               "X-Wifi-Sup: %d\r\nX-Wifi-Pmk: %d\r\n"
                               "X-Wifi-Events: %d\r\nX-Wifi-LastEvent: %d\r\n"
                               "X-Wifi-LastStatus: %d\r\nX-Wifi-Eapol: %d\r\n"
                               "X-Wifi-Link: %d\r\nX-Wifi-EvSeq: %s\r\n"
                               "X-Wifi-Tgt: %d\r\nX-Wifi-Chanspec: 0x%04x\r\n"
                               "Content-Length: %d\r\n\r\n",
                               rc, sup, pmk, nev, lastev, laststat, eapol, link,
                               seqbuf, tfound, tchsp, blen);
                }
                write(tcpdev, jhdr, hlen);   /* headers only; body is empty */
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/init — spin up Dispatcher + 4 Workers if
             *   not already running.  Idempotent.  After cold boot
             *   the load-balancer actors don't exist; this creates
             *   them on first use. */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/init",  21) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/init", 22))
            {
                extern void abcl_loadbal_init(void);
                extern int  abcl_loadbal_dispatcher_id(void);
                abcl_loadbal_init();
                static char iresp[200];
                int blen = sprintf(iresp + 100,
                    "loadbal initialized (dispatcher_obj=%d, 4 workers)\r\n",
                    abcl_loadbal_dispatcher_id());
                int hlen = sprintf(iresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(iresp + hlen, iresp + 100, blen);
                write(tcpdev, iresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/gc-actor/init — spin up Collector actor + heartbeat
             *   thread.  Idempotent.  After cold boot the GC actor
             *   doesn't exist; this creates it.  Should be called
             *   AFTER /api/loadbal/init if both are used, so the
             *   Collector can protect the load-balancer actors. */
            if (0 == strncmp(reqbuf, "GET /api/gc-actor/init",  22) ||
                0 == strncmp(reqbuf, "POST /api/gc-actor/init", 23))
            {
                extern void abcl_gc_actor_init(void);
                extern int  abcl_gc_actor_id(void);
                abcl_gc_actor_init();
                static char giresp[200];
                int blen = sprintf(giresp + 100,
                    "gc-actor initialized (obj=%d, heartbeat thread spawned)\r\n",
                    abcl_gc_actor_id());
                int hlen = sprintf(giresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(giresp + hlen, giresp + 100, blen);
                write(tcpdev, giresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/nqueens?n=N[&cols=COMMA_LIST]
             *   N-Queens benchmark route.  Submits one compute_nq
             *   task per first-column in `cols` (default: 0..N-1).
             *   Returns starting task_id; Mac polls /api/loadbal/task
             *   for each to collect partial counts, sums them.
             *   Used by tools/nqueens-bench.py for the Mac+Pi 3
             *   distributed benchmark. */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/nqueens",  24) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/nqueens", 25))
            {
                extern void abcl_loadbal_submit_nq(int, int, int);
                int n = 8;
                /* Default cols = 0..n-1 (whole problem).  If ?cols=
                 * given, parse comma-separated list. */
                char cols_str[256];
                cols_str[0] = '\0';
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url) {
                    const char *q = strchr(url, '?');
                    if (NULL != q) {
                        const char *p = q + 1;
                        while (*p && *p != ' ' && *p != '\r' && *p != '\n') {
                            const char *eq = p;
                            while (*eq && *eq != '=' && *eq != '&' &&
                                   *eq != ' ' && *eq != '\r' && *eq != '\n') eq++;
                            const char *amp = (*eq == '=') ? eq + 1 : eq;
                            while (*amp && *amp != '&' && *amp != ' ' &&
                                   *amp != '\r' && *amp != '\n') amp++;
                            int kl = eq - p;
                            if (kl == 1 && *p == 'n' && *eq == '=') {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                n = v;
                            } else if (kl == 4 && 0 == strncmp(p, "cols", 4)
                                       && *eq == '=') {
                                int cl = amp - (eq + 1);
                                if (cl > 255) cl = 255;
                                memcpy(cols_str, eq + 1, cl);
                                cols_str[cl] = '\0';
                            }
                            p = (*amp == '&') ? amp + 1 : amp;
                        }
                    }
                }
                if (n < 1) n = 1;
                if (n > 12) n = 12;     /* cap — n=12 = 14200 solutions */
                /* Static task-id allocator persists across requests so
                 * subsequent /nqueens calls don't reuse the same id
                 * range — task lookup table holds 64 entries. */
                static int g_nq_next_id = 1;
                int first_id = g_nq_next_id;
                int n_submitted = 0;
                if (cols_str[0] == '\0') {
                    /* Default: all columns 0..n-1 */
                    int c;
                    for (c = 0; c < n; c++) {
                        abcl_loadbal_submit_nq(n, c, g_nq_next_id++);
                        n_submitted++;
                    }
                } else {
                    /* Parse comma-separated */
                    const char *p = cols_str;
                    while (*p) {
                        int v = 0;
                        while (*p >= '0' && *p <= '9') {
                            v = v*10 + (*p - '0'); p++;
                        }
                        if (v >= 0 && v < n) {
                            abcl_loadbal_submit_nq(n, v, g_nq_next_id++);
                            n_submitted++;
                        }
                        while (*p && *p != ',') p++;
                        if (*p == ',') p++;
                    }
                }
                static char nqresp[300];
                int blen = sprintf(nqresp + 100,
                    "nqueens n=%d submitted=%d task_id_range=%d..%d\r\n"
                    "poll /api/loadbal/task?id=K for each, sum result fields\r\n",
                    n, n_submitted, first_id, g_nq_next_id - 1);
                int hlen = sprintf(nqresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(nqresp + hlen, nqresp + 100, blen);
                write(tcpdev, nqresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/restart?w=N — kill + respawn worker N.
             *   Uses a new obj_id (n_objects bumps).  MAX_OBJECTS=16 cap
             *   means ~9 restarts before the table fills.  Pending tasks
             *   in the old worker's mailbox are LOST and stay in
             *   submitted-not-completed forever (visible as the gap in
             *   /api/loadbal/stats).  Load counter resets to 0. */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/restart",  24) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/restart", 25))
            {
                extern void abcl_loadbal_restart_worker(int);
                int widx = -1;
                const char *q = strstr(reqbuf, "?w=");
                if (NULL != q) {
                    q += 3;
                    widx = 0;
                    while (*q >= '0' && *q <= '9') {
                        widx = widx * 10 + (*q - '0');
                        q++;
                    }
                }
                static char rresp[200];
                int blen;
                if (widx < 0 || widx >= 4) {
                    blen = sprintf(rresp + 100,
                        "usage: /api/loadbal/restart?w=0..3\r\n");
                } else {
                    abcl_loadbal_restart_worker(widx);
                    blen = sprintf(rresp + 100,
                        "restart_worker idx=%d queued — poll /api/actors "
                        "to see new obj_id\r\n", widx);
                }
                int hlen = sprintf(rresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(rresp + hlen, rresp + 100, blen);
                write(tcpdev, rresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/rate-limit?per_sec=N&capacity=M
             *   Configure the token-bucket rate limiter.  Both params
             *   optional — omit to leave current value.
             * /api/loadbal/rate-stats
             *   Current tokens + lifetime throttled count + task-timeout
             *   ms + lifetime auto-cancelled count. */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/rate-limit",  27) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/rate-limit", 28))
            {
                extern void abcl_loadbal_rate_set(int, int);
                int per_sec = -1, capacity = -1;
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url) {
                    const char *q = strchr(url, '?');
                    if (NULL != q) {
                        const char *p = q + 1;
                        while (*p && *p != ' ' && *p != '\r' && *p != '\n') {
                            const char *eq = p;
                            while (*eq && *eq != '=' && *eq != '&' &&
                                   *eq != ' ' && *eq != '\r' && *eq != '\n') eq++;
                            const char *amp = (*eq == '=') ? eq + 1 : eq;
                            while (*amp && *amp != '&' && *amp != ' ' &&
                                   *amp != '\r' && *amp != '\n') amp++;
                            int kl = eq - p;
                            if (kl == 7 && 0 == strncmp(p, "per_sec", 7)
                                && *eq == '=') {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                per_sec = v;
                            } else if (kl == 8 && 0 == strncmp(p, "capacity", 8)
                                       && *eq == '=') {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                capacity = v;
                            }
                            p = (*amp == '&') ? amp + 1 : amp;
                        }
                    }
                }
                abcl_loadbal_rate_set(per_sec, capacity);
                static char rresp2[200];
                int blen = sprintf(rresp2 + 100,
                    "rate-limit set per_sec=%d capacity=%d\r\n", per_sec, capacity);
                int hlen = sprintf(rresp2,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(rresp2 + hlen, rresp2 + 100, blen);
                write(tcpdev, rresp2, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            if (0 == strncmp(reqbuf, "GET /api/loadbal/rate-stats",  27) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/rate-stats", 28))
            {
                extern int abcl_loadbal_rate_stats(char *, int);
                static char rstats[400];
                int blen = abcl_loadbal_rate_stats(rstats + 100, 300);
                int hlen = sprintf(rstats,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(rstats + hlen, rstats + 100, blen);
                write(tcpdev, rstats, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/timeout?ms=N — set per-task pending deadline.
             *   PENDING tasks older than this get auto-CANCELLED by the
             *   Collector's tick.  0 = disable (no expiry).  Default
             *   30 000 ms (30 s). */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/timeout",  24) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/timeout", 25))
            {
                extern void abcl_loadbal_timeout_set(int);
                int ms = -1;
                const char *q = strstr(reqbuf, "?ms=");
                if (NULL != q) {
                    q += 4;
                    ms = 0;
                    while (*q >= '0' && *q <= '9') {
                        ms = ms * 10 + (*q - '0');
                        q++;
                    }
                }
                static char tresp3[200];
                int blen;
                if (ms < 0) {
                    blen = sprintf(tresp3 + 100,
                        "usage: /api/loadbal/timeout?ms=N (N >= 0)\r\n");
                } else {
                    abcl_loadbal_timeout_set(ms);
                    blen = sprintf(tresp3 + 100,
                        "task timeout set to %d ms (0 = disabled)\r\n", ms);
                }
                int hlen = sprintf(tresp3,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(tresp3 + hlen, tresp3 + 100, blen);
                write(tcpdev, tresp3, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/gc-actor/stats — periodic actor GC stats.
             * /api/gc-actor/configure?period=N&threshold=M
             * /api/gc-actor/enable?on=0|1
             * /api/gc-actor/sweep_now — force immediate sweep
             *
             * Differs from the existing /gc route (which performs a
             * one-shot manual sweep from the webactor thread): this
             * talks to the Collector ACTOR, so the sweep runs in its
             * own thread at the Collector priority (26, above the
             * load-balancer dispatcher) and the configuration is
             * persistent across HTTP requests. */
            if (0 == strncmp(reqbuf, "GET /api/gc-actor/stats",  23) ||
                0 == strncmp(reqbuf, "POST /api/gc-actor/stats", 24))
            {
                extern int abcl_gc_actor_stats(char *, int);
                static char gresp[500];
                int blen = abcl_gc_actor_stats(gresp + 100, 400);
                int hlen = sprintf(gresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(gresp + hlen, gresp + 100, blen);
                write(tcpdev, gresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            if (0 == strncmp(reqbuf, "GET /api/gc-actor/configure",  27) ||
                0 == strncmp(reqbuf, "POST /api/gc-actor/configure", 28))
            {
                extern void abcl_gc_actor_configure(int, int);
                int period = -1, threshold = -1;
                const char *url = strchr(reqbuf, ' ');
                if (NULL != url)
                {
                    const char *q = strchr(url, '?');
                    if (NULL != q)
                    {
                        const char *p = q + 1;
                        while (*p && *p != ' ' && *p != '\r' && *p != '\n')
                        {
                            const char *eq = p;
                            while (*eq && *eq != '=' && *eq != '&' &&
                                   *eq != ' ' && *eq != '\r' && *eq != '\n') eq++;
                            const char *amp = (*eq == '=') ? eq + 1 : eq;
                            while (*amp && *amp != '&' && *amp != ' ' &&
                                   *amp != '\r' && *amp != '\n') amp++;
                            int klen = eq - p;
                            if (klen == 6 && 0 == strncmp(p, "period", 6)
                                && *eq == '=')
                            {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                period = v;
                            }
                            else if (klen == 9 && 0 == strncmp(p, "threshold", 9)
                                     && *eq == '=')
                            {
                                int v = 0; const char *d = eq + 1;
                                while (d < amp && *d >= '0' && *d <= '9')
                                { v = v*10 + (*d - '0'); d++; }
                                threshold = v;
                            }
                            p = (*amp == '&') ? amp + 1 : amp;
                        }
                    }
                }
                abcl_gc_actor_configure(period, threshold);
                static char cresp3[200];
                int blen = sprintf(cresp3 + 100,
                    "gc-actor configure period=%d threshold=%d\r\n",
                    period, threshold);
                int hlen = sprintf(cresp3,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(cresp3 + hlen, cresp3 + 100, blen);
                write(tcpdev, cresp3, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            if (0 == strncmp(reqbuf, "GET /api/gc-actor/enable",  24) ||
                0 == strncmp(reqbuf, "POST /api/gc-actor/enable", 25))
            {
                extern void abcl_gc_actor_enable(int);
                int on = 1;
                const char *q = strstr(reqbuf, "?on=");
                if (NULL != q) on = (q[4] != '0');
                abcl_gc_actor_enable(on);
                static char eresp[150];
                int blen = sprintf(eresp + 100,
                    "gc-actor %s\r\n", on ? "ENABLED" : "PAUSED");
                int hlen = sprintf(eresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(eresp + hlen, eresp + 100, blen);
                write(tcpdev, eresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            if (0 == strncmp(reqbuf, "GET /api/gc-actor/sweep_now",  27) ||
                0 == strncmp(reqbuf, "POST /api/gc-actor/sweep_now", 28))
            {
                extern void abcl_gc_actor_sweep_now(void);
                abcl_gc_actor_sweep_now();
                static const char ok2[] =
                    "HTTP/1.0 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 26\r\n"
                    "\r\n"
                    "gc-actor sweep_now queued\r\n";
                write(tcpdev, (void *)ok2, sizeof(ok2) - 1);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/cancel?id=N — mark task as cancelled.
             *   Sleep-mode workers honor it on next 50 ms chunk boundary;
             *   JIT-mode workers honor it only BEFORE the JIT call (the
             *   compile + execute itself is uninterruptible).
             *   A cancelled task still reports back via done() so the
             *   load counter stays consistent.  Task table state moves
             *   PENDING -> CANCELLED -> (done message keeps it CANCELLED,
             *   not DONE — see Dispatcher_done). */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/cancel",  23) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/cancel", 24))
            {
                extern void abcl_loadbal_cancel(int);
                int task_id = 0;
                const char *q = strstr(reqbuf, "?id=");
                if (NULL != q) {
                    q += 4;
                    while (*q >= '0' && *q <= '9') {
                        task_id = task_id * 10 + (*q - '0');
                        q++;
                    }
                }
                static char cresp2[200];
                int blen;
                if (task_id <= 0) {
                    blen = sprintf(cresp2 + 100,
                        "usage: /api/loadbal/cancel?id=N (N > 0)\r\n");
                } else {
                    abcl_loadbal_cancel(task_id);
                    blen = sprintf(cresp2 + 100,
                        "cancel requested for task=%d (poll /task?id=%d for state)\r\n",
                        task_id, task_id);
                }
                int hlen = sprintf(cresp2,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(cresp2 + hlen, cresp2 + 100, blen);
                write(tcpdev, cresp2, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/task?id=N — query task by id (last 64 retained).
             *   Returns submit_ms / done_ms / elapsed_ms / state / result.
             *   Caller submits via /submit or /jit (which prints the id
             *   range), then polls /task?id= for completion. */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/task",  21) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/task", 22))
            {
                extern int abcl_loadbal_task_info(int, char *, int);
                int task_id = 0;
                const char *q = strstr(reqbuf, "?id=");
                if (NULL != q) {
                    q += 4;
                    while (*q >= '0' && *q <= '9') {
                        task_id = task_id * 10 + (*q - '0');
                        q++;
                    }
                }
                static char tresp2[500];
                int blen;
                if (task_id <= 0) {
                    blen = sprintf(tresp2 + 100,
                        "usage: /api/loadbal/task?id=N (N > 0)\r\n");
                } else {
                    blen = abcl_loadbal_task_info(task_id, tresp2 + 100, 400);
                }
                int hlen = sprintf(tresp2,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(tresp2 + hlen, tresp2 + 100, blen);
                write(tcpdev, tresp2, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /api/loadbal/stats: dispatcher + per-worker counters.
             * Used by Mac scripts to verify even distribution. */
            if (0 == strncmp(reqbuf, "GET /api/loadbal/stats", 22) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/stats", 23))
            {
                extern int abcl_loadbal_stats(char *, int);
                static char sresp[1024];
                int blen = abcl_loadbal_stats(sresp + 200, 800);
                int hlen = sprintf(sresp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                memcpy(sresp + hlen, sresp + 200, blen);
                write(tcpdev, sresp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /sd-test: read LBA 0 (MBR) and report first 16 bytes hex.
             * Sanity check that the in-kernel SD driver can read the same
             * card the firmware booted us from. */
            if (0 == strncmp(reqbuf, "GET /sd-test", 12) ||
                0 == strncmp(reqbuf, "POST /sd-test", 13))
            {
                static unsigned char sd_buf[SD_BLOCK_SIZE];
                static char sd_resp[600];
                int rc = sd_read_block(0, sd_buf);
                int blen;
                if (rc != 0)
                {
                    blen = sprintf(sd_resp + 100,
                                   "sd_read_block(0) FAILED rc=%d\r\n", rc);
                }
                else
                {
                    blen = sprintf(sd_resp + 100,
                                   "sd_read_block(0) OK\r\n"
                                   "first 16 bytes: %02x %02x %02x %02x "
                                   "%02x %02x %02x %02x %02x %02x %02x %02x "
                                   "%02x %02x %02x %02x\r\n"
                                   "MBR signature [510-511]: %02x %02x (want 55 AA)\r\n",
                                   sd_buf[0], sd_buf[1], sd_buf[2], sd_buf[3],
                                   sd_buf[4], sd_buf[5], sd_buf[6], sd_buf[7],
                                   sd_buf[8], sd_buf[9], sd_buf[10], sd_buf[11],
                                   sd_buf[12], sd_buf[13], sd_buf[14], sd_buf[15],
                                   sd_buf[510], sd_buf[511]);
                }
                int hlen = sprintf(sd_resp,
                                   "HTTP/1.0 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n", blen);
                /* response = hdr + body; body lives at sd_resp+100; rewrite hdr in place */
                memcpy(sd_resp + hlen, sd_resp + 100, blen);
                write(tcpdev, sd_resp, hlen + blen);
                close(tcpdev);
                web_cur_tcpdev = -1;
                continue;
            }
            /* /reboot: BCM2837 watchdog reset (no SD swap needed for soft
             * recovery).  Detect either GET or POST.  Responds before the
             * reset takes effect so the client sees the 200; the SoC then
             * resets and re-loads kernel from SD a moment later. */
            if (0 == strncmp(reqbuf, "GET /reboot", 11) ||
                0 == strncmp(reqbuf, "POST /reboot", 12))
            {
                static const char reboot_resp[] =
                    "HTTP/1.0 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 11\r\n"
                    "\r\n"
                    "rebooting\r\n";
                kprintf("[webactor] /reboot — triggering watchdog reset\r\n");
                write(tcpdev, (void *)reboot_resp, sizeof(reboot_resp) - 1);
                close(tcpdev);
                /* 10 ms is enough for the TCP write to drain to the wire;
                 * once watchdogset() fires we never return. */
                sleep(10);
                watchdogset(1);
                /* defensive: should not reach here */
                while (1) { }
            }
            extract_message(reqbuf, n, msg, sizeof(msg));
            if (msg[0] != '\0')
            {
                kprintf("[webactor] HTTP request -> actor %d: \"%s\"\r\n",
                        web_receiver_id, msg);
                abcl_web_deliver(web_receiver_id, "recv", msg);
            }
            write(tcpdev, (void *)resp, sizeof(resp) - 1);
        }
        close(tcpdev);
        web_cur_tcpdev = -1;
    }
    return OK;
}

/* Stop a running server: kill the server thread and free the TCP device it was
 * parked on, so the port is released for a clean restart.  Safe to call when no
 * server is running. */
void webactor_stop(void)
{
    if (BADTID != web_server_tid)
    {
        kill(web_server_tid);
        web_server_tid = BADTID;
    }
    if (web_cur_tcpdev >= 0)
    {
        close(web_cur_tcpdev);
        web_cur_tcpdev = -1;
    }
}

/* Register the AIPL WebReceiver actor and spawn the HTTP server thread.  If a
 * server is already running it is stopped first, so calling this again acts as
 * a restart.  Returns the AIPL actor id, or SYSERR. */
int webactor_start(void)
{
    tid_typ tid;

    webactor_stop();                /* drop any previous (possibly stuck) server */
    web_receiver_id = abcl_web_init();
    tid = create((void *)webactor_server, 8192, INITPRIO, "webactor", 0);
    if (SYSERR == tid)
    {
        return SYSERR;
    }
    web_server_tid = tid;
    ready(tid, RESCHED_NO);
    return web_receiver_id;
}

/* Boot auto-start: wait for ETH0 to come up (it is opened asynchronously by
 * main.c's eth_open_all thread), bring the interface up with a static IP, then
 * start the web server + WebReceiver actor.  Spawned from main.c so the whole
 * Mac-actor -> AIPL-actor path is live right after boot, with no manual
 * `netup` / `webactor` commands. */
thread webactor_autostart(void)
{
    struct netaddr ip, mask, gw;
    int i;

    /* Wait (up to ~30 s) for eth_open_all to finish opening ETH0. */
    for (i = 0; i < 60; i++)
    {
        if (ethertab[0].state == ETH_STATE_UP)
        {
            break;
        }
        sleep(500);
    }
    if (ethertab[0].state != ETH_STATE_UP)
    {
        kprintf("[webactor] autostart: ETH0 never came up\r\n");
        return SYSERR;
    }

    if (SYSERR == dot2ipv4(WEBACTOR_IP, &ip) ||
        SYSERR == dot2ipv4(WEBACTOR_MASK, &mask) ||
        SYSERR == dot2ipv4(WEBACTOR_GW, &gw))
    {
        kprintf("[webactor] autostart: bad static IP config\r\n");
        return SYSERR;
    }

    if (SYSERR == netUp((ethertab[0].dev)->num, &ip, &mask, &gw))
    {
        kprintf("[webactor] autostart: netUp failed\r\n");
        return SYSERR;
    }
    kprintf("[webactor] autostart: netUp %s/%s gw %s\r\n",
            WEBACTOR_IP, WEBACTOR_MASK, WEBACTOR_GW);

    /* "Dynamic" distributed dining: Xinu boots with ZERO dining actors.
     * We only initialise the AIPL runtime (mutexes) and start the ethernet
     * AIPL-RPC server.  At runtime the Mac ships the Fork/Philosopher source
     * (LOAD) + dynamically compiles it (COMPILE), then SPAWNs the 5 Forks
     * (ids 0..4) and 2 Xinu Philosophers (ids 5..6) over the RPC link.  No
     * aipl_main and no webactor_start here, so the first SPAWN'd Fork is
     * deterministically actor id 0. */
    {
        extern void abcl_rt_init(void);
        extern void abcl_rpc_tcp_start(int port);
        abcl_rt_init();             /* runtime ready, but no actors yet */
        abcl_rpc_tcp_start(5555);   /* Mac <-> Xinu RPC over ethernet */
    }

    /* Also start the simple HTTP server on 8080 so /reboot, /sd-test, and
     * other Mac-side recovery tools are available without needing the AIPL
     * RPC protocol.  Without this, port 8080 stays closed and the only
     * way to reach Pi 3 is the serial console. */
    {
        int rid = webactor_start();
        if (rid < 0)
        {
            kprintf("[webactor] autostart: webactor_start failed\r\n");
        }
        else
        {
            kprintf("[webactor] autostart: HTTP server up on port %d\r\n",
                    WEBACTOR_PORT);
        }
    }
    /* Load-balancer (Dispatcher + 4 Workers) and Collector (actor GC)
     * are NOT auto-started anymore — they cost 6 idle actors plus a
     * heartbeat thread that most callers don't need.  Spin them up
     * explicitly via:
     *   POST /api/loadbal/init   — start Dispatcher + 4 Workers
     *   POST /api/gc-actor/init  — start Collector + heartbeat
     * Both inits are idempotent; subsequent calls just return.  Cold
     * boot shows only WebReceiver in /api/actors. */
    return OK;
}
