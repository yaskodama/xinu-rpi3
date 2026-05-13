/**
 * @file ramdisk.h
 *
 * RAM-backed block device for Embedded Xinu.  Used as the default backing
 * store for xfs.  Exposes both a sequential char-device interface (for
 * ordinary read()/write()/seek()) and a block interface via control() that
 * the filesystem layer drives directly.
 */

#ifndef _RAMDISK_H_
#define _RAMDISK_H_

#include <device.h>
#include <stddef.h>
#include <stdint.h>
#include <semaphore.h>
#include <xfs.h>

#ifndef RAMDISK_SIZE
/* Default RAM disk: 16 MB.  Fits comfortably in any RPi memory budget. */
#define RAMDISK_SIZE   (16 * 1024 * 1024)
#endif

#define RAMDISK_BLOCK_SIZE  XFS_BLOCK_SIZE

/* control() functions */
#define RAMDISK_CTRL_BREAD   0x01      /* arg1=blkno, arg2=buf*  */
#define RAMDISK_CTRL_BWRITE  0x02      /* arg1=blkno, arg2=buf*  */
#define RAMDISK_CTRL_NBLOCKS 0x03      /* return total blocks    */
#define RAMDISK_CTRL_BLKSIZE 0x04      /* return block size      */
#define RAMDISK_CTRL_GETDEV  XFS_BD_CTRL_GETDEV  /* fill xblkdev, arg1=ptr */

/* RAM disk control block (one per minor). */
struct ramdisk
{
    int       state;
    uint8_t  *buf;          /* backing store         */
    uint32_t  size;         /* total bytes           */
    uint32_t  nblocks;      /* size / block_size     */
    uint32_t  block_size;   /* RAMDISK_BLOCK_SIZE    */
    uint32_t  pos;          /* current byte position */
    semaphore lock;
};

#define RAMDISK_STATE_FREE   0
#define RAMDISK_STATE_ALLOC  1

extern struct ramdisk ramdisktab[];

/* Driver entry points (registered through xinu.conf). */
devcall ramdiskInit   (device *);
devcall ramdiskOpen   (device *, ...);
devcall ramdiskClose  (device *);
devcall ramdiskRead   (device *, void *, uint);
devcall ramdiskWrite  (device *, const void *, uint);
devcall ramdiskSeek   (device *, long);
devcall ramdiskControl(device *, int, long, long);

/* Helper: fill an xblkdev backend from a Xinu device descriptor. */
int ramdiskGetBlkDev(int devnum, struct xblkdev *out);

#endif /* _RAMDISK_H_ */
