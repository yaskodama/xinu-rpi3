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

extern int webactor_start(void);

shellcmd xsh_webactor(int nargs, char *args[])
{
    int id;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tStart an HTTP server on port 8080 that delivers each\n");
        printf("\tincoming message to a WebReceiver AIPL actor.\n");
        printf("\tRun `netup ETH0 <ip> <mask> <gw>` first.\n");
        printf("\tFrom the Mac:  curl -d 'hello' http://<pi-ip>:8080/\n");
        return 0;
    }

    id = webactor_start();
    if (id < 0)
    {
        fprintf(stderr, "%s: failed to start (is ETH0 up? run netup)\n",
                args[0]);
        return 1;
    }
    printf("webactor: HTTP :8080 -> AIPL WebReceiver actor %d\n", id);
    return 0;
}
