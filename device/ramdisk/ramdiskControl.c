/**
 * @file ramdiskControl.c
 *
 * Block-level access for the filesystem layer plus a backend-fill helper.
 */

#include <stddef.h>
#include <device.h>
#include <semaphore.h>
#include <string.h>
#include <ramdisk.h>
#include <xfs.h>

static int rd_read_block(void *priv, uint32_t blkno, void *buf)
{
    struct ramdisk *rd = (struct ramdisk *)priv;

    if (NULL == rd->buf || blkno >= rd->nblocks)
    {
        return SYSERR;
    }
    wait(rd->lock);
    memcpy(buf, rd->buf + (uint32_t)blkno * rd->block_size, rd->block_size);
    signal(rd->lock);
    return OK;
}

static int rd_write_block(void *priv, uint32_t blkno, const void *buf)
{
    struct ramdisk *rd = (struct ramdisk *)priv;

    if (NULL == rd->buf || blkno >= rd->nblocks)
    {
        return SYSERR;
    }
    wait(rd->lock);
    memcpy(rd->buf + (uint32_t)blkno * rd->block_size, buf, rd->block_size);
    signal(rd->lock);
    return OK;
}

devcall ramdiskControl(device *devptr, int func, long arg1, long arg2)
{
    struct ramdisk *rd = &ramdisktab[devptr->minor];
    void *p;

    if (NULL == rd->buf)
    {
        return SYSERR;
    }

    switch (func)
    {
    case RAMDISK_CTRL_BREAD:
        return rd_read_block(rd, (uint32_t)arg1, (void *)arg2);

    case RAMDISK_CTRL_BWRITE:
        return rd_write_block(rd, (uint32_t)arg1, (const void *)arg2);

    case RAMDISK_CTRL_NBLOCKS:
        return (devcall)rd->nblocks;

    case RAMDISK_CTRL_BLKSIZE:
        return (devcall)rd->block_size;

    case RAMDISK_CTRL_GETDEV:
    {
        struct xblkdev *bd = (struct xblkdev *)arg1;
        if (NULL == bd) return SYSERR;
        bd->priv        = rd;
        bd->read_block  = rd_read_block;
        bd->write_block = rd_write_block;
        bd->nblocks     = rd->nblocks;
        bd->block_size  = rd->block_size;
        return OK;
    }

    default:
        return SYSERR;
    }
    (void)p;
}

int ramdiskGetBlkDev(int devnum, struct xblkdev *out)
{
    if (devnum < 0 || devnum >= NDEVS || out == NULL)
    {
        return SYSERR;
    }
    return control(devnum, RAMDISK_CTRL_GETDEV, (long)out, 0);
}
