/**
 * @file xsh_cat.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_cat(int nargs, char *args[])
{
    char buf[256];
    int  i, fd, n, j;

    if (nargs < 2)
    {
        fprintf(stderr, "Usage: cat FILE...\n");
        return 1;
    }
    for (i = 1; i < nargs; i++)
    {
        fd = xfsOpen(args[i], XFS_O_RDONLY);
        if (fd < 0)
        {
            fprintf(stderr, "cat: %s: cannot open\n", args[i]);
            continue;
        }
        while ((n = xfsRead(fd, buf, sizeof(buf))) > 0)
        {
            for (j = 0; j < n; j++) putchar(buf[j]);
        }
        xfsClose(fd);
    }
    return 0;
}
