/**
 * @file xfs.h
 *
 * Hierarchical in-memory / block-device filesystem for Embedded Xinu.
 */

#ifndef _XFS_H_
#define _XFS_H_

#include <stddef.h>
#include <stdint.h>
#include <kernel.h>

/* ---------- on-disk constants ---------- */

#define XFS_MAGIC        0x58465321u   /* 'X','F','S','!' little-endian */
#define XFS_BLOCK_SIZE   4096
#define XFS_MAX_NAME     56            /* max filename length           */
#define XFS_NDIRECT      12
#define XFS_NIND_PER_BLK (XFS_BLOCK_SIZE / sizeof(uint32_t))
#define XFS_ROOT_INO     1
#define XFS_NULL_INO     0

#define XFS_T_FREE       0
#define XFS_T_FILE       1
#define XFS_T_DIR        2

#define XFS_MODE_FILE    0x8000
#define XFS_MODE_DIR     0x4000
#define XFS_MODE_TYPE(m) ((m) & 0xF000)

/* Inode (exactly 128 bytes) */
struct xinode
{
    uint16_t mode;
    uint16_t nlink;
    uint32_t size;
    uint32_t mtime;
    uint32_t atime;
    uint32_t direct[XFS_NDIRECT];      /* 48 bytes */
    uint32_t indirect;
    uint32_t dindirect;
    uint8_t  pad[56];
};

/* Directory entry (exactly 64 bytes) */
struct xdirent
{
    uint32_t ino;                      /* 0 == empty slot */
    uint8_t  name_len;
    uint8_t  type;                     /* XFS_T_* */
    uint8_t  reserved[2];
    char     name[XFS_MAX_NAME];
};

#define XFS_DIRENTS_PER_BLK (XFS_BLOCK_SIZE / sizeof(struct xdirent))

/* Superblock (occupies block 0; only the head is meaningful, rest is pad) */
struct xsuper
{
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t inode_bmap_blk;
    uint32_t inode_bmap_nblks;
    uint32_t data_bmap_blk;
    uint32_t data_bmap_nblks;
    uint32_t inode_tbl_blk;
    uint32_t inode_tbl_nblks;
    uint32_t data_start_blk;
    uint32_t data_blocks;
    uint32_t root_inode;
    uint32_t free_inodes;
    uint32_t free_blocks;
    char     volname[64];
};

/* ---------- block-device backend ---------- */

struct xblkdev
{
    void *priv;
    int   (*read_block) (void *priv, uint32_t blkno, void *buf);
    int   (*write_block)(void *priv, uint32_t blkno, const void *buf);
    uint32_t nblocks;
    uint32_t block_size;
};

/* Block-device drivers (e.g. ramdisk, future SD) implement this control code:
 * arg1 is a pointer to a struct xblkdev to be filled in. */
#define XFS_BD_CTRL_GETDEV  0x05

/* ---------- runtime mount + open file tables ---------- */

#define XFS_MAX_MOUNTS 4
#define XFS_MAX_OPEN   32
#define XFS_PATH_MAX   256

struct xmount
{
    int            in_use;
    char           mountpoint[XFS_PATH_MAX];
    struct xsuper  super;
    struct xblkdev bd;
};

struct xfile
{
    int           in_use;
    int           flags;
    struct xmount *mnt;
    uint32_t      ino;
    struct xinode node;
    uint32_t      pos;
};

#define XFS_O_RDONLY 0x0001
#define XFS_O_WRONLY 0x0002
#define XFS_O_RDWR   0x0003
#define XFS_O_CREAT  0x0010
#define XFS_O_TRUNC  0x0020
#define XFS_O_APPEND 0x0040

/* ---------- public API ---------- */

/* Boot-time: format an in-memory ramdisk (if not already) and mount at "/". */
int xfsBootstrap(void);

/* Format the block device behind `devname` with an empty xfs. */
int xfsMkfs(const char *devname, const char *volname);

/* Mount/umount: devname is a Xinu device name like "RAMDISK0". */
int xfsMount(const char *devname, const char *path);
int xfsUmount(const char *path);

/* Path resolution helpers (absolute path or relative to cwd). */
int xfsResolve(const char *path, struct xmount **mnt_out, uint32_t *ino_out);

/* File operations. Return >=0 fd or SYSERR. */
int xfsOpen (const char *path, int flags);
int xfsClose(int fd);
int xfsRead (int fd, void *buf, uint count);
int xfsWrite(int fd, const void *buf, uint count);
int xfsSeek (int fd, int offset, int whence);
#define XFS_SEEK_SET 0
#define XFS_SEEK_CUR 1
#define XFS_SEEK_END 2

/* Metadata + namespace ops. */
int xfsMkdir (const char *path);
int xfsRmdir (const char *path);
int xfsUnlink(const char *path);
int xfsTouch (const char *path);
int xfsRename(const char *oldpath, const char *newpath);
int xfsStat  (const char *path, struct xinode *out, struct xmount **mnt_out);

/* Directory enumeration.  index starts at 0; returns OK on hit, SYSERR on
 * end-of-dir or invalid path.  *next_index is set to the next slot to scan. */
int xfsReaddir(const char *path, uint32_t index, struct xdirent *out,
               uint32_t *next_index);

/* Per-thread current working directory (absolute path). */
int  xfsChdir (const char *path);
int  xfsGetcwd(char *buf, uint size);

/* Build an absolute path from a possibly-relative input.  out must be
 * XFS_PATH_MAX bytes. */
int xfsAbspath(const char *in, char *out);

#endif /* _XFS_H_ */
