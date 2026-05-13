/**
 * @file xsh_ls.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <xfs.h>

shellcmd xsh_ls(int nargs, char *args[])
{
    const char *path;
    struct xdirent ent;
    uint32_t idx = 0, next;
    char namebuf[XFS_MAX_NAME + 1];
    int long_form = 0, i;

    path = ".";
    for (i = 1; i < nargs; i++)
    {
        if (0 == strcmp(args[i], "-l")) long_form = 1;
        else                            path = args[i];
    }

    while (OK == xfsReaddir(path, idx, &ent, &next))
    {
        idx = next;
        memcpy(namebuf, ent.name, ent.name_len);
        namebuf[ent.name_len] = 0;

        if (long_form)
        {
            char fullpath[XFS_PATH_MAX];
            struct xinode in;
            int n;
            n = strlen(path);
            if (n + 1 + ent.name_len + 1 < (int)sizeof(fullpath))
            {
                strcpy(fullpath, path);
                if (n > 0 && fullpath[n-1] != '/') { fullpath[n++] = '/'; }
                strcpy(fullpath + n, namebuf);
            }
            else
            {
                strcpy(fullpath, namebuf);
            }
            if (OK == xfsStat(fullpath, &in, NULL))
            {
                printf("%c %8u %s%s\n",
                       (ent.type == XFS_T_DIR) ? 'd' : '-',
                       in.size, namebuf,
                       (ent.type == XFS_T_DIR) ? "/" : "");
            }
        }
        else
        {
            printf("%s%s\n", namebuf,
                   (ent.type == XFS_T_DIR) ? "/" : "");
        }
    }
    return 0;
}
