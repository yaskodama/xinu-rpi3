/**
 * @file xsh_mv.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_mv(int nargs, char *args[])
{
    if (nargs != 3)
    {
        fprintf(stderr, "Usage: mv SRC DST\n");
        return 1;
    }
    if (OK != xfsRename(args[1], args[2]))
    {
        fprintf(stderr, "mv: failed\n");
        return 1;
    }
    return 0;
}
