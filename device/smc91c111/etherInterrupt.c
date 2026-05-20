/**
 * @file etherInterrupt.c
 *
 * Interrupt handler for the SMSC LAN91C111.  Drains the RX FIFO (one packet
 * per pass), copies each frame into an ::ethPktBuffer drawn from the inPool
 * and signals waiters on the per-device input semaphore.
 *
 * TX completion is acknowledged but otherwise ignored (auto-release is
 * enabled, so the chip recovers the allocated page on its own).
 */
#include "smc91c111.h"
#include <bufpool.h>
#include <ether.h>
#include <interrupt.h>
#include <kernel.h>
#include <stddef.h>
#include <string.h>

extern int resdefer;

static void rx_one(struct ether *ethptr, struct smc91c111 *chip)
{
    struct ethPktBuffer *pkt;
    ushort hdr_status;
    ushort hdr_count;
    uint   data_len;
    uchar  ctl;
    uint   i;
    ushort w;

    /* Point at the current RX packet, read mode, auto-increment */
    smc_select_bank(chip, 2);
    smc_write16(chip, SMC_PTR, (ushort)(PTR_RCV | PTR_AUTOINCR | PTR_READ));

    hdr_status = smc_read16(chip, SMC_DATA);
    hdr_count  = smc_read16(chip, SMC_DATA);

    /* hdr_count is total length stored: 2 (status) + 2 (count) + payload
     * + 1 (ctl) [+1 pad if even payload].  Subtract overheads to get the
     * MAC frame length; STRIP_CRC is on, so no CRC included. */
    if (hdr_count < 6)
    {
        /* malformed; drop */
        smc_write16(chip, SMC_MMU_CMD, MMU_REMOVE_RELEASE);
        return;
    }
    data_len = hdr_count - 6;

    /* Bad packet or queue full? drop. */
    if ((hdr_status & (RXSTAT_ALIGNERR | RXSTAT_BADCRC |
                       RXSTAT_TOO_LONG | RXSTAT_TOO_SHORT)) ||
        ethptr->icount >= ETH_IBLEN || ethptr->inPool == SYSERR)
    {
        if (ethptr->icount >= ETH_IBLEN)
        {
            ethptr->ovrrun++;
        }
        else
        {
            ethptr->errors++;
        }
        smc_write16(chip, SMC_MMU_CMD, MMU_REMOVE_RELEASE);
        return;
    }

    pkt = bufget(ethptr->inPool);
    if (pkt == (struct ethPktBuffer *)SYSERR || pkt == NULL)
    {
        ethptr->ovrrun++;
        smc_write16(chip, SMC_MMU_CMD, MMU_REMOVE_RELEASE);
        return;
    }
    pkt->buf  = (uchar *)pkt + sizeof(struct ethPktBuffer);
    pkt->data = pkt->buf;
    pkt->length = (int)data_len;

    /* Copy out the frame body (data_len bytes), as 16-bit words */
    for (i = 0; i + 1 < data_len; i += 2)
    {
        w = smc_read16(chip, SMC_DATA);
        pkt->buf[i]     = (uchar)(w & 0xFF);
        pkt->buf[i + 1] = (uchar)(w >> 8);
    }
    /* Trailing control word: bit 5 = ODD; low byte may be last data byte */
    w = smc_read16(chip, SMC_DATA);
    ctl = (uchar)(w >> 8);
    if (ctl & 0x20)
    {
        pkt->buf[data_len] = (uchar)(w & 0xFF);
        pkt->length        = (int)(data_len + 1);
    }

    /* Done with this slot — release it */
    smc_write16(chip, SMC_MMU_CMD, MMU_REMOVE_RELEASE);

    /* Enqueue for etherRead() */
    ethptr->in[(ethptr->istart + ethptr->icount) % ETH_IBLEN] = pkt;
    ethptr->icount++;
    signaln(ethptr->isema, 1);
    ethptr->rxirq++;
}

void etherInterrupt(void)
{
    struct ether     *ethptr = &ethertab[0];
    struct smc91c111 *chip   = ethptr->csr;
    ushort intregs;
    uchar  st, mask;

    /* kprintf("[smc91c111] IRQ\r\n");  -- noisy during fast TCP RX */
    smc_select_bank(chip, 2);

    for (;;)
    {
        intregs = smc_read16(chip, SMC_INT);
        st   = (uchar)(intregs & 0xFF);
        mask = (uchar)(intregs >> 8);
        st  &= mask;
        if (st == 0)
        {
            break;
        }

        /* Receive */
        if (st & INT_RCV)
        {
            rx_one(ethptr, chip);
            /* INT_RCV is level-sensitive on remaining packets; loop  */
            continue;
        }

        /* TX complete (auto-release on; just ack) */
        if (st & INT_TX)
        {
            smc_write16(chip, SMC_INT, INT_TX);
            ethptr->txirq++;
            continue;
        }

        if (st & INT_RX_OVRN)
        {
            ethptr->ovrrun++;
            smc_write16(chip, SMC_INT, INT_RX_OVRN);
            continue;
        }

        /* Any other bits — ack and move on */
        smc_write16(chip, SMC_INT, st);
    }
}
