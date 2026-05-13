/**
 * @file xsh_mkfs.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_mkfs(int nargs, char *args[])
{
    const char *vol = "xfs";
    if (nargs < 2)
    {
        fprintf(stderr, "Usage: mkfs DEVICE [VOLNAME]\n");
        return 1;
    }
    if (nargs >= 3) vol = args[2];
    if (OK != xfsMkfs(args[1], vol))
    {
        fprintf(stderr, "mkfs: failed\n");
        return 1;
    }
    printf("formatted %s\n", args[1]);
    return 0;
}
