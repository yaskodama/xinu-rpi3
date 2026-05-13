/**
 * @file xfs.c
 *
 * Hierarchical block-device filesystem.  The on-disk layout is:
 *
 *   block 0        : superblock
 *   block 1        : inode bitmap (1 block, supports up to 32768 inodes)
 *   block 2        : data block bitmap (1 block, up to 32768 data blocks)
 *   block 3..N     : inode table (xinode_count * 128 / 4096 blocks)
 *   block N+1..end : data blocks
 *
 * Indirection: 12 direct + 1 single-indirect (1024 entries).
 * Max file size: ~4 MB.  Plenty for a RAM-disk demo.
 */

#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <device.h>
#include <semaphore.h>
#include <string.h>
#include <stdio.h>
#include <thread.h>
#include <interrupt.h>
#include <memory.h>
#include <xfs.h>

/* ---------- module state ---------- */

static struct xmount xfs_mtab[XFS_MAX_MOUNTS];
static struct xfile  xfs_ftab[XFS_MAX_OPEN];
static char          xfs_cwd_tab[NTHREAD][XFS_PATH_MAX];
static semaphore     xfs_lock;
static int           xfs_inited = 0;

#define INODES_PER_BLK (XFS_BLOCK_SIZE / sizeof(struct xinode))

/* ---------- block I/O ---------- */

static int blk_read(struct xmount *m, uint32_t blk, void *buf)
{
    return m->bd.read_block(m->bd.priv, blk, buf);
}

static int blk_write(struct xmount *m, uint32_t blk, const void *buf)
{
    return m->bd.write_block(m->bd.priv, blk, buf);
}

/* ---------- bitmap ---------- */

static int bmap_set(struct xmount *m, uint32_t bmap_blk, uint32_t idx, int set)
{
    uint8_t buf[XFS_BLOCK_SIZE];
    uint32_t blk = bmap_blk + idx / (XFS_BLOCK_SIZE * 8);
    uint32_t off = idx % (XFS_BLOCK_SIZE * 8);

    if (OK != blk_read(m, blk, buf)) return SYSERR;
    if (set) buf[off >> 3] |=  (1u << (off & 7));
    else     buf[off >> 3] &= ~(1u << (off & 7));
    return blk_write(m, blk, buf);
}

/* Allocate the lowest unset bit in [1..max-1] (skip index 0).  Returns the
 * allocated index or SYSERR. */
static int bmap_alloc(struct xmount *m, uint32_t bmap_blk, uint32_t bmap_nblks,
                      uint32_t max)
{
    uint8_t buf[XFS_BLOCK_SIZE];
    uint32_t b, byte, bit, idx;

    for (b = 0; b < bmap_nblks; b++)
    {
        if (OK != blk_read(m, bmap_blk + b, buf)) return SYSERR;
        for (byte = 0; byte < XFS_BLOCK_SIZE; byte++)
        {
            if (buf[byte] == 0xFF) continue;
            for (bit = 0; bit < 8; bit++)
            {
                if (buf[byte] & (1u << bit)) continue;
                idx = b * XFS_BLOCK_SIZE * 8 + byte * 8 + bit;
                if (idx == 0) continue;        /* reserve 0 */
                if (idx >= max) return SYSERR;
                buf[byte] |= (1u << bit);
                if (OK != blk_write(m, bmap_blk + b, buf)) return SYSERR;
                return (int)idx;
            }
        }
    }
    return SYSERR;
}

/* ---------- inode I/O ---------- */

static int inode_read(struct xmount *m, uint32_t ino, struct xinode *out)
{
    uint8_t buf[XFS_BLOCK_SIZE];
    uint32_t blk = m->super.inode_tbl_blk + ino / INODES_PER_BLK;
    uint32_t off = (ino % INODES_PER_BLK) * sizeof(struct xinode);

    if (ino == 0 || ino >= m->super.inode_count) return SYSERR;
    if (OK != blk_read(m, blk, buf)) return SYSERR;
    memcpy(out, buf + off, sizeof(*out));
    return OK;
}

static int inode_write(struct xmount *m, uint32_t ino,
                       const struct xinode *in)
{
    uint8_t buf[XFS_BLOCK_SIZE];
    uint32_t blk = m->super.inode_tbl_blk + ino / INODES_PER_BLK;
    uint32_t off = (ino % INODES_PER_BLK) * sizeof(struct xinode);

    if (ino == 0 || ino >= m->super.inode_count) return SYSERR;
    if (OK != blk_read(m, blk, buf)) return SYSERR;
    memcpy(buf + off, in, sizeof(*in));
    return blk_write(m, blk, buf);
}

static int inode_alloc(struct xmount *m, uint16_t mode, uint32_t *out_ino)
{
    int ino;
    struct xinode in;

    ino = bmap_alloc(m, m->super.inode_bmap_blk, m->super.inode_bmap_nblks,
                     m->super.inode_count);
    if (SYSERR == ino) return SYSERR;

    memset(&in, 0, sizeof(in));
    in.mode  = mode;
    in.nlink = 1;
    if (OK != inode_write(m, (uint32_t)ino, &in))
    {
        bmap_set(m, m->super.inode_bmap_blk, (uint32_t)ino, 0);
        return SYSERR;
    }
    m->super.free_inodes--;
    *out_ino = (uint32_t)ino;
    return OK;
}

/* ---------- data block alloc/free ---------- */

static int dblock_alloc(struct xmount *m, uint32_t *out_blk)
{
    int idx;
    uint8_t zero[XFS_BLOCK_SIZE];

    idx = bmap_alloc(m, m->super.data_bmap_blk, m->super.data_bmap_nblks,
                     m->super.data_blocks);
    if (SYSERR == idx) return SYSERR;
    memset(zero, 0, sizeof(zero));
    if (OK != blk_write(m, m->super.data_start_blk + (uint32_t)idx, zero))
    {
        bmap_set(m, m->super.data_bmap_blk, (uint32_t)idx, 0);
        return SYSERR;
    }
    m->super.free_blocks--;
    *out_blk = m->super.data_start_blk + (uint32_t)idx;
    return OK;
}

static int dblock_free(struct xmount *m, uint32_t blk)
{
    uint32_t idx;

    if (blk < m->super.data_start_blk) return SYSERR;
    idx = blk - m->super.data_start_blk;
    if (idx >= m->super.data_blocks) return SYSERR;
    if (OK != bmap_set(m, m->super.data_bmap_blk, idx, 0)) return SYSERR;
    m->super.free_blocks++;
    return OK;
}

/* Map a file-relative block index to an absolute block number, optionally
 * allocating along the way.  *in is updated in place; caller writes it back. */
static int file_bmap(struct xmount *m, struct xinode *in, uint32_t fblk,
                     int create, uint32_t *out_blk)
{
    uint32_t ind[XFS_NIND_PER_BLK];
    uint32_t newblk;

    if (fblk < XFS_NDIRECT)
    {
        if (in->direct[fblk] == 0)
        {
            if (!create) { *out_blk = 0; return OK; }
            if (OK != dblock_alloc(m, &newblk)) return SYSERR;
            in->direct[fblk] = newblk;
        }
        *out_blk = in->direct[fblk];
        return OK;
    }
    fblk -= XFS_NDIRECT;
    if (fblk < XFS_NIND_PER_BLK)
    {
        if (in->indirect == 0)
        {
            if (!create) { *out_blk = 0; return OK; }
            if (OK != dblock_alloc(m, &newblk)) return SYSERR;
            in->indirect = newblk;
        }
        if (OK != blk_read(m, in->indirect, ind)) return SYSERR;
        if (ind[fblk] == 0)
        {
            if (!create) { *out_blk = 0; return OK; }
            if (OK != dblock_alloc(m, &newblk)) return SYSERR;
            ind[fblk] = newblk;
            if (OK != blk_write(m, in->indirect, ind)) return SYSERR;
        }
        *out_blk = ind[fblk];
        return OK;
    }
    return SYSERR;        /* file too large for current implementation */
}

static int file_truncate(struct xmount *m, struct xinode *in)
{
    uint32_t i;
    uint32_t ind[XFS_NIND_PER_BLK];

    for (i = 0; i < XFS_NDIRECT; i++)
    {
        if (in->direct[i] != 0) dblock_free(m, in->direct[i]);
        in->direct[i] = 0;
    }
    if (in->indirect != 0)
    {
        if (OK == blk_read(m, in->indirect, ind))
        {
            for (i = 0; i < XFS_NIND_PER_BLK; i++)
                if (ind[i] != 0) dblock_free(m, ind[i]);
        }
        dblock_free(m, in->indirect);
        in->indirect = 0;
    }
    in->size = 0;
    return OK;
}

/* ---------- file read/write at the inode layer ---------- */

static int inode_read_bytes(struct xmount *m, struct xinode *in, uint32_t pos,
                            void *buf, uint32_t len)
{
    uint8_t blkbuf[XFS_BLOCK_SIZE];
    uint8_t *out = (uint8_t *)buf;
    uint32_t copied = 0, fblk, off, n, abs_blk;

    if (pos >= in->size) return 0;
    if (pos + len > in->size) len = in->size - pos;

    while (copied < len)
    {
        fblk = (pos + copied) / XFS_BLOCK_SIZE;
        off  = (pos + copied) % XFS_BLOCK_SIZE;
        n    = XFS_BLOCK_SIZE - off;
        if (n > len - copied) n = len - copied;

        if (OK != file_bmap(m, in, fblk, 0, &abs_blk)) return SYSERR;
        if (abs_blk == 0)
        {
            memset(out + copied, 0, n);
        }
        else
        {
            if (OK != blk_read(m, abs_blk, blkbuf)) return SYSERR;
            memcpy(out + copied, blkbuf + off, n);
        }
        copied += n;
    }
    return (int)copied;
}

static int inode_write_bytes(struct xmount *m, struct xinode *in, uint32_t pos,
                             const void *buf, uint32_t len)
{
    uint8_t blkbuf[XFS_BLOCK_SIZE];
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0, fblk, off, n, abs_blk;

    while (written < len)
    {
        fblk = (pos + written) / XFS_BLOCK_SIZE;
        off  = (pos + written) % XFS_BLOCK_SIZE;
        n    = XFS_BLOCK_SIZE - off;
        if (n > len - written) n = len - written;

        if (OK != file_bmap(m, in, fblk, 1, &abs_blk)) break;
        if (off != 0 || n != XFS_BLOCK_SIZE)
        {
            if (OK != blk_read(m, abs_blk, blkbuf)) break;
        }
        memcpy(blkbuf + off, src + written, n);
        if (OK != blk_write(m, abs_blk, blkbuf)) break;
        written += n;
    }
    if (pos + written > in->size) in->size = pos + written;
    return (int)written;
}

/* ---------- directory ops ---------- */

static int dir_iter_lookup(struct xmount *m, struct xinode *dir_in,
                           const char *name, uint32_t *out_ino,
                           uint32_t *out_slot, uint8_t *out_type)
{
    struct xdirent ent;
    uint32_t i, total;
    int r;
    size_t nlen;

    nlen = strnlen(name, XFS_MAX_NAME);
    total = dir_in->size / sizeof(struct xdirent);

    for (i = 0; i < total; i++)
    {
        r = inode_read_bytes(m, dir_in, i * sizeof(ent), &ent, sizeof(ent));
        if (r != (int)sizeof(ent)) break;
        if (ent.ino == XFS_NULL_INO) continue;
        if (ent.name_len == nlen &&
            0 == memcmp(ent.name, name, nlen))
        {
            if (out_ino)  *out_ino  = ent.ino;
            if (out_slot) *out_slot = i;
            if (out_type) *out_type = ent.type;
            return OK;
        }
    }
    return SYSERR;
}

static int dir_insert(struct xmount *m, uint32_t dir_ino,
                      const char *name, uint32_t target_ino, uint8_t type)
{
    struct xinode dir_in;
    struct xdirent ent;
    uint32_t i, total, slot;
    size_t nlen;
    int r, found_free = 0;

    if (OK != inode_read(m, dir_ino, &dir_in)) return SYSERR;
    if (XFS_MODE_TYPE(dir_in.mode) != XFS_MODE_DIR) return SYSERR;

    nlen = strnlen(name, XFS_MAX_NAME);
    if (nlen == 0 || nlen > XFS_MAX_NAME) return SYSERR;

    /* Detect collision and find a free slot in one pass. */
    total = dir_in.size / sizeof(struct xdirent);
    slot  = total;
    for (i = 0; i < total; i++)
    {
        r = inode_read_bytes(m, &dir_in, i * sizeof(ent), &ent, sizeof(ent));
        if (r != (int)sizeof(ent)) return SYSERR;
        if (ent.ino == XFS_NULL_INO)
        {
            if (!found_free) { slot = i; found_free = 1; }
            continue;
        }
        if (ent.name_len == nlen && 0 == memcmp(ent.name, name, nlen))
            return SYSERR;          /* already exists */
    }

    memset(&ent, 0, sizeof(ent));
    ent.ino      = target_ino;
    ent.name_len = (uint8_t)nlen;
    ent.type     = type;
    memcpy(ent.name, name, nlen);

    r = inode_write_bytes(m, &dir_in, slot * sizeof(ent), &ent, sizeof(ent));
    if (r != (int)sizeof(ent)) return SYSERR;
    return inode_write(m, dir_ino, &dir_in);
}

static int dir_remove(struct xmount *m, uint32_t dir_ino, const char *name)
{
    struct xinode dir_in;
    struct xdirent ent;
    uint32_t slot;
    int r;

    if (OK != inode_read(m, dir_ino, &dir_in)) return SYSERR;
    if (SYSERR == dir_iter_lookup(m, &dir_in, name, NULL, &slot, NULL))
        return SYSERR;

    memset(&ent, 0, sizeof(ent));
    r = inode_write_bytes(m, &dir_in, slot * sizeof(ent), &ent, sizeof(ent));
    if (r != (int)sizeof(ent)) return SYSERR;
    return inode_write(m, dir_ino, &dir_in);
}

static int dir_empty(struct xmount *m, uint32_t dir_ino)
{
    struct xinode dir_in;
    struct xdirent ent;
    uint32_t i, total;

    if (OK != inode_read(m, dir_ino, &dir_in)) return SYSERR;
    total = dir_in.size / sizeof(struct xdirent);
    for (i = 0; i < total; i++)
    {
        if (sizeof(ent) != (uint32_t)inode_read_bytes(m, &dir_in,
                                            i * sizeof(ent), &ent, sizeof(ent)))
            return SYSERR;
        if (ent.ino == XFS_NULL_INO) continue;
        if (ent.name_len == 1 && ent.name[0] == '.') continue;
        if (ent.name_len == 2 && ent.name[0] == '.' && ent.name[1] == '.') continue;
        return 0;
    }
    return 1;
}

/* ---------- mount table + path resolution ---------- */

static struct xmount *find_mount(const char *abspath)
{
    struct xmount *best = NULL;
    int best_len = -1;
    int i, mlen;

    for (i = 0; i < XFS_MAX_MOUNTS; i++)
    {
        if (!xfs_mtab[i].in_use) continue;
        mlen = (int)strlen(xfs_mtab[i].mountpoint);
        if (0 == strncmp(abspath, xfs_mtab[i].mountpoint, mlen) &&
            (abspath[mlen] == '/' || abspath[mlen] == '\0' ||
             (mlen == 1 && xfs_mtab[i].mountpoint[0] == '/')))
        {
            if (mlen > best_len)
            {
                best = &xfs_mtab[i];
                best_len = mlen;
            }
        }
    }
    return best;
}

/* Normalize and concatenate.  cwd must already be absolute. */
/* Walk up the parent chain to find an inherited cwd.  Falls back to "/" if
 * no ancestor (including self) has set one. */
static const char *cwd_lookup(void)
{
    tid_typ tid = gettid();
    int hops = 0;
    while (hops++ < NTHREAD)
    {
        if (tid < 0 || tid >= NTHREAD) break;
        if (xfs_cwd_tab[tid][0] != 0) return xfs_cwd_tab[tid];
        if (tid == NULLTHREAD) break;
        tid = thrtab[tid].parent;
    }
    return "/";
}

int xfsAbspath(const char *in, char *out)
{
    char tmp[XFS_PATH_MAX];
    char comp[XFS_MAX_NAME + 1];
    int  ncomp = 0;
    const char *p;
    int  i, n, len;

    if (in == NULL || out == NULL) return SYSERR;
    out[0] = 0;

    if (in[0] == '/')
    {
        strlcpy(tmp, in, sizeof(tmp));
    }
    else
    {
        strlcpy(tmp, cwd_lookup(), sizeof(tmp));
        n = strlen(tmp);
        if (n == 0 || tmp[n-1] != '/')
        {
            if (n + 1 >= (int)sizeof(tmp)) return SYSERR;
            tmp[n++] = '/';
            tmp[n]   = '\0';
        }
        if (strlen(tmp) + strlen(in) + 1 > sizeof(tmp)) return SYSERR;
        strncpy(tmp + strlen(tmp), in, sizeof(tmp) - strlen(tmp) - 1);
    }

    /* Walk components, resolving "." and "..".  Build `out` directly: when we
     * pop on "..", strip the trailing component from `out` rather than
     * tracking a parts array. */
    p = tmp;
    while (*p)
    {
        while (*p == '/') p++;
        if (*p == 0) break;
        n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p && *p != '/') p++;        /* skip overflow */
        if (n == 0) continue;
        if (n == 1 && comp[0] == '.') continue;
        if (n == 2 && comp[0] == '.' && comp[1] == '.')
        {
            if (ncomp > 0)
            {
                len = strlen(out);
                while (len > 0 && out[len - 1] != '/') len--;
                if (len > 0) len--;        /* drop the slash too */
                out[len] = 0;
                ncomp--;
            }
            continue;
        }
        len = strlen(out);
        if (len + 1 + n + 1 >= XFS_PATH_MAX) return SYSERR;
        out[len++] = '/';
        for (i = 0; i < n; i++) out[len++] = comp[i];
        out[len] = 0;
        ncomp++;
    }
    if (ncomp == 0) strlcpy(out, "/", XFS_PATH_MAX);
    return OK;
}

/* Resolve absolute path to (mount, inode).  Also returns the path remainder
 * inside the mount (useful for parent resolution). */
static int resolve_abs(const char *abspath, struct xmount **m_out,
                       uint32_t *ino_out)
{
    struct xmount *m;
    const char *p;
    char comp[XFS_MAX_NAME + 1];
    int n;
    uint32_t cur, child;
    struct xinode in;
    uint8_t type;
    int mplen;

    m = find_mount(abspath);
    if (m == NULL) return SYSERR;
    mplen = strlen(m->mountpoint);
    p = abspath + mplen;
    if (m->mountpoint[mplen-1] != '/' && *p == '/') p++;

    cur = m->super.root_inode;
    while (*p)
    {
        while (*p == '/') p++;
        if (*p == 0) break;
        n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p && *p != '/') p++;
        if (n == 0) continue;

        if (OK != inode_read(m, cur, &in)) return SYSERR;
        if (XFS_MODE_TYPE(in.mode) != XFS_MODE_DIR) return SYSERR;
        if (OK != dir_iter_lookup(m, &in, comp, &child, NULL, &type))
            return SYSERR;
        cur = child;
    }
    *m_out   = m;
    *ino_out = cur;
    return OK;
}

int xfsResolve(const char *path, struct xmount **m_out, uint32_t *ino_out)
{
    char abs[XFS_PATH_MAX];
    if (OK != xfsAbspath(path, abs)) return SYSERR;
    return resolve_abs(abs, m_out, ino_out);
}

/* Split path into dir-part + leaf, then resolve dir-part. */
static int resolve_parent(const char *path, struct xmount **m_out,
                          uint32_t *parent_out, char *leaf_out)
{
    char abs[XFS_PATH_MAX];
    char *slash;
    int i;

    if (OK != xfsAbspath(path, abs)) return SYSERR;
    if (abs[0] != '/' || abs[1] == 0) return SYSERR;        /* root has no parent */

    slash = NULL;
    for (i = 0; abs[i]; i++) if (abs[i] == '/') slash = &abs[i];
    if (slash == NULL) return SYSERR;
    strlcpy(leaf_out, slash + 1, XFS_MAX_NAME + 1);
    if (slash == abs) { abs[1] = 0; }   /* parent is "/" */
    else              { *slash  = 0; }
    return resolve_abs(abs, m_out, parent_out);
}

/* ---------- mkfs / mount ---------- */

int xfsMkfs(const char *devname, const char *volname)
{
    int dev;
    struct xblkdev bd;
    struct xsuper  sb;
    uint8_t buf[XFS_BLOCK_SIZE];
    struct xinode root;
    uint32_t inode_count, ibmap_blks, dbmap_blks, itbl_blks, data_blks;

    dev = getdev(devname);
    if (SYSERR == dev) return SYSERR;
    if (OK != control(dev, XFS_BD_CTRL_GETDEV, (long)&bd, 0)) return SYSERR;
    if (bd.block_size != XFS_BLOCK_SIZE) return SYSERR;

    /* Sizing heuristic: 1 inode per 16 KB of disk, capped at 16384. */
    inode_count = bd.nblocks * XFS_BLOCK_SIZE / (16 * 1024);
    if (inode_count < 64)    inode_count = 64;
    if (inode_count > 16384) inode_count = 16384;

    ibmap_blks = (inode_count + (XFS_BLOCK_SIZE * 8) - 1)
               / (XFS_BLOCK_SIZE * 8);
    itbl_blks  = (inode_count * sizeof(struct xinode)
                  + XFS_BLOCK_SIZE - 1) / XFS_BLOCK_SIZE;
    /* Provisional data bitmap size based on remaining blocks. */
    {
        uint32_t header = 1 + ibmap_blks + 1 /*tentative dbmap*/ + itbl_blks;
        if (header >= bd.nblocks) return SYSERR;
        data_blks  = bd.nblocks - header;
        dbmap_blks = (data_blks + (XFS_BLOCK_SIZE * 8) - 1)
                   / (XFS_BLOCK_SIZE * 8);
        header     = 1 + ibmap_blks + dbmap_blks + itbl_blks;
        if (header >= bd.nblocks) return SYSERR;
        data_blks  = bd.nblocks - header;
    }

    memset(&sb, 0, sizeof(sb));
    sb.magic            = XFS_MAGIC;
    sb.block_size       = XFS_BLOCK_SIZE;
    sb.total_blocks     = bd.nblocks;
    sb.inode_count      = inode_count;
    sb.inode_bmap_blk   = 1;
    sb.inode_bmap_nblks = ibmap_blks;
    sb.data_bmap_blk    = 1 + ibmap_blks;
    sb.data_bmap_nblks  = dbmap_blks;
    sb.inode_tbl_blk    = 1 + ibmap_blks + dbmap_blks;
    sb.inode_tbl_nblks  = itbl_blks;
    sb.data_start_blk   = 1 + ibmap_blks + dbmap_blks + itbl_blks;
    sb.data_blocks      = data_blks;
    sb.root_inode       = XFS_ROOT_INO;
    sb.free_inodes      = inode_count - 2;       /* 0 reserved, 1 = root */
    sb.free_blocks      = data_blks;
    if (volname) strlcpy(sb.volname, volname, sizeof(sb.volname));

    /* Zero everything in the metadata area. */
    memset(buf, 0, sizeof(buf));
    {
        uint32_t b, end = sb.data_start_blk;
        for (b = 0; b < end; b++)
            if (OK != bd.write_block(bd.priv, b, buf)) return SYSERR;
    }

    /* Write the superblock. */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &sb, sizeof(sb));
    if (OK != bd.write_block(bd.priv, 0, buf)) return SYSERR;

    /* Mark inode 0 + root inode as used. */
    {
        struct xmount tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.bd = bd;
        tmp.super = sb;
        if (OK != bmap_set(&tmp, sb.inode_bmap_blk, 0, 1)) return SYSERR;
        if (OK != bmap_set(&tmp, sb.inode_bmap_blk, XFS_ROOT_INO, 1))
            return SYSERR;

        /* Initialize the root directory. */
        memset(&root, 0, sizeof(root));
        root.mode  = XFS_MODE_DIR;
        root.nlink = 2;
        if (OK != inode_write(&tmp, XFS_ROOT_INO, &root)) return SYSERR;

        /* Insert "." and ".." */
        if (OK != dir_insert(&tmp, XFS_ROOT_INO, ".",  XFS_ROOT_INO,
                             XFS_T_DIR)) return SYSERR;
        if (OK != dir_insert(&tmp, XFS_ROOT_INO, "..", XFS_ROOT_INO,
                             XFS_T_DIR)) return SYSERR;

        /* Persist updated superblock (free_blocks/inodes were touched). */
        memset(buf, 0, sizeof(buf));
        memcpy(buf, &tmp.super, sizeof(tmp.super));
        if (OK != bd.write_block(bd.priv, 0, buf)) return SYSERR;
    }
    return OK;
}

int xfsMount(const char *devname, const char *path)
{
    int dev, i;
    struct xblkdev bd;
    uint8_t buf[XFS_BLOCK_SIZE];
    struct xsuper sb;
    struct xmount *m = NULL;

    dev = getdev(devname);
    if (SYSERR == dev) return SYSERR;
    if (OK != control(dev, XFS_BD_CTRL_GETDEV, (long)&bd, 0)) return SYSERR;
    if (OK != bd.read_block(bd.priv, 0, buf)) return SYSERR;
    memcpy(&sb, buf, sizeof(sb));
    if (sb.magic != XFS_MAGIC) return SYSERR;

    wait(xfs_lock);
    for (i = 0; i < XFS_MAX_MOUNTS; i++)
        if (!xfs_mtab[i].in_use) { m = &xfs_mtab[i]; break; }
    if (m == NULL) { signal(xfs_lock); return SYSERR; }

    m->in_use = 1;
    strlcpy(m->mountpoint, path, sizeof(m->mountpoint));
    m->super = sb;
    m->bd    = bd;
    signal(xfs_lock);
    return OK;
}

int xfsUmount(const char *path)
{
    int i;
    wait(xfs_lock);
    for (i = 0; i < XFS_MAX_MOUNTS; i++)
    {
        if (xfs_mtab[i].in_use &&
            0 == strcmp(xfs_mtab[i].mountpoint, path))
        {
            xfs_mtab[i].in_use = 0;
            signal(xfs_lock);
            return OK;
        }
    }
    signal(xfs_lock);
    return SYSERR;
}

/* ---------- public file API ---------- */

int xfsOpen(const char *path, int flags)
{
    char abs[XFS_PATH_MAX];
    char leaf[XFS_MAX_NAME + 1];
    struct xmount *m;
    uint32_t ino, parent;
    int fd, r;
    struct xinode in;

    if (OK != xfsAbspath(path, abs)) return SYSERR;

    r = resolve_abs(abs, &m, &ino);
    if (r == SYSERR)
    {
        if (!(flags & XFS_O_CREAT)) return SYSERR;
        if (OK != resolve_parent(path, &m, &parent, leaf)) return SYSERR;
        if (OK != inode_alloc(m, XFS_MODE_FILE, &ino)) return SYSERR;
        if (OK != dir_insert(m, parent, leaf, ino, XFS_T_FILE))
        {
            bmap_set(m, m->super.inode_bmap_blk, ino, 0);
            return SYSERR;
        }
    }

    if (OK != inode_read(m, ino, &in)) return SYSERR;
    if (XFS_MODE_TYPE(in.mode) != XFS_MODE_FILE) return SYSERR;

    if (flags & XFS_O_TRUNC)
    {
        file_truncate(m, &in);
        if (OK != inode_write(m, ino, &in)) return SYSERR;
    }

    wait(xfs_lock);
    for (fd = 0; fd < XFS_MAX_OPEN; fd++)
        if (!xfs_ftab[fd].in_use) break;
    if (fd >= XFS_MAX_OPEN) { signal(xfs_lock); return SYSERR; }

    xfs_ftab[fd].in_use = 1;
    xfs_ftab[fd].flags  = flags;
    xfs_ftab[fd].mnt    = m;
    xfs_ftab[fd].ino    = ino;
    xfs_ftab[fd].node   = in;
    xfs_ftab[fd].pos    = (flags & XFS_O_APPEND) ? in.size : 0;
    signal(xfs_lock);
    return fd;
}

int xfsClose(int fd)
{
    if (fd < 0 || fd >= XFS_MAX_OPEN) return SYSERR;
    if (!xfs_ftab[fd].in_use) return SYSERR;
    if (xfs_ftab[fd].flags & XFS_O_WRONLY)
        inode_write(xfs_ftab[fd].mnt, xfs_ftab[fd].ino, &xfs_ftab[fd].node);
    xfs_ftab[fd].in_use = 0;
    return OK;
}

int xfsRead(int fd, void *buf, uint count)
{
    int r;
    if (fd < 0 || fd >= XFS_MAX_OPEN || !xfs_ftab[fd].in_use) return SYSERR;
    r = inode_read_bytes(xfs_ftab[fd].mnt, &xfs_ftab[fd].node,
                         xfs_ftab[fd].pos, buf, count);
    if (r > 0) xfs_ftab[fd].pos += (uint32_t)r;
    return r;
}

int xfsWrite(int fd, const void *buf, uint count)
{
    int r;
    if (fd < 0 || fd >= XFS_MAX_OPEN || !xfs_ftab[fd].in_use) return SYSERR;
    if (!(xfs_ftab[fd].flags & XFS_O_WRONLY)) return SYSERR;
    r = inode_write_bytes(xfs_ftab[fd].mnt, &xfs_ftab[fd].node,
                          xfs_ftab[fd].pos, buf, count);
    if (r > 0) xfs_ftab[fd].pos += (uint32_t)r;
    inode_write(xfs_ftab[fd].mnt, xfs_ftab[fd].ino, &xfs_ftab[fd].node);
    return r;
}

int xfsSeek(int fd, int offset, int whence)
{
    int newpos;
    if (fd < 0 || fd >= XFS_MAX_OPEN || !xfs_ftab[fd].in_use) return SYSERR;
    switch (whence)
    {
    case XFS_SEEK_SET: newpos = offset;                                 break;
    case XFS_SEEK_CUR: newpos = (int)xfs_ftab[fd].pos + offset;         break;
    case XFS_SEEK_END: newpos = (int)xfs_ftab[fd].node.size + offset;   break;
    default: return SYSERR;
    }
    if (newpos < 0) return SYSERR;
    xfs_ftab[fd].pos = (uint32_t)newpos;
    return newpos;
}

int xfsMkdir(const char *path)
{
    char leaf[XFS_MAX_NAME + 1];
    struct xmount *m;
    uint32_t parent, ino;
    struct xinode dirinode;

    if (OK != resolve_parent(path, &m, &parent, leaf)) return SYSERR;
    if (OK != inode_alloc(m, XFS_MODE_DIR, &ino)) return SYSERR;
    if (OK != inode_read(m, ino, &dirinode)) return SYSERR;
    dirinode.nlink = 2;
    if (OK != inode_write(m, ino, &dirinode))
    {
        bmap_set(m, m->super.inode_bmap_blk, ino, 0);
        return SYSERR;
    }
    if (OK != dir_insert(m, ino, ".",  ino,    XFS_T_DIR)) return SYSERR;
    if (OK != dir_insert(m, ino, "..", parent, XFS_T_DIR)) return SYSERR;
    if (OK != dir_insert(m, parent, leaf, ino, XFS_T_DIR)) return SYSERR;
    return OK;
}

int xfsRmdir(const char *path)
{
    char leaf[XFS_MAX_NAME + 1];
    struct xmount *m;
    uint32_t parent, ino;
    struct xinode in;

    if (OK != resolve_parent(path, &m, &parent, leaf)) return SYSERR;
    {
        struct xinode pin;
        if (OK != inode_read(m, parent, &pin)) return SYSERR;
        if (SYSERR == dir_iter_lookup(m, &pin, leaf, &ino, NULL, NULL))
            return SYSERR;
    }
    if (OK != inode_read(m, ino, &in)) return SYSERR;
    if (XFS_MODE_TYPE(in.mode) != XFS_MODE_DIR) return SYSERR;
    if (1 != dir_empty(m, ino)) return SYSERR;
    if (OK != dir_remove(m, parent, leaf)) return SYSERR;
    file_truncate(m, &in);
    inode_write(m, ino, &in);
    bmap_set(m, m->super.inode_bmap_blk, ino, 0);
    m->super.free_inodes++;
    return OK;
}

int xfsUnlink(const char *path)
{
    char leaf[XFS_MAX_NAME + 1];
    struct xmount *m;
    uint32_t parent, ino;
    struct xinode pin, in;

    if (OK != resolve_parent(path, &m, &parent, leaf)) return SYSERR;
    if (OK != inode_read(m, parent, &pin)) return SYSERR;
    if (SYSERR == dir_iter_lookup(m, &pin, leaf, &ino, NULL, NULL))
        return SYSERR;
    if (OK != inode_read(m, ino, &in)) return SYSERR;
    if (XFS_MODE_TYPE(in.mode) != XFS_MODE_FILE) return SYSERR;
    if (OK != dir_remove(m, parent, leaf)) return SYSERR;
    file_truncate(m, &in);
    inode_write(m, ino, &in);
    bmap_set(m, m->super.inode_bmap_blk, ino, 0);
    m->super.free_inodes++;
    return OK;
}

int xfsTouch(const char *path)
{
    int fd = xfsOpen(path, XFS_O_RDWR | XFS_O_CREAT);
    if (fd < 0) return SYSERR;
    return xfsClose(fd);
}

int xfsRename(const char *oldpath, const char *newpath)
{
    char oldleaf[XFS_MAX_NAME + 1];
    char newleaf[XFS_MAX_NAME + 1];
    struct xmount *m1, *m2;
    uint32_t op, np, ino;
    struct xinode opin;
    uint8_t type;

    if (OK != resolve_parent(oldpath, &m1, &op, oldleaf)) return SYSERR;
    if (OK != resolve_parent(newpath, &m2, &np, newleaf)) return SYSERR;
    if (m1 != m2) return SYSERR;

    if (OK != inode_read(m1, op, &opin)) return SYSERR;
    if (SYSERR == dir_iter_lookup(m1, &opin, oldleaf, &ino, NULL, &type))
        return SYSERR;

    /* If destination already exists and is a regular file, overwrite. */
    {
        struct xinode npin;
        uint32_t old_ino;
        if (OK == inode_read(m2, np, &npin) &&
            OK == dir_iter_lookup(m2, &npin, newleaf, &old_ino, NULL, NULL))
        {
            if (OK != xfsUnlink(newpath)) return SYSERR;
        }
    }

    if (OK != dir_insert(m2, np, newleaf, ino, type)) return SYSERR;
    if (OK != dir_remove(m1, op, oldleaf)) return SYSERR;
    return OK;
}

int xfsStat(const char *path, struct xinode *out, struct xmount **mnt_out)
{
    struct xmount *m;
    uint32_t ino;

    if (OK != xfsResolve(path, &m, &ino)) return SYSERR;
    if (OK != inode_read(m, ino, out))    return SYSERR;
    if (mnt_out) *mnt_out = m;
    return OK;
}

int xfsReaddir(const char *path, uint32_t index, struct xdirent *out,
               uint32_t *next_index)
{
    struct xmount *m;
    uint32_t ino, total, i;
    struct xinode in;
    struct xdirent ent;
    int r;

    if (OK != xfsResolve(path, &m, &ino)) return SYSERR;
    if (OK != inode_read(m, ino, &in)) return SYSERR;
    if (XFS_MODE_TYPE(in.mode) != XFS_MODE_DIR) return SYSERR;

    total = in.size / sizeof(struct xdirent);
    for (i = index; i < total; i++)
    {
        r = inode_read_bytes(m, &in, i * sizeof(ent), &ent, sizeof(ent));
        if (r != (int)sizeof(ent)) return SYSERR;
        if (ent.ino == XFS_NULL_INO) continue;
        *out = ent;
        if (next_index) *next_index = i + 1;
        return OK;
    }
    if (next_index) *next_index = total;
    return SYSERR;
}

/* ---------- cwd ---------- */

int xfsChdir(const char *path)
{
    char abs[XFS_PATH_MAX];
    struct xmount *m;
    uint32_t ino;
    struct xinode in;
    tid_typ tid;

    if (OK != xfsAbspath(path, abs)) return SYSERR;
    if (OK != resolve_abs(abs, &m, &ino)) return SYSERR;
    if (OK != inode_read(m, ino, &in)) return SYSERR;
    if (XFS_MODE_TYPE(in.mode) != XFS_MODE_DIR) return SYSERR;

    tid = gettid();
    if (tid < 0 || tid >= NTHREAD) return SYSERR;
    strlcpy(xfs_cwd_tab[tid], abs, XFS_PATH_MAX);
    return OK;
}

int xfsGetcwd(char *buf, uint size)
{
    const char *cwd = cwd_lookup();
    if (size <= strlen(cwd)) return SYSERR;
    strcpy(buf, cwd);
    return OK;
}

/* ---------- bootstrap ---------- */

int xfsBootstrap(void)
{
    int dev, r;
    struct xblkdev bd;
    uint8_t buf[XFS_BLOCK_SIZE];
    struct xsuper sb;
    int i;

    if (xfs_inited) return OK;
    xfs_lock = semcreate(1);
    for (i = 0; i < NTHREAD; i++) xfs_cwd_tab[i][0] = 0;
    for (i = 0; i < XFS_MAX_MOUNTS; i++) xfs_mtab[i].in_use = 0;
    for (i = 0; i < XFS_MAX_OPEN;   i++) xfs_ftab[i].in_use = 0;
    xfs_inited = 1;

    dev = getdev("RAMDISK0");
    if (SYSERR == dev) return SYSERR;
    if (OK != open(dev)) return SYSERR;
    if (OK != control(dev, XFS_BD_CTRL_GETDEV, (long)&bd, 0)) return SYSERR;

    /* Mkfs if the superblock looks unitialized. */
    r = bd.read_block(bd.priv, 0, buf);
    if (OK != r) return SYSERR;
    memcpy(&sb, buf, sizeof(sb));
    if (sb.magic != XFS_MAGIC)
    {
        if (OK != xfsMkfs("RAMDISK0", "ramfs")) return SYSERR;
    }
    if (OK != xfsMount("RAMDISK0", "/")) return SYSERR;
    return OK;
}
