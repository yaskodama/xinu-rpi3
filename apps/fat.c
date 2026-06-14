/**
 * @file fat.c
 *
 * Minimal read-only FAT32 directory layer over apps/sd_block.c.  See
 * fat.h.  Powers `ls /microsd` from the Xinu shell.
 */

#include "sd_block.h"
#include <fat.h>
#include <string.h>

static unsigned long clus_to_lba(unsigned long c);   /* defined below */
static unsigned long fat_next(unsigned long c);

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

/* extra geometry cached by fat_mount() for the write path */
static unsigned      fat_num_fats;      /* number of FAT copies (usually 2)     */
static unsigned long fat_sz_sectors;    /* sectors per FAT                       */
static unsigned long fat_part_lba;      /* LBA of the partition (VBR)            */

/* The block device the FAT layer reads from / writes to.  Defaults to the
 * on-board SD/EMMC; ls /sd and cp point it at the USB mass-storage reader via
 * fat_set_blkdev() so the same FAT code drives either card. */
static int (*g_blk)(unsigned long, void *) = sd_read_block;
static int (*g_blkw)(unsigned long, const void *) = sd_write_block;
void fat_set_blkdev(int (*reader)(unsigned long, void *),
                    int (*writer)(unsigned long, const void *)) {
    if (reader == 0) reader = sd_read_block;
    if (writer == 0) writer = sd_write_block;
    if (reader != g_blk) { g_blk = reader; g_blkw = writer; fat_mounted = 0; }  /* force re-mount */
    else g_blkw = writer;
}

/* one shared, 4-byte-aligned sector buffer (sd_read_block drains 32-bit) */
static unsigned char secbuf[SD_BLOCK_SIZE] __attribute__((aligned(4)));

int fat_mount(void)
{
    unsigned char *b = secbuf;
    unsigned long part_lba = 0;
    int i, found = 0;

    if (fat_mounted) return 0;

    /* MBR: scan the 4 primary partition entries for a FAT32 type. */
    if (g_blk(0, b) != 0) return -1;
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
    if (g_blk(part_lba, b) != 0) return -1;
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
        fat_num_fats   = num_fats;
        fat_sz_sectors = fat_sz;
        fat_part_lba   = part_lba;
    }
    fat_mounted = 1;
    return 0;
}

/* ===================================================================
 * Write support: allocate clusters, update the FAT (all copies), and add a
 * file to the root directory (8.3 names, root only — enough for `cp .. /sd/`).
 * =================================================================== */
static void wr32(unsigned char *p, unsigned long v)
{ p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24); }
static void wr16(unsigned char *p, unsigned v)
{ p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); }

/* Set FAT[cluster] = value in every FAT copy. */
static int fat_set_entry(unsigned long cluster, unsigned long value)
{
    static unsigned char fbuf[SD_BLOCK_SIZE] __attribute__((aligned(4)));
    unsigned long off = cluster * 4;
    unsigned long rel = off / SD_BLOCK_SIZE;
    unsigned f;
    for (f = 0; f < fat_num_fats; f++) {
        unsigned long sec = fat_first_lba + f * fat_sz_sectors + rel;
        if (g_blk(sec, fbuf) != 0) return -1;
        unsigned long cur = rd32(fbuf + (off % SD_BLOCK_SIZE)) & 0xF0000000UL;
        wr32(fbuf + (off % SD_BLOCK_SIZE), cur | (value & 0x0FFFFFFFUL));
        if (g_blkw(sec, fbuf) != 0) return -1;
    }
    return 0;
}

/* Find a free cluster (FAT entry == 0), mark it end-of-chain, return it (0=full). */
static unsigned long fat_alloc(void)
{
    static unsigned char fbuf[SD_BLOCK_SIZE] __attribute__((aligned(4)));
    unsigned long maxc = fat_sz_sectors * (SD_BLOCK_SIZE / 4);
    unsigned long c;
    for (c = 2; c < maxc; c++) {
        unsigned long off = c * 4;
        if (g_blk(fat_first_lba + off / SD_BLOCK_SIZE, fbuf) != 0) return 0;
        if ((rd32(fbuf + (off % SD_BLOCK_SIZE)) & 0x0FFFFFFFUL) == 0) {
            if (fat_set_entry(c, 0x0FFFFFFFUL) != 0) return 0;   /* EOC */
            return c;
        }
    }
    return 0;
}

/* Free a whole cluster chain (set each entry to 0). */
static void fat_free_chain(unsigned long c)
{
    while (c >= 2 && c < 0x0FFFFFF8UL) {
        unsigned long nx = fat_next(c);
        fat_set_entry(c, 0);
        c = nx;
    }
}

/* Build an 8.3 padded name (11 bytes, upper-case) from a user name. */
static void make_83(const char *name, unsigned char out[11])
{
    int i, n = 0;
    for (i = 0; i < 11; i++) out[i] = ' ';
    /* base (up to 8) */
    for (; *name && *name != '.' && n < 8; name++) {
        char ch = *name; if (ch >= 'a' && ch <= 'z') ch -= 32;
        out[n++] = (unsigned char)ch;
    }
    while (*name && *name != '.') name++;
    if (*name == '.') {
        name++; n = 8;
        for (; *name && n < 11; name++) {
            char ch = *name; if (ch >= 'a' && ch <= 'z') ch -= 32;
            out[n++] = (unsigned char)ch;
        }
    }
}

/* Write (create/overwrite) a root-directory file.  Returns 0 on success. */
int fat_write_root(const char *name, const unsigned char *data, unsigned long size)
{
    static unsigned char dbuf[SD_BLOCK_SIZE] __attribute__((aligned(4)));
    static unsigned char zero[SD_BLOCK_SIZE] __attribute__((aligned(4)));
    unsigned char name83[11];
    unsigned long clus, first = 0, prev = 0;
    unsigned long bytes_per_clus, remaining;
    int i;

    if (fat_mount() != 0) return -1;
    make_83(name, name83);
    bytes_per_clus = (unsigned long)fat_spc * SD_BLOCK_SIZE;

    /* If a file with this 8.3 name already exists, free its old chain so we can
     * overwrite cleanly, and remember the directory slot to reuse. */
    unsigned long dir_clus = 0, dir_sec = 0, dir_off = 0;
    {
        unsigned long c2;
        for (c2 = fat_root_clus; c2 >= 2 && c2 < 0x0FFFFFF8UL && !dir_clus; c2 = fat_next(c2)) {
            unsigned s;
            for (s = 0; s < fat_spc && !dir_clus; s++) {
                int e;
                if (g_blk(clus_to_lba(c2) + s, dbuf) != 0) return -1;
                for (e = 0; e < SD_BLOCK_SIZE; e += 32) {
                    unsigned char *d = dbuf + e;
                    if (d[11] == 0x0F) continue;                 /* LFN component */
                    if (memcmp(d, name83, 11) == 0) {            /* same name -> overwrite */
                        unsigned long oc = ((unsigned long)rd16(d + 20) << 16) | rd16(d + 26);
                        fat_free_chain(oc);
                        dir_clus = c2; dir_sec = s; dir_off = e;
                        break;
                    }
                }
            }
        }
    }

    /* Allocate + write the data clusters. */
    remaining = size;
    while (remaining > 0) {
        unsigned s;
        clus = fat_alloc();
        if (clus == 0) return -1;                    /* disk full */
        if (first == 0) first = clus;
        if (prev != 0) fat_set_entry(prev, clus);    /* link previous -> this */
        prev = clus;
        for (s = 0; s < fat_spc && remaining > 0; s++) {
            unsigned long n = remaining < SD_BLOCK_SIZE ? remaining : SD_BLOCK_SIZE;
            memcpy(dbuf, zero, SD_BLOCK_SIZE);
            memcpy(dbuf, data, n);
            if (g_blkw(clus_to_lba(clus) + s, dbuf) != 0) return -1;
            data += n; remaining -= n;
        }
    }

    /* Find a free directory slot if not overwriting. */
    if (!dir_clus) {
        unsigned long c2;
        for (c2 = fat_root_clus; c2 >= 2 && c2 < 0x0FFFFFF8UL && !dir_clus; c2 = fat_next(c2)) {
            unsigned s;
            for (s = 0; s < fat_spc && !dir_clus; s++) {
                int e;
                if (g_blk(clus_to_lba(c2) + s, dbuf) != 0) return -1;
                for (e = 0; e < SD_BLOCK_SIZE; e += 32) {
                    if (dbuf[e] == 0x00 || dbuf[e] == 0xE5) {
                        dir_clus = c2; dir_sec = s; dir_off = e;
                        break;
                    }
                }
            }
        }
        if (!dir_clus) return -1;                    /* root directory full */
    }

    /* Write the directory entry. */
    if (g_blk(clus_to_lba(dir_clus) + dir_sec, dbuf) != 0) return -1;
    {
        unsigned char *d = dbuf + dir_off;
        for (i = 0; i < 11; i++) d[i] = name83[i];
        d[11] = 0x20;                                /* archive */
        for (i = 12; i < 32; i++) d[i] = 0;
        wr16(d + 20, (unsigned)(first >> 16));       /* first cluster hi */
        wr16(d + 26, (unsigned)(first & 0xFFFF));    /* first cluster lo */
        wr32(d + 28, size);                          /* file size */
    }
    if (g_blkw(clus_to_lba(dir_clus) + dir_sec, dbuf) != 0) return -1;
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
    if (g_blk(fat_first_lba + off / SD_BLOCK_SIZE, fbuf) != 0) return 0x0FFFFFFF;
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

/* case-insensitive ASCII string equality */
static int fat_ci_eq(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

struct fat_find { const char *want; struct fat_dirent *out; int found; };
static int fat_find_cb(const struct fat_dirent *e, void *ctx)
{
    struct fat_find *f = (struct fat_find *)ctx;
    if (fat_ci_eq(e->name, f->want)) { *f->out = *e; f->found = 1; return 1; }
    return 0;
}
int fat_find_root(const char *name, struct fat_dirent *out)
{
    struct fat_find f; f.want = name; f.out = out; f.found = 0;
    if (fat_list_root(fat_find_cb, &f) != 0) return -1;
    return f.found ? 0 : -1;
}

int fat_read_file(unsigned long clus, unsigned long size,
                  int (*cb)(const unsigned char *buf, int len, void *ctx),
                  void *ctx)
{
    unsigned long remaining = size;

    if (fat_mount() != 0) return -1;
    while (clus >= 2 && clus < 0x0FFFFFF8UL && remaining > 0)
    {
        unsigned s;
        for (s = 0; s < fat_spc && remaining > 0; s++)
        {
            int n = (remaining < SD_BLOCK_SIZE) ? (int)remaining : SD_BLOCK_SIZE;
            if (g_blk(clus_to_lba(clus) + s, secbuf) != 0) return -1;
            if (cb(secbuf, n, ctx) != 0) return 0;
            remaining -= n;
        }
        clus = fat_next(clus);
    }
    return 0;
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
            if (g_blk(clus_to_lba(clus) + s, secbuf) != 0) return -1;
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
