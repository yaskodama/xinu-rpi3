/**
 * @file xsh_cp.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_cp(int nargs, char *args[])
{
    char buf[256];
    int  in, out, n, w;

    if (nargs != 3)
    {
        fprintf(stderr, "Usage: cp SRC DST\n");
        return 1;
    }
    in = xfsOpen(args[1], XFS_O_RDONLY);
    if (in < 0)
    {
        fprintf(stderr, "cp: %s: cannot open\n", args[1]);
        return 1;
    }
    out = xfsOpen(args[2], XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
    if (out < 0)
    {
        fprintf(stderr, "cp: %s: cannot create\n", args[2]);
        xfsClose(in);
        return 1;
    }
    while ((n = xfsRead(in, buf, sizeof(buf))) > 0)
    {
        w = xfsWrite(out, buf, n);
        if (w != n) { fprintf(stderr, "cp: short write\n"); break; }
    }
    xfsClose(in);
    xfsClose(out);
    return 0;
}
