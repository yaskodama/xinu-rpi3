/**
 * @file xsh_pwd.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <xfs.h>

shellcmd xsh_pwd(int nargs, char *args[])
{
    char buf[XFS_PATH_MAX];
    (void)nargs; (void)args;
    if (OK != xfsGetcwd(buf, sizeof(buf)))
    {
        fprintf(stderr, "pwd: error\n");
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}
