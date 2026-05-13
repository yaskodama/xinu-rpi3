/**
 * @file xsh_cd.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_cd(int nargs, char *args[])
{
    const char *path = (nargs >= 2) ? args[1] : "/";
    if (OK != xfsChdir(path))
    {
        fprintf(stderr, "cd: %s: No such directory\n", path);
        return 1;
    }
    return 0;
}
