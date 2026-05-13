/**
 * @file xsh_umount.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_umount(int nargs, char *args[])
{
    if (nargs != 2)
    {
        fprintf(stderr, "Usage: umount MOUNTPOINT\n");
        return 1;
    }
    if (OK != xfsUmount(args[1]))
    {
        fprintf(stderr, "umount: failed\n");
        return 1;
    }
    return 0;
}
