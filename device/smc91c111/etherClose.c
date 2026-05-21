/**
 * @file etherClose.c
 */
#include "smc91c111.h"
#include <bufpool.h>
#include <ether.h>
#include <interrupt.h>

devcall etherClose(device *devptr)
{
    struct ether    *ethptr;
    struct smc91c111 *chip;
    irqmask im;

    im = disable();

    ethptr = &ethertab[devptr->minor];
    if (ethptr->state == ETH_STATE_FREE || ethptr->state == ETH_STATE_DOWN)
    {
        restore(im);
        return SYSERR;
    }
    chip = ethptr->csr;

    smc_disable(chip);

    if (ethptr->inPool != SYSERR)
    {
        bfpfree(ethptr->inPool);
        ethptr->inPool = SYSERR;
    }

    ethptr->state = ETH_STATE_DOWN;
    restore(im);
    return OK;
}
