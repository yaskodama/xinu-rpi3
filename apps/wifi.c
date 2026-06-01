/**
 * @file wifi.c
 *
 * Raspberry Pi 3 B+ on-board WiFi (Cypress/Broadcom BCM43455) bring-up.
 *
 * STAGE 1+2 ONLY — SDIO host controller + chip power-on + SDIO enumeration +
 * backplane chip-ID readback.  This does NOT download firmware (Stage 3) and
 * does NOT join any network.  The goal of this stage is a single, verifiable
 * milestone: power the WiFi chip, talk to it over SDIO, and read back its
 * silicon chip-ID (expected 0x4345 for the BCM43455 on the Pi 3 B+).
 *
 * Hardware facts (Pi 3 / BCM2837), confirmed against raspberrypi/linux:
 *   - The Pi 3 has TWO SD controllers.  The microSD CARD is on the custom
 *     "sdhost" controller (0x3F202000, GPIO48-53).  The Arasan SDHCI-compatible
 *     "emmc" controller (0x3F300000) is wired to the WiFi chip's SDIO bus on
 *     GPIO34-39 (ALT3).  So the EMMC register block we drive here is the WiFi
 *     SDIO host, NOT the card. (apps/sd_block.c targeted the same block but
 *     wrongly assumed the card was on it — that is why it never worked.)
 *   - WiFi power (WL_REG_ON) is NOT a SoC GPIO; it is expander GPIO 1 on the
 *     VideoCore firmware GPIO expander.  dts: wifi_pwrseq reset-gpios =
 *     <&expgpio 1 ACTIVE_LOW>, and expgpio names pin 1 "WL_ON".  The expander
 *     base in the firmware mailbox numbering is 128, so WL_ON = firmware GPIO
 *     129.  We assert it via the property mailbox SET_GPIO_STATE tag.
 *
 * Reference: raspberrypi/linux drivers/net/wireless/.../brcmfmac/{sdio,chip,
 * bcmsdh}.c and drivers/mmc/host/sdhci*.c (cloned to ~/projects/rpi-linux-ref).
 *
 * ★ Many constants below need on-hardware confirmation; every step emits a
 * kprintf marker "[wifi] ..." so the serial console shows exactly how far the
 * bring-up gets.  This is deliberate — the deploy loop (SD swap + power cycle)
 * is slow, so the first boot must be maximally diagnostic.
 */

#include <kernel.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <clock.h>
#include <thread.h>

/* ------------------------------------------------------------------ *
 *  MMIO bases (BCM2837, peripheral base 0x3F000000)                  *
 * ------------------------------------------------------------------ */
#define WIFI_GPIO_BASE     0x3F200000UL
#define WIFI_EMMC_BASE     0x3F300000UL   /* Arasan SDHCI = WiFi SDIO host */
#define WIFI_MBOX_BASE     0x3F00B880UL   /* VideoCore mailbox             */

/* GPIO function-select registers (each GPFSELn covers 10 pins, 3 bits each) */
#define GPFSEL3            (*(volatile uint32_t *)(WIFI_GPIO_BASE + 0x0C)) /* GPIO30-39 */
#define GPPUD              (*(volatile uint32_t *)(WIFI_GPIO_BASE + 0x94))
#define GPPUDCLK0          (*(volatile uint32_t *)(WIFI_GPIO_BASE + 0x98))
#define GPPUDCLK1          (*(volatile uint32_t *)(WIFI_GPIO_BASE + 0x9C)) /* GPIO32-53 */

/* ------------------------------------------------------------------ *
 *  Arasan SDHCI register block (SDHCI v3 layout)                     *
 * ------------------------------------------------------------------ */
#define EMMC_ARG2          (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x00))
#define EMMC_BLKSIZECNT    (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x04))
#define EMMC_ARG1          (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x08))
#define EMMC_CMDTM         (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x0C))
#define EMMC_RESP0         (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x10))
#define EMMC_RESP1         (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x14))
#define EMMC_RESP2         (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x18))
#define EMMC_RESP3         (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x1C))
#define EMMC_DATA          (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x20))
#define EMMC_STATUS        (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x24))
#define EMMC_CONTROL0      (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x28))
#define EMMC_CONTROL1      (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x2C))
#define EMMC_INTERRUPT     (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x30))
#define EMMC_IRPT_MASK     (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x34))
#define EMMC_IRPT_EN       (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x38))
#define EMMC_CONTROL2      (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0x3C))
#define EMMC_SLOTISR_VER   (*(volatile uint32_t *)(WIFI_EMMC_BASE + 0xFC))

/* STATUS bits */
#define SR_CMD_INHIBIT     (1u << 0)
#define SR_DAT_INHIBIT     (1u << 1)

/* CONTROL1 bits */
#define C1_CLK_INTLEN      (1u << 0)   /* internal clock enable             */
#define C1_CLK_STABLE      (1u << 1)
#define C1_CLK_EN          (1u << 2)   /* clock enable to the card/chip     */
#define C1_TOUNIT_MAX      (0xeu << 16)
#define C1_SRST_HC         (1u << 24)  /* reset complete host circuit       */
#define C1_SRST_CMD        (1u << 25)
#define C1_SRST_DATA       (1u << 26)

/* INTERRUPT bits (also used as IRPT_EN/MASK) */
#define INT_CMD_DONE       (1u << 0)
#define INT_DATA_DONE      (1u << 1)
#define INT_WRITE_RDY      (1u << 4)
#define INT_READ_RDY       (1u << 5)
#define INT_ERR            0xFFFF8000u

/* CMDTM response-type encodings (bits 16-17) + flags */
#define CMD_RSPNS_NONE     (0u << 16)
#define CMD_RSPNS_136      (1u << 16)
#define CMD_RSPNS_48       (2u << 16)
#define CMD_RSPNS_48BUSY   (3u << 16)
#define CMD_CRCCHK_EN      (1u << 19)
#define CMD_IXCHK_EN       (1u << 20)
#define CMD_ISDATA         (1u << 21)
#define TM_DAT_DIR_CH      (1u << 4)   /* card -> host (read)               */

/* ------------------------------------------------------------------ *
 *  SDIO command set + register addresses (SDIO simplified spec)      *
 * ------------------------------------------------------------------ */
#define SD_CMD0_GO_IDLE         0
#define SD_CMD3_SEND_RCA        3
#define SD_CMD5_IO_OP_COND      5
#define SD_CMD7_SELECT          7
#define SD_CMD52_IO_RW_DIRECT   52
#define SD_CMD53_IO_RW_EXT      53

/* CCCR (function 0) registers */
#define SDIO_CCCR_IOEx          0x02   /* I/O function enable               */
#define SDIO_CCCR_IORx          0x03   /* I/O function ready                */
#define SDIO_CCCR_BUS_IF        0x07   /* bus width                         */

/* Broadcom F1 backplane window registers (brcmfmac sdio.h) */
#define SBSDIO_FUNC1_SBADDRLOW  0x1000A
#define SBSDIO_FUNC1_SBADDRMID  0x1000B
#define SBSDIO_FUNC1_SBADDRHIGH 0x1000C
#define SBSDIO_SB_OFT_ADDR_MASK 0x07FFF
#define SBSDIO_SB_ACCESS_2_4B   0x08000  /* 32-bit access flag in the offset */
#define SBSDIO_SBWINDOW_MASK    0xFFFF8000u

/* Backplane: chipcommon enumeration base + chip-id register (offset 0) */
#define SI_ENUM_BASE            0x18000000u
#define CID_ID_MASK             0x0000FFFFu
#define BCM43455_CHIP_ID        0x4345     /* what we hope to read back      */

#define WIFI_GPIO_WL_ON_FW      129        /* expander pin 1 + base 128      */

/* ------------------------------------------------------------------ *
 *  tiny busy-wait (Xinu sleep() is ms-granular; we need sub-ms too)  *
 * ------------------------------------------------------------------ */
static void wifi_udelay(volatile unsigned long n)
{
    while (n--) { asm volatile("nop"); }
}

/* ================================================================== *
 *  VideoCore property mailbox (channel 8) — used to assert WL_REG_ON *
 * ================================================================== */
#define MBOX_FULL   0x80000000u
#define MBOX_EMPTY  0x40000000u
#define MBOX_CHAN_PROP 8

static volatile uint32_t *const mbox = (volatile uint32_t *)WIFI_MBOX_BASE;
/* register word indices within the 0x3F00B880 block (see bcm2835_power.c) */
#define MBOX_READ   0
#define MBOX_STATUS 6
#define MBOX_WRITE  8

/* 16-byte-aligned property buffer (mailbox requires the low 4 bits clear). */
static volatile uint32_t mbox_buf[36] __attribute__((aligned(16)));

static uint32_t wifi_phys_to_bus(volatile void *p)
{
    /* Xinu runs with D-cache off; hand the GPU the uncached bus alias so it
     * reads our writes from RAM directly (same rule as screenInit.c). */
    return ((uint32_t)(unsigned long)p & 0x3FFFFFFFu) | 0xC0000000u;
}

/* Generic property-mailbox request.  `tag` is the property tag; `in`/`n_in`
 * are the request value words (copied into the value buffer); on return up to
 * `n_out` response value words are copied back into `out`.  vbuf_bytes is the
 * value buffer size (max of request/response value sizes).  Returns 0 on a
 * success response code. */
static int wifi_mbox_prop(uint32_t tag, const uint32_t *in, int n_in,
                          uint32_t *out, int n_out, int vbuf_bytes)
{
    volatile uint32_t *b = mbox_buf;
    uint32_t r;
    int i, hdr = 5;     /* words [0..4] are the header, value starts at [5] */

    b[0] = (hdr + (vbuf_bytes / 4) + 1) * 4;   /* total buffer size (bytes) */
    b[1] = 0;                                  /* request                   */
    b[2] = tag;
    b[3] = vbuf_bytes;                         /* value buffer size (bytes) */
    b[4] = 0;                                  /* req/resp code             */
    for (i = 0; i < n_in; i++)  b[hdr + i] = in[i];
    for (i = n_in; i < vbuf_bytes / 4; i++) b[hdr + i] = 0;
    b[hdr + vbuf_bytes / 4] = 0;               /* end tag                   */

    while (mbox[MBOX_STATUS] & MBOX_FULL) { }
    mbox[MBOX_WRITE] = (wifi_phys_to_bus(b) & ~0xFu) | MBOX_CHAN_PROP;
    do {
        while (mbox[MBOX_STATUS] & MBOX_EMPTY) { }
        r = mbox[MBOX_READ];
    } while ((r & 0xF) != MBOX_CHAN_PROP);

    for (i = 0; i < n_out; i++) out[i] = b[hdr + i];
    return (b[1] == 0x80000000u) ? 0 : -1;     /* response success code */
}

/* Set a VideoCore-expander GPIO state (SET_GPIO_STATE).  state 1 = on. */
static int wifi_fw_set_gpio(uint32_t gpio, uint32_t state)
{
    uint32_t in[2] = { gpio, state };
    return wifi_mbox_prop(0x00038041, in, 2, NULL, 0, 8);
}

/* Read back a VideoCore-expander GPIO state (GET_GPIO_STATE). */
static int wifi_fw_get_gpio(uint32_t gpio, uint32_t *state)
{
    uint32_t in[2] = { gpio, 0 }, out[2] = { 0, 0 };
    int rc = wifi_mbox_prop(0x00030041, in, 1, out, 2, 8);
    if (state) *state = out[1];
    return rc;
}

/* Enable/disable a firmware-managed clock (SET_CLOCK_STATE). */
static int wifi_fw_set_clock_state(uint32_t clk_id, uint32_t on)
{
    uint32_t in[2] = { clk_id, on & 1 }, out[2] = { 0, 0 };
    return wifi_mbox_prop(0x00038001, in, 2, out, 2, 8);
}

/* Query a firmware-managed clock's current rate in Hz (GET_CLOCK_RATE). */
static uint32_t wifi_fw_get_clock_rate(uint32_t clk_id)
{
    uint32_t in[1] = { clk_id }, out[2] = { 0, 0 };
    if (wifi_mbox_prop(0x00030002, in, 1, out, 2, 8) != 0)
        return 0;
    return out[1];   /* out[0] = clock id echoed, out[1] = rate in Hz */
}

#define RPI_FW_EMMC_CLK_ID  1

/* ================================================================== *
 *  GPIO 34-39 -> ALT3 (SDIO bus to the WiFi chip)                    *
 * ================================================================== */
static void wifi_gpio_sdio(void)
{
    uint32_t sel = GPFSEL3;
    int pin;
    /* GPIO34-39 live in GPFSEL3; pin p uses bits [3*(p-30) .. +2].
     * ALT3 = 0b111. */
    for (pin = 34; pin <= 39; pin++) {
        int sh = 3 * (pin - 30);
        sel &= ~(7u << sh);
        sel |=  (7u << sh);   /* ALT3 == 7 */
    }
    GPFSEL3 = sel;

    /* SDIO line pulls (dts sdio_pins brcm,pull = <0 2 2 2 2 2>):
     *   GPIO34 = SDIO CLK   -> no pull
     *   GPIO35 = SDIO CMD   -> pull-up
     *   GPIO36-39 = DAT0-3  -> pull-up
     * Without the CMD/DAT pull-ups the lines float and CMD5 never sees a
     * response (the bring-up's first CMD5 timeout). */
    /* pull-UP (GPPUD=2) on GPIO35-39 = GPPUDCLK1 bits 3..7 */
    GPPUD = 2;
    wifi_udelay(300);
    GPPUDCLK1 = (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);
    wifi_udelay(300);
    GPPUD = 0;
    GPPUDCLK1 = 0;
    /* no pull (GPPUD=0) on GPIO34 = GPPUDCLK1 bit 2 */
    GPPUD = 0;
    wifi_udelay(300);
    GPPUDCLK1 = (1u << 2);
    wifi_udelay(300);
    GPPUD = 0;
    GPPUDCLK1 = 0;
}

/* ================================================================== *
 *  SDHCI host controller bring-up                                    *
 * ================================================================== */
/* Compute the SDHCI 10-bit "divided clock" divisor that yields <= target Hz
 * from the given base clock: SDCLK = base / (2 * divisor). */
static uint32_t emmc_divisor(uint32_t base_hz, uint32_t target_hz)
{
    uint32_t d = 1;
    if (target_hz == 0) target_hz = 400000;
    while ((base_hz / (2 * d)) > target_hz && d < 1023)
        d++;
    return d;
}

static int emmc_reset_clock(uint32_t divisor)
{
    uint32_t c1;
    int t;

    /* full host-circuit reset */
    EMMC_CONTROL1 |= C1_SRST_HC;
    for (t = 0; t < 100000; t++) {
        if (!(EMMC_CONTROL1 & C1_SRST_HC)) break;
        wifi_udelay(100);
    }
    if (EMMC_CONTROL1 & C1_SRST_HC) {
        kprintf("[wifi]   SRST_HC did not clear (controller dead?)\r\n");
        return -1;
    }

    /* program the SD clock: 8-bit divided-clock mode (SDHCI v3 uses the
     * low 8 bits of the divisor in [15:8] plus the upper 2 bits in [7:6]). */
    c1 = EMMC_CONTROL1;
    c1 &= ~0xFFE0u;                       /* clear CLK_EN | DIV | GENSEL */
    c1 |= C1_CLK_INTLEN | C1_TOUNIT_MAX;
    c1 |= (divisor & 0xFF) << 8;
    c1 |= ((divisor >> 8) & 0x3) << 6;
    EMMC_CONTROL1 = c1;

    for (t = 0; t < 100000; t++) {
        if (EMMC_CONTROL1 & C1_CLK_STABLE) break;
        wifi_udelay(100);
    }
    if (!(EMMC_CONTROL1 & C1_CLK_STABLE)) {
        kprintf("[wifi]   internal clock never stabilised\r\n");
        return -1;
    }
    EMMC_CONTROL1 |= C1_CLK_EN;            /* gate clock to the chip */
    wifi_udelay(20000);
    return 0;
}

static int emmc_host_init(void)
{
    uint32_t ver, base, div;

    /* The Arasan EMMC needs its base clock running before its registers are
     * meaningful.  Ask the firmware to power the EMMC clock on and tell us its
     * rate, then derive the SDHCI divisor for a ~400 kHz SDIO init clock.
     * (This was the likely cause of the first CMD5 timeout — no bus clock.) */
    wifi_fw_set_clock_state(RPI_FW_EMMC_CLK_ID, 1);
    base = wifi_fw_get_clock_rate(RPI_FW_EMMC_CLK_ID);
    kprintf("[wifi] EMMC base clock = %u Hz\r\n", base);
    if (base == 0) {
        base = 250000000u;   /* fallback to a common Pi default */
        kprintf("[wifi]   (base=0 from firmware, assuming %u)\r\n", base);
    }

    ver = EMMC_SLOTISR_VER;
    kprintf("[wifi] SDHCI HC version=0x%02x (SLOTISR_VER=0x%08x)\r\n",
            (ver >> 16) & 0xFF, ver);

    div = emmc_divisor(base, 400000);
    kprintf("[wifi] SDIO init clock divisor=%u (~%u Hz)\r\n",
            div, base / (2 * div));
    if (emmc_reset_clock(div) != 0)
        return -1;

    /* enable all normal-interrupt status bits (polled, not IRQ) */
    EMMC_IRPT_EN   = 0xFFFFFFFFu;
    EMMC_IRPT_MASK = 0xFFFFFFFFu;
    EMMC_INTERRUPT = 0xFFFFFFFFu;          /* clear stale */
    kprintf("[wifi] SDHCI host initialised (clock on)\r\n");
    return 0;
}

/* Issue one SDIO command.  resp_flags selects the response encoding; data!=0
 * means a 4-byte CMD53 read into *data.  Returns 0 on success. */
static int emmc_cmd(uint32_t cmd_idx, uint32_t arg, uint32_t resp_flags,
                    uint32_t *resp_out, int read_4byte, uint32_t *data)
{
    uint32_t cmd, intr, t;

    /* wait for the command line to be free */
    for (t = 0; t < 1000000; t++) {
        if (!(EMMC_STATUS & SR_CMD_INHIBIT)) break;
    }
    EMMC_INTERRUPT = 0xFFFFFFFFu;

    if (read_4byte) {
        EMMC_BLKSIZECNT = (1u << 16) | 4;   /* 1 block of 4 bytes */
    }

    EMMC_ARG1  = arg;
    cmd = (cmd_idx << 24) | resp_flags;
    if (read_4byte)
        cmd |= CMD_ISDATA | TM_DAT_DIR_CH;
    EMMC_CMDTM = cmd;

    /* wait for CMD_DONE or error */
    for (t = 0; t < 1000000; t++) {
        intr = EMMC_INTERRUPT;
        if (intr & INT_ERR)      { kprintf("[wifi]   cmd%d err intr=0x%08x\r\n", cmd_idx, intr); return -1; }
        if (intr & INT_CMD_DONE) { EMMC_INTERRUPT = INT_CMD_DONE; break; }
    }
    if (t >= 1000000) { kprintf("[wifi]   cmd%d timeout (no CMD_DONE)\r\n", cmd_idx); return -1; }

    if (resp_out) *resp_out = EMMC_RESP0;

    if (read_4byte) {
        for (t = 0; t < 1000000; t++) {
            intr = EMMC_INTERRUPT;
            if (intr & INT_ERR)       { kprintf("[wifi]   cmd%d data err 0x%08x\r\n", cmd_idx, intr); return -1; }
            if (intr & INT_READ_RDY)  { EMMC_INTERRUPT = INT_READ_RDY; break; }
        }
        if (t >= 1000000) { kprintf("[wifi]   cmd%d no READ_RDY\r\n", cmd_idx); return -1; }
        if (data) *data = EMMC_DATA;
        for (t = 0; t < 1000000; t++) {
            if (EMMC_INTERRUPT & INT_DATA_DONE) { EMMC_INTERRUPT = INT_DATA_DONE; break; }
        }
    }
    return 0;
}

/* CMD52: single-byte direct register access.  write!=0 -> write `val`.
 * Returns the read/echoed byte (0..255) or -1 on error. */
static int sdio_cmd52(int func, uint32_t addr, int write, uint32_t val)
{
    uint32_t arg, resp;
    arg = ((uint32_t)(write & 1) << 31) |
          ((uint32_t)(func & 7) << 28)  |
          (write ? (1u << 27) : 0)      |   /* RAW: read-after-write */
          ((addr & 0x1FFFF) << 9)       |
          (val & 0xFF);
    if (emmc_cmd(SD_CMD52_IO_RW_DIRECT, arg, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN,
                 &resp, 0, NULL) != 0)
        return -1;
    return (int)(resp & 0xFF);
}

/* CMD53 4-byte read at backplane (function-1) offset `f1off`. */
static int sdio_cmd53_read32(uint32_t f1off, uint32_t *out)
{
    uint32_t arg;
    /* byte mode, function 1, increment, count=4 */
    arg = (0u << 31) |              /* read                 */
          (1u << 28) |              /* function 1           */
          (0u << 27) |              /* byte (not block) mode*/
          (1u << 26) |              /* increment address    */
          ((f1off & 0x1FFFF) << 9) |
          (4u & 0x1FF);             /* 4 bytes              */
    return emmc_cmd(SD_CMD53_IO_RW_EXT, arg,
                    CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, NULL, 1, out);
}

/* Point the F1 backplane window at `addr` (brcmf_sdiod_set_backplane_window). */
static int sdio_set_window(uint32_t addr)
{
    uint32_t bar0 = addr & SBSDIO_SBWINDOW_MASK;
    uint32_t v = bar0 >> 8;
    int i;
    for (i = 0; i < 3; i++, v >>= 8) {
        if (sdio_cmd52(1, SBSDIO_FUNC1_SBADDRLOW + i, 1, v & 0xFF) < 0)
            return -1;
    }
    return 0;
}

/* Read a 32-bit backplane register at absolute address `addr`. */
static int wifi_backplane_read32(uint32_t addr, uint32_t *out)
{
    uint32_t off;
    if (sdio_set_window(addr) != 0)
        return -1;
    off = (addr & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B;
    return sdio_cmd53_read32(off, out);
}

/* ================================================================== *
 *  Public: power on the chip and probe its SDIO identity + chip-id.  *
 * ================================================================== */
int wifi_probe(void)
{
    uint32_t ocr = 0, rca = 0, chipid = 0;
    int r, ioe;

    kprintf("[wifi] === BCM43455 SDIO bring-up (Stage 1+2) ===\r\n");

    /* 1. power-cycle the chip via WL_REG_ON (firmware expander gpio 129):
     *    drive reset asserted (0), settle, then deasserted (1), and read it
     *    back to confirm the firmware actually drove the expander pin. */
    {
        uint32_t st = 0xDEAD;
        kprintf("[wifi] WL_REG_ON reset cycle (fw gpio %d)...\r\n", WIFI_GPIO_WL_ON_FW);
        wifi_fw_set_gpio(WIFI_GPIO_WL_ON_FW, 0);   /* hold in reset */
        sleep(50);
        if (wifi_fw_set_gpio(WIFI_GPIO_WL_ON_FW, 1) != 0)  /* release */
            kprintf("[wifi]   warning: SET_GPIO_STATE response not OK\r\n");
        sleep(150);                                 /* regulators + LPO settle */
        if (wifi_fw_get_gpio(WIFI_GPIO_WL_ON_FW, &st) == 0)
            kprintf("[wifi] WL_REG_ON readback state=%u (expect 1)\r\n", st);
        else
            kprintf("[wifi]   warning: GET_GPIO_STATE failed\r\n");
    }

    /* 2. route GPIO34-39 to ALT3 (SDIO bus). */
    wifi_gpio_sdio();
    kprintf("[wifi] GPIO34-39 -> ALT3 (GPFSEL3=0x%08x)\r\n", GPFSEL3);

    /* 3. bring up the Arasan SDHCI host. */
    if (emmc_host_init() != 0) {
        kprintf("[wifi] FAILED: host controller init\r\n");
        return -1;
    }

    /* 4. CMD0 (go idle) then CMD5 loop to read the SDIO OCR. */
    emmc_cmd(SD_CMD0_GO_IDLE, 0, CMD_RSPNS_NONE, NULL, 0, NULL);
    sleep(2);

    /* first CMD5 with arg 0 reads the OCR; then repeat with the voltage
     * window until the chip reports ready (OCR bit 31). */
    r = emmc_cmd(SD_CMD5_IO_OP_COND, 0, CMD_RSPNS_48, &ocr, 0, NULL);
    if (r != 0) {
        kprintf("[wifi] FAILED: CMD5 (no SDIO device responding)\r\n");
        return -1;
    }
    kprintf("[wifi] CMD5 OCR=0x%08x (numfn=%d mempresent=%d)\r\n",
            ocr, (ocr >> 28) & 7, (ocr >> 27) & 1);

    {
        int tries;
        uint32_t voltwin = ocr & 0x00FFFF00u;   /* the chip's supported window */
        for (tries = 0; tries < 50; tries++) {
            if (emmc_cmd(SD_CMD5_IO_OP_COND, voltwin, CMD_RSPNS_48, &ocr, 0, NULL) != 0)
                break;
            if (ocr & 0x80000000u) break;        /* ready */
            sleep(10);
        }
    }
    if (!(ocr & 0x80000000u)) {
        kprintf("[wifi] FAILED: SDIO OCR never ready (0x%08x)\r\n", ocr);
        return -1;
    }
    kprintf("[wifi] SDIO power-up ready, OCR=0x%08x\r\n", ocr);

    /* 5. CMD3 -> relative card address, CMD7 -> select. */
    if (emmc_cmd(SD_CMD3_SEND_RCA, 0, CMD_RSPNS_48, &rca, 0, NULL) != 0) {
        kprintf("[wifi] FAILED: CMD3 (no RCA)\r\n");
        return -1;
    }
    rca &= 0xFFFF0000u;
    kprintf("[wifi] RCA=0x%08x\r\n", rca);
    if (emmc_cmd(SD_CMD7_SELECT, rca, CMD_RSPNS_48BUSY, NULL, 0, NULL) != 0) {
        kprintf("[wifi] FAILED: CMD7 (select)\r\n");
        return -1;
    }
    kprintf("[wifi] chip selected (SDIO command state)\r\n");

    /* 6. enable backplane function 1 (CCCR IOEx bit 1) and wait IOR1. */
    if (sdio_cmd52(0, SDIO_CCCR_IOEx, 1, 0x02) < 0) {
        kprintf("[wifi] FAILED: enable F1 (CMD52 IOEx)\r\n");
        return -1;
    }
    {
        int tries;
        for (tries = 0; tries < 50; tries++) {
            ioe = sdio_cmd52(0, SDIO_CCCR_IORx, 0, 0);
            if (ioe >= 0 && (ioe & 0x02)) break;
            sleep(2);
        }
    }
    if (!(ioe & 0x02)) {
        kprintf("[wifi] FAILED: F1 not ready (IOR1=0x%02x)\r\n", ioe);
        return -1;
    }
    kprintf("[wifi] backplane function 1 enabled+ready\r\n");

    /* 7. read the silicon chip-id over the backplane (chipcommon offset 0). */
    if (wifi_backplane_read32(SI_ENUM_BASE, &chipid) != 0) {
        kprintf("[wifi] FAILED: backplane chip-id read\r\n");
        return -1;
    }
    kprintf("[wifi] chipcommon[0]=0x%08x  chip-id=0x%04x  rev=%d\r\n",
            chipid, chipid & CID_ID_MASK, (chipid >> 16) & 0xF);

    if ((chipid & CID_ID_MASK) == BCM43455_CHIP_ID) {
        kprintf("[wifi] *** SUCCESS: BCM43455 (0x4345) detected over SDIO ***\r\n");
        return 0;
    }
    kprintf("[wifi] chip-id 0x%04x is not the expected 0x4345 "
            "(check window/access path)\r\n", chipid & CID_ID_MASK);
    return -1;
}
