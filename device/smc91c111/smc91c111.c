/**
 * @file smc91c111.c
 *
 * Low-level operations for the SMSC LAN91C111 Ethernet controller on the
 * QEMU versatilepb platform.  The chip is a 16-bit memory-mapped device
 * organised as four register banks selected through the Bank Select Register.
 */

#include "smc91c111.h"
#include <clock.h>
#include <kernel.h>
#include <stddef.h>

/* MMIO accessors.  All registers are at 16-bit-aligned offsets; the data
 * port also supports 32-bit access on this platform. */

void smc_select_bank(struct smc91c111 *chip, int bank)
{
    *((volatile ushort *)((ulong)chip->base + SMC_BANK)) = (ushort)(bank & 3);
}

ushort smc_read16(struct smc91c111 *chip, int off)
{
    return *((volatile ushort *)((ulong)chip->base + off));
}

void smc_write16(struct smc91c111 *chip, int off, ushort val)
{
    *((volatile ushort *)((ulong)chip->base + off)) = val;
}

ulong smc_read32(struct smc91c111 *chip, int off)
{
    return *((volatile ulong *)((ulong)chip->base + off));
}

void smc_write32(struct smc91c111 *chip, int off, ulong val)
{
    *((volatile ulong *)((ulong)chip->base + off)) = val;
}

/* Software-reset the chip.  Returns OK on success, SYSERR on timeout. */
int smc_reset(struct smc91c111 *chip)
{
    int i;

    /* Soft reset via RCR.SOFT_RST */
    smc_select_bank(chip, 0);
    smc_write16(chip, SMC_RCR, RCR_SOFT_RST);
    /* tiny settle — datasheet asks for at least 10us */
    for (i = 0; i < 1000; i++)
    {
        asm volatile ("nop");
    }
    smc_write16(chip, SMC_RCR, 0);

    /* Disable TX and RX while we configure */
    smc_write16(chip, SMC_TCR, 0);

    /* Reset MMU (clears RX FIFO and TX queues) */
    smc_select_bank(chip, 2);
    smc_write16(chip, SMC_MMU_CMD, MMU_RESET);

    /* Mask all interrupts (high byte of SMC_INT) */
    smc_write16(chip, SMC_INT, 0x0000);

    /* Bank 1: auto-release Tx packets (saves an MMU command per send) */
    smc_select_bank(chip, 1);
    smc_write16(chip, SMC_CTL, smc_read16(chip, SMC_CTL) | CTL_AUTO_RELEASE);

    /* Bank 0: enable auto-negotiation so QEMU SLIRP shows link-up */
    smc_select_bank(chip, 0);
    smc_write16(chip, SMC_RPCR,
                (ushort)(RPCR_ANEG |
                         (RPCR_LED_LINK  << RPCR_LED_SHIFT) |
                         (RPCR_LED_TX_RX << (RPCR_LED_SHIFT + 3))));

    return OK;
}

/* Program the chip's individual address (MAC). */
void smc_set_mac(struct smc91c111 *chip, const uchar mac[ETH_ADDR_LEN])
{
    smc_select_bank(chip, 1);
    smc_write16(chip, SMC_IA0_1, (ushort)(mac[0] | (mac[1] << 8)));
    smc_write16(chip, SMC_IA2_3, (ushort)(mac[2] | (mac[3] << 8)));
    smc_write16(chip, SMC_IA4_5, (ushort)(mac[4] | (mac[5] << 8)));
}

/* Enable RX + TX paths. */
void smc_enable(struct smc91c111 *chip)
{
    smc_select_bank(chip, 0);
    smc_write16(chip, SMC_TCR, TCR_TXENA | TCR_PAD_EN);
    smc_write16(chip, SMC_RCR, RCR_RXEN | RCR_STRIP_CRC);

    /* Enable RX and ALLOC interrupts (TX status comes via INT_TX) */
    smc_select_bank(chip, 2);
    smc_write16(chip, SMC_INT,
                (ushort)((INT_RCV | INT_TX | INT_RX_OVRN) << 8));
}

/* Disable RX + TX. */
void smc_disable(struct smc91c111 *chip)
{
    smc_select_bank(chip, 2);
    smc_write16(chip, SMC_INT, 0);

    smc_select_bank(chip, 0);
    smc_write16(chip, SMC_TCR, 0);
    smc_write16(chip, SMC_RCR, 0);
}
