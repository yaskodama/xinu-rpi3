/**
 * @file fat.h
 *
 * Minimal read-only FAT32 layer over the SDHCI block driver
 * (apps/sd_block.c).  Just enough to list the boot partition the firmware
 * loaded kernel.img from, so `ls /microsd` works from the Xinu shell.
 *
 * Scope: MBR partition scan -> first FAT32 partition -> VBR geometry ->
 * walk a directory's cluster chain.  Long-file-name (VFAT) entries are
 * reassembled so real names (e.g. "bcm2710-rpi-3-b.dtb") show, not just
 * mangled 8.3 names.  No writes, no file reads yet — directory listing
 * only.
 */
#ifndef _FAT_H_
#define _FAT_H_

/* One listed directory entry handed to the fat_list callback. */
struct fat_dirent {
    char          name[256];    /* NUL-terminated long (or 8.3) name */
    unsigned long size;         /* file size in bytes (0 for dirs)   */
    unsigned long cluster;      /* first cluster of the entry        */
    int           is_dir;       /* 1 if a subdirectory               */
};

/**
 * Mount the SD card's first FAT32 partition (idempotent).  Reads the MBR
 * and VBR and caches the geometry.  Returns 0 on success, -1 if the SD
 * read failed or no FAT32 partition / 512-byte sectors were found.
 */
int fat_mount(void);

/**
 * Walk the root directory, invoking @cb for each real entry (LFN
 * reassembled, "." / ".." / volume-label / deleted entries skipped).
 * @cb returns 0 to continue or non-zero to stop early.  Returns 0 on a
 * clean walk, -1 on mount/read error.
 */
int fat_list_root(int (*cb)(const struct fat_dirent *e, void *ctx), void *ctx);

/**
 * Find a root-directory entry by name (case-insensitive, long or 8.3).
 * Fills *out and returns 0 if found, -1 otherwise.
 */
int fat_find_root(const char *name, struct fat_dirent *out);

/**
 * Stream a file's bytes: starting at @first_cluster, walk the cluster chain
 * and invoke @cb with each chunk (<= 512 bytes) until @size bytes have been
 * delivered.  @cb returns 0 to continue, non-zero to stop.  Returns 0 on a
 * clean read, -1 on mount/read error.
 */
int fat_read_file(unsigned long first_cluster, unsigned long size,
                  int (*cb)(const unsigned char *buf, int len, void *ctx),
                  void *ctx);

#endif /* _FAT_H_ */
