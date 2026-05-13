/**
 * @file xsh_mkdir.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_mkdir(int nargs, char *args[])
{
    int i, rc = 0;
    if (nargs < 2)
    {
        fprintf(stderr, "Usage: mkdir DIR...\n");
        return 1;
    }
    for (i = 1; i < nargs; i++)
    {
        if (OK != xfsMkdir(args[i]))
        {
            fprintf(stderr, "mkdir: cannot create %s\n", args[i]);
            rc = 1;
        }
    }
    return rc;
}
