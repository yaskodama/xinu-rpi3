/**
 * @file fat.c
 *
 * Minimal read-only FAT32 directory layer over apps/sd_block.c.  See
 * fat.h.  Powers `ls /microsd` from the Xinu shell.
 */

#include "sd_block.h"
#include <fat.h>

/* little-endian field readers */
static unsigned       rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned long  rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* cached geometry (set by fat_mount) */
static int           fat_mounted;
static unsigned long fat_first_lba;     /* LBA of the FAT region          */
static unsigned long fat_data_lba;      /* LBA of cluster 2 (data region) */
static unsigned      fat_spc;           /* sectors per cluster            */
static unsigned long fat_root_clus;     /* first cluster of root dir      */

/* one shared, 4-byte-aligned sector buffer (sd_read_block drains 32-bit) */
static unsigned char secbuf[SD_BLOCK_SIZE] __attribute__((aligned(4)));

int fat_mount(void)
{
    unsigned char *b = secbuf;
    unsigned long part_lba = 0;
    int i, found = 0;

    if (fat_mounted) return 0;

    /* MBR: scan the 4 primary partition entries for a FAT32 type. */
    if (sd_read_block(0, b) != 0) return -1;
    if (b[510] != 0x55 || b[511] != 0xAA) return -1;
    for (i = 0; i < 4; i++) {
        unsigned char *e = b + 446 + 16 * i;
        unsigned char type = e[4];
        if (type == 0x0B || type == 0x0C) {     /* FAT32 / FAT32-LBA */
            part_lba = rd32(e + 8);
            found = 1;
            break;
        }
    }
    if (!found) return -1;

    /* VBR (BPB) of that partition. */
    if (sd_read_block(part_lba, b) != 0) return -1;
    {
        unsigned bytes_per_sec = rd16(b + 11);
        unsigned sec_per_clus  = b[13];
        unsigned reserved      = rd16(b + 14);
        unsigned num_fats      = b[16];
        unsigned long fat_sz   = rd32(b + 36);   /* FATSz32  */
        unsigned long root_clus = rd32(b + 44);  /* root dir first cluster */

        if (bytes_per_sec != SD_BLOCK_SIZE || sec_per_clus == 0) return -1;

        fat_first_lba = part_lba + reserved;
        fat_data_lba  = part_lba + reserved + (unsigned long)num_fats * fat_sz;
        fat_spc       = sec_per_clus;
        fat_root_clus = root_clus ? root_clus : 2;
    }
    fat_mounted = 1;
    return 0;
}

static unsigned long clus_to_lba(unsigned long c)
{
    return fat_data_lba + (c - 2) * fat_spc;
}

/* Next cluster in the chain (reads one FAT sector into a local buffer so it
 * does not clobber the directory sector the caller is iterating). */
static unsigned long fat_next(unsigned long c)
{
    static unsigned char fbuf[SD_BLOCK_SIZE] __attribute__((aligned(4)));
    unsigned long off = c * 4;
    if (sd_read_block(fat_first_lba + off / SD_BLOCK_SIZE, fbuf) != 0) return 0x0FFFFFFF;
    return rd32(fbuf + (off % SD_BLOCK_SIZE)) & 0x0FFFFFFFUL;
}

/* Reassemble a VFAT long-name entry's 13 UTF-16 units into ASCII. */
static void lfn_chars(const unsigned char *d, char *name)
{
    static const int off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    int seq = d[0] & 0x3F, k;
    if (seq < 1 || seq > 19) return;
    int base = (seq - 1) * 13;
    for (k = 0; k < 13; k++) {
        unsigned u = (unsigned)d[off[k]] | ((unsigned)d[off[k] + 1] << 8);
        char ch = (u == 0 || u == 0xFFFF) ? 0 : (u < 0x80 ? (char)u : '?');
        if (base + k < 255) name[base + k] = ch;
    }
}

/* Build the 8.3 short name (used when no LFN precedes the entry). */
static void short_name(const unsigned char *d, char *name)
{
    int n = 0, k;
    for (k = 0; k < 8 && d[k] != ' '; k++) name[n++] = d[k];
    if (d[8] != ' ') {
        name[n++] = '.';
        for (k = 8; k < 11 && d[k] != ' '; k++) name[n++] = d[k];
    }
    name[n] = 0;
}

int fat_list_root(int (*cb)(const struct fat_dirent *e, void *ctx), void *ctx)
{
    unsigned long clus;
    char lfn[256];
    int have_lfn = 0;

    if (fat_mount() != 0) return -1;
    lfn[0] = 0;

    for (clus = fat_root_clus; clus >= 2 && clus < 0x0FFFFFF8UL; clus = fat_next(clus)) {
        unsigned s;
        for (s = 0; s < fat_spc; s++) {
            int e;
            if (sd_read_block(clus_to_lba(clus) + s, secbuf) != 0) return -1;
            for (e = 0; e < SD_BLOCK_SIZE; e += 32) {
                unsigned char *d = secbuf + e;
                unsigned char attr;

                if (d[0] == 0x00) return 0;          /* end of directory   */
                if (d[0] == 0xE5) { have_lfn = 0; continue; }  /* deleted   */
                attr = d[11];

                if (attr == 0x0F) {                  /* LFN component       */
                    if (d[0] & 0x40) { int i; for (i = 0; i < 256; i++) lfn[i] = 0; }
                    lfn_chars(d, lfn);
                    have_lfn = 1;
                    continue;
                }
                if (attr & 0x08) { have_lfn = 0; continue; }   /* vol label */
                if (attr & 0x02) { have_lfn = 0; continue; }   /* hidden (e.g. macOS .fseventsd) */
                if (d[0] == '.') { have_lfn = 0; continue; }   /* . / ..    */

                {
                    struct fat_dirent ent;
                    if (have_lfn && lfn[0]) {
                        int i; for (i = 0; i < 255 && lfn[i]; i++) ent.name[i] = lfn[i];
                        ent.name[i] = 0;
                    } else {
                        short_name(d, ent.name);
                    }
                    ent.is_dir  = (attr & 0x10) ? 1 : 0;
                    ent.size    = rd32(d + 28);
                    ent.cluster = ((unsigned long)rd16(d + 20) << 16) | rd16(d + 26);
                    have_lfn = 0;
                    if (cb(&ent, ctx) != 0) return 0;
                }
            }
        }
    }
    return 0;
}
