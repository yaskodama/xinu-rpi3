/**
 * @file ramdiskInit.c
 */

#include <stddef.h>
#include <device.h>
#include <memory.h>
#include <semaphore.h>
#include <string.h>
#include <ramdisk.h>

struct ramdisk ramdisktab[NRAMDISK];

devcall ramdiskInit(device *devptr)
{
    struct ramdisk *rd = &ramdisktab[devptr->minor];
    void *buf;

    rd->state      = RAMDISK_STATE_FREE;
    rd->size       = RAMDISK_SIZE;
    rd->block_size = RAMDISK_BLOCK_SIZE;
    rd->nblocks    = RAMDISK_SIZE / RAMDISK_BLOCK_SIZE;
    rd->pos        = 0;

    buf = memget(rd->size);
    if ((void *)SYSERR == buf)
    {
        rd->buf = NULL;
        return SYSERR;
    }
    rd->buf = (uint8_t *)buf;
    memset(rd->buf, 0, rd->size);

    rd->lock = semcreate(1);
    return OK;
}
