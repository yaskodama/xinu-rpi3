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

/* AIPL bridge (apps/abcl_program.c) */
extern int  abcl_web_init(void);
extern void abcl_web_deliver(int receiver, const char *method, const char *str);

#define WEBACTOR_PORT 8080
#define WEB_BUFSZ     1024

/* Static network config used by the boot auto-start (webactor_autostart). */
#define WEBACTOR_IP   "192.168.3.50"
#define WEBACTOR_MASK "255.255.255.0"
#define WEBACTOR_GW   "192.168.3.1"

static int web_receiver_id = -1;

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
        /* TCP_PASSIVE blocks until a client connects */
        if (open(tcpdev, host, NULL, WEBACTOR_PORT, NULL, TCP_PASSIVE) < 0)
        {
            close(tcpdev);
            sleep(100);
            continue;
        }

        n = read(tcpdev, reqbuf, WEB_BUFSZ - 1);
        if (n > 0)
        {
            reqbuf[n] = '\0';
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
    }
    return OK;
}

/* Register the AIPL WebReceiver actor and spawn the HTTP server thread.
 * Returns the AIPL actor id, or SYSERR. */
int webactor_start(void)
{
    tid_typ tid;

    web_receiver_id = abcl_web_init();
    tid = create((void *)webactor_server, 8192, INITPRIO, "webactor", 0);
    if (SYSERR == tid)
    {
        return SYSERR;
    }
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

    webactor_start();
    return OK;
}
