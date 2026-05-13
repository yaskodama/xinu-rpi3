/**
 * @file ramdiskClose.c
 */

#include <stddef.h>
#include <device.h>
#include <ramdisk.h>

devcall ramdiskClose(device *devptr)
{
    struct ramdisk *rd = &ramdisktab[devptr->minor];
    rd->state = RAMDISK_STATE_FREE;
    return OK;
}
