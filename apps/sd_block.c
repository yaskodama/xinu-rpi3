/**
 * @file sd_block.c
 *
 * SDHCI v3 single-block read/write for BCM2837 (Raspberry Pi 3).
 *
 * Ported from xinu-rpi4 device/sd/sd.c with two changes:
 *   1. SD_BASE adjusted from 0xFE340000 (BCM2711 EMMC2) to 0x3F300000
 *      (BCM2837 EMMC).
 *   2. Added sd_write_block (CMD24, WRITE_SINGLE_BLOCK).
 *
 * The firmware bootloader read kernel.img off the card to boot us, which
 * means the EMMC controller is already at high speed, the card is already
 * selected, and block-address mode is in effect (SDHC/SDXC).  We do not
 * touch clocks, power, CMD0, CMD2/3, CMD7, etc. — just issue CMD17
 * (read) or CMD24 (write) against the already-selected card.
 */

#include "sd_block.h"

/* BCM2837 EMMC base.  Pi 4 / BCM2711 uses the wholly separate EMMC2 at
 * 0xFE340000; Pi 3 / BCM2837 only has the legacy single EMMC at
 * peripheral_base(0x3F000000) + 0x300000. */
#define SD_BASE              0x3F300000UL

#define EMMC_ARG2          (*(volatile unsigned int *)(SD_BASE + 0x00))
#define EMMC_BLKSIZECNT    (*(volatile unsigned int *)(SD_BASE + 0x04))
#define EMMC_ARG1          (*(volatile unsigned int *)(SD_BASE + 0x08))
#define EMMC_CMDTM         (*(volatile unsigned int *)(SD_BASE + 0x0C))
#define EMMC_RESP0         (*(volatile unsigned int *)(SD_BASE + 0x10))
#define EMMC_DATA          (*(volatile unsigned int *)(SD_BASE + 0x20))
#define EMMC_STATUS        (*(volatile unsigned int *)(SD_BASE + 0x24))
#define EMMC_INTERRUPT     (*(volatile unsigned int *)(SD_BASE + 0x30))

/* CMDTM bit fields per SDHCI spec.
 *   bits 24-31 = command index
 *   bit  21    = data direction transfer flag (ISDATA)
 *   bit  20    = check response index against CMD index
 *   bit  19    = check response CRC7
 *   bits 16-17 = response type (10 = 48-bit, 00 = none)
 *   bit  4     = data transfer direction (0 = host->card, 1 = card->host)
 */
#define CMDTM_CMD17_READ \
    ((17u << 24) | (1u << 21) | (1u << 20) | (1u << 19) | (2u << 16) | (1u << 4))
#define CMDTM_CMD24_WRITE \
    ((24u << 24) | (1u << 21) | (1u << 20) | (1u << 19) | (2u << 16))

#define INT_CMD_DONE       (1u << 0)
#define INT_DATA_DONE      (1u << 1)
#define INT_WRITE_RDY      (1u << 4)
#define INT_READ_RDY       (1u << 5)
#define INT_ERROR_MASK     0xFFFF8000u

#define POLL_LIMIT         5000000UL

static int wait_intr(unsigned int flag)
{
    for (unsigned long t = 0; t < POLL_LIMIT; t++)
    {
        unsigned int v = EMMC_INTERRUPT;
        if (v & INT_ERROR_MASK)
        {
            return -1;
        }
        if (v & flag)
        {
            EMMC_INTERRUPT = flag;          /* W1C — clear that flag only */
            return 0;
        }
    }
    return -1;
}

int sd_init(void)
{
    unsigned char block[SD_BLOCK_SIZE];
    if (sd_read_block(0, block) != 0)
    {
        return -1;
    }
    /* MBR ends with 0x55 0xAA at offset 510 */
    if (block[510] != 0x55 || block[511] != 0xAA)
    {
        return -1;
    }
    return 0;
}

int sd_read_block(unsigned long lba, void *buf)
{
    EMMC_INTERRUPT = 0xFFFFFFFFu;
    EMMC_BLKSIZECNT = (1u << 16) | SD_BLOCK_SIZE;
    EMMC_ARG1 = (unsigned int)lba;
    EMMC_CMDTM = CMDTM_CMD17_READ;

    if (wait_intr(INT_CMD_DONE) != 0) return -1;
    if (wait_intr(INT_READ_RDY) != 0) return -1;

    unsigned int *p = (unsigned int *)buf;
    int i;
    for (i = 0; i < SD_BLOCK_SIZE / 4; i++)
    {
        p[i] = EMMC_DATA;
    }
    if (wait_intr(INT_DATA_DONE) != 0) return -1;
    return 0;
}

int sd_write_block(unsigned long lba, const void *buf)
{
    EMMC_INTERRUPT = 0xFFFFFFFFu;
    EMMC_BLKSIZECNT = (1u << 16) | SD_BLOCK_SIZE;
    EMMC_ARG1 = (unsigned int)lba;
    EMMC_CMDTM = CMDTM_CMD24_WRITE;

    if (wait_intr(INT_CMD_DONE) != 0) return -1;
    /* For writes the controller signals it's ready to ACCEPT data via
     * INT_WRITE_RDY (bit 4), not INT_READ_RDY (bit 5).  After we push
     * 128 words the controller transfers them to the card and asserts
     * INT_DATA_DONE. */
    if (wait_intr(INT_WRITE_RDY) != 0) return -1;

    const unsigned int *p = (const unsigned int *)buf;
    int i;
    for (i = 0; i < SD_BLOCK_SIZE / 4; i++)
    {
        EMMC_DATA = p[i];
    }
    if (wait_intr(INT_DATA_DONE) != 0) return -1;
    return 0;
}
