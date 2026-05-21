/**
 * @file etherRead.c
 */
#include "smc91c111.h"
#include <bufpool.h>
#include <ether.h>
#include <interrupt.h>
#include <stddef.h>
#include <string.h>

devcall etherRead(device *devptr, void *buf, uint len)
{
    struct ether *ethptr;
    struct ethPktBuffer *pkt;
    uint length;
    irqmask im;

    ethptr = &ethertab[devptr->minor];

    im = disable();
    if (ethptr->state != ETH_STATE_UP || len < ETH_HEADER_LEN)
    {
        restore(im);
        return SYSERR;
    }
    restore(im);

    /* Block until the interrupt handler has queued at least one packet */
    wait(ethptr->isema);

    im = disable();
    pkt = ethptr->in[ethptr->istart];
    ethptr->in[ethptr->istart] = NULL;
    ethptr->istart = (ethptr->istart + 1) % ETH_IBLEN;
    ethptr->icount--;
    restore(im);

    if (pkt == NULL)
    {
        return 0;
    }

    length = (uint)pkt->length;
    if (length > len)
    {
        length = len;
    }
    memcpy(buf, pkt->data, length);
    buffree(pkt);

    return length;
}
