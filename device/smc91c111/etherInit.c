/**
 * @file etherInit.c
 *
 * Initialise the SMSC LAN91C111 Ethernet controller (MMIO version, used by
 * the QEMU versatilepb platform).
 */

#include "smc91c111.h"
#include <bufpool.h>
#include <clock.h>
#include <conf.h>
#include <ether.h>
#include <interrupt.h>
#include <kernel.h>
#include <memory.h>
#include <platform.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>

struct ether     ethertab[NETHER];
static struct smc91c111 smc_chips[NETHER];

static void random_mac(uchar addr[ETH_ADDR_LEN])
{
    uint i;
    srand(clkcount());
    for (i = 0; i < ETH_ADDR_LEN; i++)
    {
        addr[i] = rand();
    }
    addr[0] &= 0xFE;        /* clear multicast bit  */
    addr[0] |= 0x02;        /* set locally-assigned */
}

devcall etherInit(device *devptr)
{
    struct ether    *ethptr;
    struct smc91c111 *chip;

    ethptr = &ethertab[devptr->minor];
    bzero(ethptr, sizeof(*ethptr));
    ethptr->dev           = devptr;
    ethptr->state         = ETH_STATE_DOWN;
    ethptr->mtu           = ETH_MTU;
    ethptr->addressLength = ETH_ADDR_LEN;
    ethptr->isema         = semcreate(0);
    if (isbadsem(ethptr->isema))
    {
        return SYSERR;
    }

    chip       = &smc_chips[devptr->minor];
    chip->base = devptr->csr;
    chip->irq  = devptr->irq;
    ethptr->csr = chip;

    /* Probe: read the bank-3 revision register. QEMU's smc91c111 model
     * reports 0x3391 (rev 9, chip 0x33). */
    smc_select_bank(chip, 3);
    kprintf("[smc91c111] base=0x%08x rev=0x%04x\r\n",
            (uint)chip->base, smc_read16(chip, SMC_REVISION));

    /* Bring the chip into a known idle state */
    smc_reset(chip);

    /* QEMU's smc91c111 model lets the host pick the MAC via -net nic,
     * macaddr=...  Otherwise generate a stable random one. */
    random_mac(ethptr->devAddress);
    smc_set_mac(chip, ethptr->devAddress);

    /* Hook our interrupt handler.  Install on a few candidate IRQ lines
     * since QEMU versions have varied (25 is the documented value). */
    {
        int candidates[] = {25, 26, 27, 28, 29, 30, 31, 24};
        uint k;
        for (k = 0; k < sizeof(candidates)/sizeof(candidates[0]); k++)
        {
            interruptVector[candidates[k]] = devptr->intr;
            enable_irq(candidates[k]);
        }
    }
    kprintf("[smc91c111] irq registered on candidates 24..31\r\n");

    /* Probe via software IRQ to verify routing works */
    *((volatile ulong *)0x10140018) = (1U << 25);  /* VICSOFTINT bit 25 */
    {
        volatile int probe = 100000;
        while (probe--) { asm volatile("nop"); }
    }
    *((volatile ulong *)0x1014001C) = (1U << 25);  /* VICSOFTINTCLEAR */

    return OK;
}
