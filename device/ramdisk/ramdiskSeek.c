/**
 * @file ramdiskSeek.c
 */

#include <stddef.h>
#include <device.h>
#include <semaphore.h>
#include <ramdisk.h>

devcall ramdiskSeek(device *devptr, long pos)
{
    struct ramdisk *rd = &ramdisktab[devptr->minor];

    if (NULL == rd->buf)
    {
        return SYSERR;
    }
    if (pos < 0 || (uint32_t)pos > rd->size)
    {
        return SYSERR;
    }
    wait(rd->lock);
    rd->pos = (uint32_t)pos;
    signal(rd->lock);
    return OK;
}
