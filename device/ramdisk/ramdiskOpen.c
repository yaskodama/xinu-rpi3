/**
 * @file ramdiskOpen.c
 */

#include <stddef.h>
#include <device.h>
#include <ramdisk.h>

devcall ramdiskOpen(device *devptr, ...)
{
    struct ramdisk *rd = &ramdisktab[devptr->minor];

    if (NULL == rd->buf)
    {
        return SYSERR;
    }
    rd->state = RAMDISK_STATE_ALLOC;
    rd->pos   = 0;
    return OK;
}
