/**
 * @file xsh_write.c
 *
 * Convenience: write the joined remaining arguments to FILE (truncating).
 *   write FILE TEXT...
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <xfs.h>

shellcmd xsh_write(int nargs, char *args[])
{
    int fd, i, n, w;
    if (nargs < 3)
    {
        fprintf(stderr, "Usage: write FILE TEXT...\n");
        return 1;
    }
    fd = xfsOpen(args[1], XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
    if (fd < 0)
    {
        fprintf(stderr, "write: %s: cannot create\n", args[1]);
        return 1;
    }
    for (i = 2; i < nargs; i++)
    {
        n = strlen(args[i]);
        w = xfsWrite(fd, args[i], n);
        if (w != n) { fprintf(stderr, "write: short write\n"); break; }
        if (i + 1 < nargs)
        {
            if (1 != xfsWrite(fd, " ", 1)) break;
        }
    }
    xfsWrite(fd, "\n", 1);
    xfsClose(fd);
    return 0;
}
