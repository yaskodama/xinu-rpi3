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
#include "sd_block.h"  /* sd_init / sd_read_block for /sd-test route */

/* AIPL bridge (apps/abcl_program.c) */
extern int  abcl_web_init(void);
extern void abcl_web_deliver(int receiver, const char *method, const char *str);

#define WEBACTOR_PORT 8080
#define WEB_BUFSZ     1024

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
    char   reqbuf[WEB_BUFSZ];
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
         * 1023 bytes never arrive.  Read byte-by-byte until the request is
         * complete: the blank line after the headers, plus any Content-Length
         * body. */
        n = 0;
        {
            int header_end = -1, content_len = 0, have_cl = 0;
            while (n < WEB_BUFSZ - 1)
            {
                if (read(tcpdev, reqbuf + n, 1) <= 0)
                {
                    break;                  /* peer closed / error */
                }
                n++;
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
                    }
                }
                if (header_end >= 0)
                {
                    if (!have_cl || content_len <= 0)
                    {
                        break;              /* no body (e.g. GET) */
                    }
                    if (n >= header_end + content_len)
                    {
                        break;              /* full body received */
                    }
                }
            }
        }
        if (n > 0)
        {
            reqbuf[n] = '\0';
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
    return OK;
}
