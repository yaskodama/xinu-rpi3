/**
 * @file sd_block.c
 *
 * Single-block read for the Raspberry Pi 3 (BCM2837) SD card.
 *
 * IMPORTANT Pi 3 wiring (see apps/wifi.c): the SD CARD is on the Broadcom
 * "SDHOST" controller at 0x3F202000 (GPIO48-53).  The Arasan SDHCI "EMMC"
 * block at 0x3F300000 is wired to the *WiFi* chip's SDIO bus, NOT the card.
 * This driver therefore talks to SDHOST.  (The earlier version was ported
 * straight from the Pi 4, where the card is on EMMC2, and consequently read
 * the WiFi SDIO host instead of the card — every sd_read_block failed.)
 *
 * The firmware bootloader already brought the card up on SDHOST to load
 * kernel.img (a live register read shows SDCDIV=9, the last command was
 * CMD17/READ, SDHSTS clean), so we reuse that state: no clock/power/CMD0/
 * CMD2/3/7 — just issue CMD17 and drain the 512-byte FIFO.  WiFi lives on
 * the separate EMMC block, so this never conflicts with it.
 */

#include "sd_block.h"

#define SDHOST_BASE        0x3F202000UL

#define SDCMD   (*(volatile unsigned int *)(SDHOST_BASE + 0x00))  /* command  */
#define SDARG   (*(volatile unsigned int *)(SDHOST_BASE + 0x04))  /* argument */
#define SDTOUT  (*(volatile unsigned int *)(SDHOST_BASE + 0x08))  /* timeout  */
#define SDCDIV  (*(volatile unsigned int *)(SDHOST_BASE + 0x0C))  /* clk div  */
#define SDRSP0  (*(volatile unsigned int *)(SDHOST_BASE + 0x10))  /* response */
#define SDHSTS  (*(volatile unsigned int *)(SDHOST_BASE + 0x20))  /* status   */
#define SDVDD   (*(volatile unsigned int *)(SDHOST_BASE + 0x30))  /* power    */
#define SDEDM   (*(volatile unsigned int *)(SDHOST_BASE + 0x34))  /* FIFO FSM */
#define SDHCFG  (*(volatile unsigned int *)(SDHOST_BASE + 0x38))  /* host cfg */
#define SDHBCT  (*(volatile unsigned int *)(SDHOST_BASE + 0x3C))  /* byte cnt */
#define SDDATA  (*(volatile unsigned int *)(SDHOST_BASE + 0x40))  /* FIFO     */
#define SDHBLC  (*(volatile unsigned int *)(SDHOST_BASE + 0x50))  /* blk cnt  */

#define SDCMD_NEW_FLAG       0x8000
#define SDCMD_FAIL_FLAG      0x4000
#define SDCMD_READ_CMD       0x0040
#define SDCMD_WRITE_CMD      0x0080

#define SDHSTS_REW_TIME_OUT  0x80
#define SDHSTS_CMD_TIME_OUT  0x40
#define SDHSTS_CRC16_ERROR   0x20
#define SDHSTS_CRC7_ERROR    0x10
#define SDHSTS_FIFO_ERROR    0x08
#define SDHSTS_DATA_FLAG     0x01
#define SDHSTS_ERR_MASK \
    (SDHSTS_REW_TIME_OUT | SDHSTS_CMD_TIME_OUT | SDHSTS_CRC16_ERROR | \
     SDHSTS_CRC7_ERROR | SDHSTS_FIFO_ERROR)
/* W1C status/IRPT bits to clear before a command (BUSY|BLOCK|SDIO|errors). */
#define SDHSTS_CLEAR         0x07F8

#define POLL_LIMIT           2000000UL

#define SDVDD_POWER_ON       0x01

/* The firmware powers the SDHOST host block DOWN after loading kernel.img
 * (a live read shows SDVDD=0, and a fresh CMD17 leaves SDCMD's NEW_FLAG set
 * with no timeout — i.e. no clock is going out).  The card itself keeps its
 * 3.3V rail and stays selected, so re-asserting host power + clearing status
 * is enough to make commands flow again.  Done once, lazily. */
static int sd_kicked;
static void sd_hw_kick(void)
{
    volatile unsigned long d;
    if (sd_kicked) return;
    SDVDD  = SDVDD_POWER_ON;                 /* re-enable host power/clock   */
    SDHSTS = SDHSTS_CLEAR;
    for (d = 0; d < 200000; d++) { }         /* let the clock settle         */
    sd_kicked = 1;
}

int sd_read_block(unsigned long lba, void *buf)
{
    unsigned int *p = (unsigned int *)buf;
    unsigned long t;
    int i;

    sd_hw_kick();

    /* wait for the controller to be idle, then clear stale status */
    for (t = 0; t < POLL_LIMIT; t++)
        if (!(SDCMD & SDCMD_NEW_FLAG)) break;
    SDHSTS = SDHSTS_CLEAR;

    /* one 512-byte block at @lba (SDHC/SDXC use block addressing) */
    SDHBCT = SD_BLOCK_SIZE;
    SDHBLC = 1;
    SDARG  = (unsigned int)lba;
    SDCMD  = 17u | SDCMD_READ_CMD | SDCMD_NEW_FLAG;     /* CMD17 */

    for (t = 0; t < POLL_LIMIT; t++)                    /* command accepted? */
        if (!(SDCMD & SDCMD_NEW_FLAG)) break;
    if (SDCMD & (SDCMD_NEW_FLAG | SDCMD_FAIL_FLAG)) return -1;

    /* drain 128 words as the data FIFO fills */
    for (i = 0; i < SD_BLOCK_SIZE / 4; i++)
    {
        int ready = 0;
        for (t = 0; t < POLL_LIMIT; t++)
        {
            unsigned int hs = SDHSTS;
            if (hs & SDHSTS_ERR_MASK) return -1;
            if (hs & SDHSTS_DATA_FLAG) { ready = 1; break; }
        }
        if (!ready) return -1;
        p[i] = SDDATA;
    }
    if (SDHSTS & SDHSTS_ERR_MASK) return -1;
    return 0;
}

int sd_init(void)
{
    unsigned char block[SD_BLOCK_SIZE] __attribute__((aligned(4)));
    if (sd_read_block(0, block) != 0) return -1;
    if (block[510] != 0x55 || block[511] != 0xAA) return -1;   /* MBR sig */
    return 0;
}

/* Writing is not needed for `ls /microsd` and is left unimplemented on the
 * SDHOST path to avoid risking the card the kernel boots from. */
int sd_write_block(unsigned long lba, const void *buf)
{
    (void)lba; (void)buf;
    return -1;
}
