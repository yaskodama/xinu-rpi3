/**
 * @file xsh_cp.c
 *
 * `cp SRC DST` — copy a file.  Adds FAT32 support for the on-board SD
 * (/microsd) and a USB-attached SD-card reader (/sd):
 *     cp /microsd/boot.txt /sd/boot.txt
 *     cp /sd/log.txt /microsd/log.txt
 *     cp -t "hello world" /sd/note.txt     (write literal text)
 * Paths that are not /microsd or /sd fall back to the in-memory xfs.
 */
#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <xfs.h>
#include <fat.h>

static unsigned char cpbuf[512 * 1024] __attribute__((aligned(4)));
static unsigned long  cplen;

static int is_fat_path(const char *p)
{
    return (0 == strncmp(p, "/microsd/", 9)) || (0 == strncmp(p, "/sd/", 4));
}
static int read_cb(const unsigned char *buf, int len, void *ctx)
{
    (void)ctx;
    if (cplen + (unsigned)len > sizeof cpbuf) return 1;   /* too big: stop */
    memcpy(cpbuf + cplen, buf, len);
    cplen += len;
    return 0;
}
/* Point the FAT layer at the device named by @path; returns the file name part
 * (after the device prefix) or NULL on a bad path. */
static const char *fat_select(const char *path)
{
    extern int usbmsc_fat_select(void);
    if (0 == strncmp(path, "/microsd/", 9)) { fat_set_blkdev(0, 0); return path + 9; }
    if (0 == strncmp(path, "/sd/", 4)) {
        if (usbmsc_fat_select() != 0) { printf("cp: /sd not ready (USB card?)\n"); return NULL; }
        return path + 4;
    }
    return NULL;
}

/* Original xfs copy (in-memory filesystem). */
static int cp_xfs(char *src, char *dst)
{
    char buf[256];
    int in, out, n, w;
    in = xfsOpen(src, XFS_O_RDONLY);
    if (in < 0) { fprintf(stderr, "cp: %s: cannot open\n", src); return 1; }
    out = xfsOpen(dst, XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
    if (out < 0) { fprintf(stderr, "cp: %s: cannot create\n", dst); xfsClose(in); return 1; }
    while ((n = xfsRead(in, buf, sizeof(buf))) > 0) {
        w = xfsWrite(out, buf, n);
        if (w != n) { fprintf(stderr, "cp: short write\n"); break; }
    }
    xfsClose(in); xfsClose(out);
    return 0;
}

shellcmd xsh_cp(int nargs, char *args[])
{
    struct fat_dirent ent;
    const char *name;
    char *dstpath;

    /* cp -t "text" /dst/name : write literal text to a FAT file.  The Xinu
     * shell groups a quoted argument into one token but keeps the surrounding
     * quotes, so strip a matching leading/trailing " or ' here. */
    if (nargs == 4 && 0 == strcmp(args[1], "-t")) {
        char *txt = args[2];
        unsigned long tlen = strlen(txt);
        if (tlen >= 2 && (txt[0] == '"' || txt[0] == '\'') && txt[tlen - 1] == txt[0]) {
            txt++; tlen -= 2;
        }
        cplen = tlen;
        if (cplen > sizeof cpbuf) cplen = sizeof cpbuf;
        memcpy(cpbuf, txt, cplen);
        dstpath = args[3];
        goto write_dst;
    }

    if (nargs != 3) {
        fprintf(stderr, "usage: cp SRC DST   (SRC/DST = /microsd/NAME or /sd/NAME)\n");
        fprintf(stderr, "       cp -t \"text\" /sd/NAME\n");
        return 1;
    }

    /* If neither side touches FAT, use the original xfs copy. */
    if (!is_fat_path(args[1]) && !is_fat_path(args[2]))
        return cp_xfs(args[1], args[2]);

    /* ---- read the source ---- */
    if (is_fat_path(args[1])) {
        name = fat_select(args[1]);
        if (!name) return 1;
        cplen = 0;
        if (fat_mount() != 0 || fat_find_root(name, &ent) != 0) {
            printf("cp: source not found: %s\n", args[1]); fat_set_blkdev(0, 0); return 1;
        }
        if (fat_read_file(ent.cluster, ent.size, read_cb, NULL) != 0) {
            printf("cp: read error\n"); fat_set_blkdev(0, 0); return 1;
        }
        fat_set_blkdev(0, 0);
    } else {
        int in, n;
        in = xfsOpen(args[1], XFS_O_RDONLY);
        if (in < 0) { fprintf(stderr, "cp: %s: cannot open\n", args[1]); return 1; }
        cplen = 0;
        while ((n = xfsRead(in, (char *)cpbuf + cplen, sizeof cpbuf - cplen)) > 0) cplen += n;
        xfsClose(in);
    }
    dstpath = args[2];

write_dst:
    /* ---- write the destination ---- */
    if (is_fat_path(dstpath)) {
        name = fat_select(dstpath);
        if (!name) return 1;
        if (fat_write_root(name, cpbuf, cplen) != 0) {
            printf("cp: write failed (disk full / root full / I/O error)\n");
            fat_set_blkdev(0, 0); return 1;
        }
        fat_set_blkdev(0, 0);
        printf("cp: wrote %lu bytes to %s\n", cplen, dstpath);
    } else {
        int out, w;
        out = xfsOpen(dstpath, XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
        if (out < 0) { fprintf(stderr, "cp: %s: cannot create\n", dstpath); return 1; }
        w = xfsWrite(out, cpbuf, (int)cplen);
        xfsClose(out);
        if (w != (int)cplen) { fprintf(stderr, "cp: short write\n"); return 1; }
    }
    return 0;
}
