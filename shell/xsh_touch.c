/**
 * @file xsh_touch.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_touch(int nargs, char *args[])
{
    int i, rc = 0;
    if (nargs < 2)
    {
        fprintf(stderr, "Usage: touch FILE...\n");
        return 1;
    }
    for (i = 1; i < nargs; i++)
    {
        if (OK != xfsTouch(args[i]))
        {
            fprintf(stderr, "touch: %s\n", args[i]);
            rc = 1;
        }
    }
    return rc;
}
