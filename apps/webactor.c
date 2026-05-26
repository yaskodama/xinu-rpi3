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
#include <tcp.h>
#include <string.h>
#include <stdio.h>
#include <clock.h>

/* AIPL bridge (apps/abcl_program.c) */
extern int  abcl_web_init(void);
extern void abcl_web_deliver(int receiver, const char *method, const char *str);

#define WEBACTOR_PORT 8080
#define WEB_BUFSZ     1024

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
