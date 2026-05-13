/**
 * @file xsh_rm.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_rm(int nargs, char *args[])
{
    int i, rc = 0;
    if (nargs < 2)
    {
        fprintf(stderr, "Usage: rm FILE...\n");
        return 1;
    }
    for (i = 1; i < nargs; i++)
    {
        if (OK != xfsUnlink(args[i]))
        {
            fprintf(stderr, "rm: cannot remove %s\n", args[i]);
            rc = 1;
        }
    }
    return rc;
}
