/**
 * @file xsh_history.c
 * "history" shell command — print the command history (oldest first).
 */
/* Embedded Xinu, Copyright (C) 2024.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <shell.h>
#include <shell_readline.h>

/* Set by shell() to point at the active shell's circular history buffer. */
extern struct shell_history *g_shell_hist;

/**
 * @ingroup shell
 *
 * Shell command (history) — list previously entered commands.
 */
shellcmd xsh_history(int nargs, char *args[])
{
    struct shell_history *h;
    int n, i;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tList the command history, oldest first.\n");
        printf("Options:\n");
        printf("\t--help\t display this help and exit\n");
        return 0;
    }

    h = g_shell_hist;
    if (NULL == h || shell_history_size(h) == 0)
    {
        printf("(no history)\n");
        return 0;
    }

    n = shell_history_size(h);
    /* shell_history_at(h, k) counts k from the newest (0 = most recent).
     * Walk from the oldest (k = n-1) to the newest (k = 0). */
    for (i = n - 1; i >= 0; i--)
    {
        const char *line = shell_history_at(h, i);
        if (NULL != line)
        {
            printf("%4d  %s\n", n - i, line);
        }
    }
    return 0;
}
