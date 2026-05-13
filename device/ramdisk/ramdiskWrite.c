/**
 * @file ramdiskWrite.c
 */

#include <stddef.h>
#include <device.h>
#include <semaphore.h>
#include <string.h>
#include <ramdisk.h>

devcall ramdiskWrite(device *devptr, const void *buf, uint len)
{
    struct ramdisk *rd = &ramdisktab[devptr->minor];
    uint avail;

    if (NULL == rd->buf)
    {
        return SYSERR;
    }

    wait(rd->lock);
    if (rd->pos >= rd->size)
    {
        signal(rd->lock);
        return 0;
    }
    avail = rd->size - rd->pos;
    if (len > avail)
    {
        len = avail;
    }
    memcpy(rd->buf + rd->pos, buf, len);
    rd->pos += len;
    signal(rd->lock);
    return len;
}
