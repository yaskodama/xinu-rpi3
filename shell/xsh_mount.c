/**
 * @file xsh_mount.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_mount(int nargs, char *args[])
{
    if (nargs != 3)
    {
        fprintf(stderr, "Usage: mount DEVICE MOUNTPOINT\n");
        return 1;
    }
    if (OK != xfsMount(args[1], args[2]))
    {
        fprintf(stderr, "mount: failed\n");
        return 1;
    }
    return 0;
}
