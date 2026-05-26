/**
 * @file xsh_webactor.c
 *
 * `webactor` shell command: start an HTTP server that bridges incoming web
 * messages to a WebReceiver AIPL actor.  Run `netup ETH0 ...` first.
 *
 * From the Mac:   curl -d 'hello from mac' http://<pi-ip>:8080/
 */

#include <kernel.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <shell.h>

extern int  webactor_start(void);
extern void webactor_stop(void);

shellcmd xsh_webactor(int nargs, char *args[])
{
    int id;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [-k]\n\n", args[0]);
        printf("Description:\n");
        printf("\tStart (or restart) an HTTP server on port 8080 that\n");
        printf("\tdelivers each incoming message to a WebReceiver AIPL\n");
        printf("\tactor.  Run `netup ETH0 <ip> <mask> <gw>` first (or it\n");
        printf("\tauto-starts at boot).  Running it again restarts the\n");
        printf("\tserver; -k stops it.\n");
        printf("\tFrom the Mac:  curl -d 'hello' http://<pi-ip>:8080/\n");
        return 0;
    }

    /* `webactor -k` (or stop): kill the server thread and free its port. */
    if (nargs == 2 && (strcmp(args[1], "-k") == 0 ||
                       strcmp(args[1], "stop") == 0))
    {
        webactor_stop();
        printf("webactor: stopped\n");
        return 0;
    }

    /* No args (or anything else): (re)start.  webactor_start() stops any
     * previous server first, so this doubles as a restart. */
    id = webactor_start();
    if (id < 0)
    {
        fprintf(stderr, "%s: failed to start (is ETH0 up? run netup)\n",
                args[0]);
        return 1;
    }
    printf("webactor: HTTP :8080 -> AIPL WebReceiver actor %d (started)\n", id);
    return 0;
}
