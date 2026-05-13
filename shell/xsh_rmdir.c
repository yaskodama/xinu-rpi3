/**
 * @file xsh_rmdir.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_rmdir(int nargs, char *args[])
{
    int i, rc = 0;
    if (nargs < 2)
    {
        fprintf(stderr, "Usage: rmdir DIR...\n");
        return 1;
    }
    for (i = 1; i < nargs; i++)
    {
        if (OK != xfsRmdir(args[i]))
        {
            fprintf(stderr, "rmdir: failed to remove %s\n", args[i]);
            rc = 1;
        }
    }
    return rc;
}
