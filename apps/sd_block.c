/**
 * @file sd_block.c
 *
 * Single-block read for the Raspberry Pi 3 (BCM2837) SD card on the Arasan
 * SDHCI "EMMC" controller at 0x3F300000.
 *
 * Pi 3 wiring (confirmed from the live GPIO function-selects + Linux dts):
 * the SD card is routed to the **Arasan EMMC** controller via GPIO48-53 in
 * ALT3 (dts `emmc_gpio48` = ALT3).  ALT0 (`sdhost_gpio48`) would route the
 * card to the Broadcom SDHOST block instead, but this board uses ALT3.  The
 * same EMMC controller is shared (muxed by GPIO) with the WiFi SDIO bus
 * (GPIO34-39 ALT3), so `ls /microsd` works only while WiFi is down.
 *
 * The firmware reads kernel.img through this controller but then DISABLES its
 * clock (CONTROL1=0) before handing off, which deselects the card.  So unlike
 * the Pi 4 (where firmware leaves EMMC2 hot), here we must re-init: reset the
 * host, restart the clock at identify speed, and re-enumerate the card
 * (CMD0/8/ACMD41/CMD2/CMD3/CMD7/CMD16) before CMD17 reads work.  This whole
 * sequence was first verified live over /mmio-write (CMD8 -> 0x1AA, ACMD41 ->
 * 0xc0ff8000 SDHC-ready, CMD3 -> RCA 0xaaaa, select OK).
 */

#include "sd_block.h"

#define EMMC_BASE            0x3F300000UL

#define EMMC_ARG2          (*(volatile unsigned int *)(EMMC_BASE + 0x00))
#define EMMC_BLKSIZECNT    (*(volatile unsigned int *)(EMMC_BASE + 0x04))
#define EMMC_ARG1          (*(volatile unsigned int *)(EMMC_BASE + 0x08))
#define EMMC_CMDTM         (*(volatile unsigned int *)(EMMC_BASE + 0x0C))
#define EMMC_RESP0         (*(volatile unsigned int *)(EMMC_BASE + 0x10))
#define EMMC_DATA          (*(volatile unsigned int *)(EMMC_BASE + 0x20))
#define EMMC_STATUS        (*(volatile unsigned int *)(EMMC_BASE + 0x24))
#define EMMC_CONTROL0      (*(volatile unsigned int *)(EMMC_BASE + 0x28))
#define EMMC_CONTROL1      (*(volatile unsigned int *)(EMMC_BASE + 0x2C))
#define EMMC_INTERRUPT     (*(volatile unsigned int *)(EMMC_BASE + 0x30))
#define EMMC_IRPT_MASK     (*(volatile unsigned int *)(EMMC_BASE + 0x34))
#define EMMC_IRPT_EN       (*(volatile unsigned int *)(EMMC_BASE + 0x38))

/* INTERRUPT bits */
#define INT_CMD_DONE       0x00000001
#define INT_DATA_DONE      0x00000002
#define INT_READ_RDY       0x00000020
#define INT_ERROR_MASK     0xFFFF8000u

/* CONTROL1 bits */
#define C1_CLK_INTLEN      0x00000001   /* internal clock enable            */
#define C1_CLK_STABLE      0x00000002
#define C1_CLK_EN          0x00000004   /* SD clock enable                  */
#define C1_SRST_HC         0x01000000   /* reset the complete host          */

/* CMDTM command encodings (index<<24 | resp-type<<16 | flags), verified live */
#define CMD_GO_IDLE        0x00000000   /* CMD0,  no response               */
#define CMD_SEND_IF_COND   0x08020000   /* CMD8,  R7                        */
#define CMD_APP_CMD        0x37020000   /* CMD55, R1                        */
#define CMD_SD_SENDOPCOND  0x29020000   /* ACMD41,R3 (no CRC/idx check)     */
#define CMD_ALL_SEND_CID   0x02010000   /* CMD2,  R2 (136-bit)              */
#define CMD_SEND_REL_ADDR  0x03020000   /* CMD3,  R6                        */
#define CMD_SELECT_CARD    0x07030000   /* CMD7,  R1b                       */
#define CMD_SET_BLOCKLEN   0x10020000   /* CMD16, R1                        */
#define CMD_READ_SINGLE    0x113A0010   /* CMD17, R1 + data card->host      */

#define POLL_LIMIT         3000000UL

static int sd_inited;

/* Diagnostics surfaced by the /sd-test route so a failing init reports the
 * exact step + controller state instead of just rc=-1. */
volatile int          sd_dbg_step;     /* last init stage reached      */
volatile unsigned int sd_dbg_int;      /* INTERRUPT at that point      */
volatile unsigned int sd_dbg_resp;     /* RESP0 at that point          */
int          sd_get_step(void) { return sd_dbg_step; }
unsigned int sd_get_int(void)  { return sd_dbg_int; }
unsigned int sd_get_resp(void) { return sd_dbg_resp; }
#define DBG(s) do { sd_dbg_step = (s); sd_dbg_int = EMMC_INTERRUPT; sd_dbg_resp = EMMC_RESP0; } while (0)

static int wait_int(unsigned int mask)
{
    unsigned long t;
    for (t = 0; t < POLL_LIMIT; t++)
    {
        unsigned int v = EMMC_INTERRUPT;
        if (v & INT_ERROR_MASK) { EMMC_INTERRUPT = v; return -1; }
        if (v & mask)           { EMMC_INTERRUPT = mask; return 0; }
    }
    return -1;
}

/* STATUS bits: CMD line / DAT line busy ("inhibit"). */
#define ST_CMD_INHIBIT     0x00000001
#define ST_DAT_INHIBIT     0x00000002

/* Spin until the controller will accept a new command (the previous one's
 * CMD and DAT lines are free).  Required: issuing back-to-back commands
 * without this drops the second one — the bug that made every read fail past
 * CMD8 even though each command works on its own. */
static int sd_wait_ready(void)
{
    unsigned long t;
    for (t = 0; t < POLL_LIMIT; t++)
        if (!(EMMC_STATUS & (ST_CMD_INHIBIT | ST_DAT_INHIBIT))) return 0;
    return -1;
}

/* Issue a command (no data phase) and wait for it to complete. */
static int sd_cmd(unsigned int cmdtm, unsigned int arg)
{
    if (sd_wait_ready() != 0) return -1;
    EMMC_INTERRUPT = 0xFFFFFFFFu;
    EMMC_ARG1 = arg;
    EMMC_CMDTM = cmdtm;
    return wait_int(INT_CMD_DONE);
}

/* Bring the EMMC controller back up and re-enumerate the card.  Returns 0 on
 * success.  Idempotent. */
int sd_init(void)
{
    unsigned long t;
    unsigned int rca;

    if (sd_inited) return 0;
    DBG(1);

    /* Reset the whole host, wait for the reset bit to self-clear. */
    EMMC_CONTROL1 = C1_SRST_HC;
    for (t = 0; t < POLL_LIMIT; t++)
        if (!(EMMC_CONTROL1 & C1_SRST_HC)) break;
    if (EMMC_CONTROL1 & C1_SRST_HC) return -1;

    /* Identify-speed clock: data timeout 0xe, divider field 0x80
     * (~250MHz/1024 ≈ 244 kHz), internal clock enable; wait stable; enable
     * the SD clock.  (Live-verified value 0xe0085.) */
    EMMC_CONTROL1 = 0x000E0081u;
    for (t = 0; t < POLL_LIMIT; t++)
        if (EMMC_CONTROL1 & C1_CLK_STABLE) break;
    if (!(EMMC_CONTROL1 & C1_CLK_STABLE)) return -1;
    EMMC_CONTROL1 |= C1_CLK_EN;
    for (t = 0; t < 200000; t++) { }            /* let the card settle */

    /* Enable every event in the Interrupt *Status* register (0x34) so the
     * INTERRUPT register (0x30) actually latches CMD_DONE / READ_RDY / errors
     * for polling.  Without this it reads 0 forever and every wait_int times
     * out.  Leave the Signal-enable (0x38) at 0 — we poll, no CPU IRQ. */
    EMMC_IRPT_EN = 0;
    EMMC_IRPT_MASK = 0xFFFFFFFFu;
    EMMC_INTERRUPT = 0xFFFFFFFFu;
    DBG(2);                                       /* reset+clock done     */

    sd_cmd(CMD_GO_IDLE, 0);                       /* CMD0 (no response)   */
    DBG(3);
    if (sd_cmd(CMD_SEND_IF_COND, 0x1AA) != 0) { DBG(40); return -1; }   /* CMD8 */
    DBG(4);
    if (EMMC_RESP0 != 0x1AA) { DBG(41); return -1; }   /* must echo pattern */

    /* ACMD41: repeat CMD55 + CMD41 (HCS=1) until OCR busy bit (31) sets. */
    for (t = 0; t < 100000; t++)
    {
        if (sd_cmd(CMD_APP_CMD, 0) != 0) { DBG(50); return -1; }
        if (sd_cmd(CMD_SD_SENDOPCOND, 0x40FF8000u) != 0) { DBG(60); return -1; }
        if (EMMC_RESP0 & 0x80000000u) break;      /* card powered up      */
        { unsigned long d; for (d = 0; d < 50000; d++) { } }
    }
    if (!(EMMC_RESP0 & 0x80000000u)) { DBG(61); return -1; }
    DBG(7);

    if (sd_cmd(CMD_ALL_SEND_CID, 0) != 0) { DBG(80); return -1; }     /* CMD2  */
    DBG(8);
    if (sd_cmd(CMD_SEND_REL_ADDR, 0) != 0) { DBG(90); return -1; }    /* CMD3  */
    rca = EMMC_RESP0 & 0xFFFF0000u;
    DBG(9);
    if (sd_cmd(CMD_SELECT_CARD, rca) != 0) { DBG(100); return -1; }   /* CMD7  */
    DBG(10);
    if (sd_cmd(CMD_SET_BLOCKLEN, SD_BLOCK_SIZE) != 0) { DBG(110); return -1; } /* CMD16 */
    DBG(11);

    sd_inited = 1;
    return 0;
}

int sd_read_block(unsigned long lba, void *buf)
{
    unsigned int *p = (unsigned int *)buf;
    int i;

    if (!sd_inited && sd_init() != 0) return -1;

    if (sd_wait_ready() != 0) return -1;
    EMMC_INTERRUPT = 0xFFFFFFFFu;
    EMMC_BLKSIZECNT = (1u << 16) | SD_BLOCK_SIZE;     /* 1 block of 512 B   */
    EMMC_ARG1 = (unsigned int)lba;                     /* SDHC: block addr  */
    EMMC_CMDTM = CMD_READ_SINGLE;                      /* CMD17             */

    if (wait_int(INT_CMD_DONE) != 0) return -1;
    if (wait_int(INT_READ_RDY) != 0) return -1;
    for (i = 0; i < SD_BLOCK_SIZE / 4; i++) p[i] = EMMC_DATA;
    if (wait_int(INT_DATA_DONE) != 0) return -1;
    return 0;
}

/* Writing is not needed for `ls /microsd`; left unimplemented to avoid any
 * risk to the card the kernel boots from. */
int sd_write_block(unsigned long lba, const void *buf)
{
    (void)lba; (void)buf;
    return -1;
}
