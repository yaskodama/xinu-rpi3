/**
 * @file xsh_ls.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <xfs.h>
#include <fat.h>

/* `ls /microsd` callback: print one FAT32 directory entry off the SD card. */
static int ls_microsd_cb(const struct fat_dirent *e, void *ctx)
{
    int long_form = *(int *)ctx;
    if (long_form)
        printf("%c %8u %s%s\n", e->is_dir ? 'd' : '-',
               (unsigned)e->size, e->name, e->is_dir ? "/" : "");
    else
        printf("%s%s\n", e->name, e->is_dir ? "/" : "");
    return 0;
}

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

    /* /microsd -> the SD card's FAT32 boot partition (read-only listing). */
    if (0 == strncmp(path, "/microsd", 8))
    {
        if (fat_mount() != 0)
        {
            printf("ls: /microsd: cannot read SD card (no FAT32 partition)\n");
            return 1;
        }
        if (fat_list_root(ls_microsd_cb, &long_form) != 0)
        {
            printf("ls: /microsd: SD read error\n");
            return 1;
        }
        return 0;
    }

    /* /sd -> a USB-attached SD-card reader (USB Mass-Storage / BBB+SCSI).
     * usbmsc_fat_list() prints its own diagnostics (present / capacity /
     * raw-block-0 / mount) so failures are visible in the shell window. */
    if (0 == strncmp(path, "/sd", 3))
    {
        extern int usbmsc_fat_list(int long_form);   /* apps/usbmsc.c */
        return (usbmsc_fat_list(long_form) == 0) ? 0 : 1;
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
