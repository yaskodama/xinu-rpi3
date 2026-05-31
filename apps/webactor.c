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
 * doesn't inflate the on-disk binary, just runtime footprint. */
static unsigned char  upload_slot_data[524288];
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
            if (0 == strncmp(reqbuf, "GET /api/loadbal/submit", 23) ||
                0 == strncmp(reqbuf, "POST /api/loadbal/submit", 24))
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
    /* Spin up the load-balancer (1 Dispatcher + 4 Workers) so the
     * /api/loadbal/* routes work without needing aipl_main. */
    {
        extern void abcl_loadbal_init(void);
        abcl_loadbal_init();
    }
    return OK;
}
