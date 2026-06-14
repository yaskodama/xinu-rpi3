/**
 * @file xsh_cat.c
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <xfs.h>
#include <fat.h>

/* cat callback: write a chunk of a /microsd file straight to stdout. */
static int cat_emit(const unsigned char *buf, int len, void *ctx)
{
    int k;
    (void)ctx;
    for (k = 0; k < len; k++) putchar(buf[k]);
    return 0;
}

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
        /* /microsd/<name> -> read the file off the SD card's FAT32 root. */
        if (0 == strncmp(args[i], "/microsd/", 9))
        {
            const char *name = args[i] + 9;
            struct fat_dirent e;
            if (fat_mount() != 0)
            {
                fprintf(stderr, "cat: /microsd: cannot read SD card\n");
                continue;
            }
            if (0 != fat_find_root(name, &e) || e.is_dir)
            {
                fprintf(stderr, "cat: %s: no such file\n", args[i]);
                continue;
            }
            fat_read_file(e.cluster, e.size, cat_emit, NULL);
            continue;
        }

        /* /sd/<name> -> read off a USB-attached SD-card reader's FAT32 root. */
        if (0 == strncmp(args[i], "/sd/", 4))
        {
            extern int usbmsc_fat_select(void);
            const char *name = args[i] + 4;
            struct fat_dirent e;
            if (usbmsc_fat_select() != 0)
            {
                printf("cat: /sd: USB card not ready\n");
                continue;
            }
            if (fat_mount() != 0)
            {
                printf("cat: /sd: mount failed\n");
                fat_set_blkdev(0, 0);
                continue;
            }
            if (0 != fat_find_root(name, &e) || e.is_dir)
            {
                printf("cat: %s: no such file\n", args[i]);
                fat_set_blkdev(0, 0);
                continue;
            }
            fat_read_file(e.cluster, e.size, cat_emit, NULL);
            putchar('\n');
            fat_set_blkdev(0, 0);
            continue;
        }

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
