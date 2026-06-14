/** 
 * @file     xsh_help.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>

/**
 * @ingroup shell
 *
 * Shell command (help) provides a list of commands recognized by the
 * shell, or displays help information for a particular command.
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_help(int nargs, char *args[])
{
    uchar i;
    char *command_args[2];      /* temporary storage for [command] --help */

    /* Output help, if '--help' argument was supplied */
    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage:\n");
        printf("\t%s [command]\n", args[0]);
        printf("Description:\n");
        printf("\tProvides a list of commands for the shell.\n");
        printf("\tIf command is provided, help information will\n");
        printf("\tbe provided for the specified command; equivalent\n");
        printf("\tto entering 'command --help' into the shell.\n");
        printf("Options:\n");
        printf("\tcommand\tcommand name to display for which to\n");
        printf("\t\tdisplay help information\n");
        printf("\t--help\tdisplay this help and exit\n");
        return 0;
    }

    /* Check for correct number of arguments */
    if (nargs > 2)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information.\n",
                args[0]);
        return 1;
    }

    /* Output help for specific command, if 'command' argument was supplied */
    if (nargs == 2)
    {
        for (i = 0; i < ncommand; i++)
        {
            if (strcmp(args[1], commandtab[i].name) == 0)
            {
                command_args[0] = args[1];
                command_args[1] = "--help";
                (*commandtab[i].procedure) (2, command_args);
                return 0;
            }
        }
        printf("%s: no help topics match '%s'.\n", args[0], args[1]);
        printf("  Try '%s --help' for more information.\n", args[0]);
        return 1;
    }

    /* Output command list in columns, spread across the window width. */
    printf("Shell Commands:\n");
    {
        int maxlen = 0, colw, cols, col = 0, j, pad;
        const int width = 54;          /* usable chars in the shell window */
        for (i = 0; i < ncommand; i++)
        {
            int l = (int)strlen(commandtab[i].name);
            if (l > maxlen) maxlen = l;
        }
        colw = maxlen + 2;
        cols = width / colw;
        if (cols < 1) cols = 1;
        for (i = 0; i < ncommand; i++)
        {
            printf("%s", commandtab[i].name);
            if (++col >= cols)
            {
                printf("\n");
                col = 0;
            }
            else
            {
                pad = colw - (int)strlen(commandtab[i].name);
                for (j = 0; j < pad; j++) printf(" ");
            }
        }
        if (col != 0) printf("\n");
    }

    return 0;
}
