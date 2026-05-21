/**
 * @file etherOpen.c
 */
#include "smc91c111.h"
#include <bufpool.h>
#include <ether.h>
#include <interrupt.h>

devcall etherOpen(device *devptr)
{
    struct ether    *ethptr;
    struct smc91c111 *chip;
    irqmask im;
    int retval = SYSERR;

    im = disable();

    ethptr = &ethertab[devptr->minor];
    if (ethptr->state != ETH_STATE_DOWN)
    {
        goto out;
    }
    chip = ethptr->csr;

    /* Buffer pool for received packets queued to etherRead() */
    ethptr->inPool = bfpalloc(sizeof(struct ethPktBuffer) + ETH_MAX_PKT_LEN,
                              ETH_IBLEN);
    if (ethptr->inPool == SYSERR)
    {
        goto out;
    }

    /* The chip has its own 8KB on-board RAM for TX, so no outPool. */
    ethptr->outPool = SYSERR;

    /* Apply MAC (may have changed since init) and enable RX/TX */
    smc_set_mac(chip, ethptr->devAddress);
    smc_enable(chip);

    ethptr->state = ETH_STATE_UP;
    retval = OK;

    /* QEMU の smc91c111 模倣では IRQ ラインがアサートされない事があり、
     * その場合 RX が永遠に放置される.5ms 周期で etherInterrupt() を
     * 呼ぶ poller スレッドを立ち上げて回避. (etherPoll.c) */
    {
        extern void smc_spawn_poller(void);
        smc_spawn_poller();
    }

out:
    restore(im);
    return retval;
}
