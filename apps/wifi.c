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
#include <stdarg.h>

/* Build id — bump on every flashed build so /api/wifi/probe (and the serial
 * trace) unambiguously report WHICH kernel is actually running.  The slow
 * SD-swap + power-cycle deploy loop kept leaving a stale kernel resident in
 * RAM; this removes the "is the new code even running?" guesswork. */
#define WIFI_BUILD_ID "wifi-stage1-b7 (free Arasan from SD card: GPIO48-53 ALT0)"

extern int kprintf(const char *, ...);
extern int _doprnt(const char *fmt, va_list ap, int (*putc)(int, int), int arg);

/* ------------------------------------------------------------------ *
 *  Trace buffer — every [wifi] line is captured here AND echoed to    *
 *  the serial console, so the HTTP reply can carry the whole trace.   *
 * ------------------------------------------------------------------ */
static char wifi_tbuf[4000];
static int  wifi_tn;

static int wifi_tputc(int c, int arg)
{
    (void)arg;
    if (wifi_tn < (int)sizeof(wifi_tbuf) - 1)
        wifi_tbuf[wifi_tn++] = (char)c;
    return c;
}

static void wifi_log(const char *fmt, ...)
{
    va_list ap;
    int start = wifi_tn;
    va_start(ap, fmt);
    _doprnt(fmt, ap, wifi_tputc, 0);
    va_end(ap);
    wifi_tbuf[wifi_tn] = '\0';
    kprintf("%s", &wifi_tbuf[start]);   /* mirror to serial console */
}

/* Public: the captured trace from the last wifi_probe() run. */
const char *wifi_trace(void) { return wifi_tbuf; }

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
#define GPFSEL4            (*(volatile uint32_t *)(WIFI_GPIO_BASE + 0x10)) /* GPIO40-49 */
#define GPFSEL5            (*(volatile uint32_t *)(WIFI_GPIO_BASE + 0x14)) /* GPIO50-59 */

/* Clock manager (GPCLK2 = WIFI_CLK on GPIO43, dts gpclk2_gpio43). */
#define WIFI_CM_BASE       0x3F101000UL
#define CM_GP2CTL          (*(volatile uint32_t *)(WIFI_CM_BASE + 0x80))
#define CM_GP2DIV          (*(volatile uint32_t *)(WIFI_CM_BASE + 0x84))
#define CM_PASSWD          0x5A000000u
#define CM_CTL_ENAB        (1u << 4)
#define CM_CTL_BUSY        (1u << 7)
#define CM_SRC_OSC         1          /* 19.2 MHz crystal oscillator */

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

/* Precise microsecond delay via the BCM free-running system timer (1 MHz). */
#define SYSTIMER_CLO (*(volatile uint32_t *)(0x3F003000UL + 0x04))
static void wifi_delay_us(uint32_t us)
{
    uint32_t start = SYSTIMER_CLO;
    while ((SYSTIMER_CLO - start) < us) { /* spin */ }
}

/* Write an EMMC/SDHCI register, then honour the BCM2835 Arasan erratum: at the
 * <=400 kHz init clock, successive register writes within ~2 SD clocks can be
 * LOST (clock-domain crossing).  sdhci-iproc delays ~4 SD clocks (~10us @
 * 400kHz) after each write; bcm2835-mmc ~6us.  Without this the command/clock
 * register writes silently vanish and CMD5 never actually issues -> the CMD5
 * timeout we were chasing.  The DATA register is exempt (per the erratum). */
static void ew(volatile uint32_t *reg, uint32_t val)
{
    *reg = val;
    wifi_delay_us(12);   /* generous: > 4 SD clocks at 400 kHz */
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
 *  WIFI_CLK — GPCLK2 on GPIO43 (the chip's reference clock).         *
 *  The BCM43455 needs a slow reference clock to come out of reset and *
 *  respond on SDIO; on the Pi 3 this is GPCLK2 routed to GPIO43       *
 *  (dts: gpclk2_gpio43).  Target ~32.768 kHz from the 19.2 MHz osc.   *
 * ================================================================== */
static void wifi_clk_setup(void)
{
    uint32_t sel = GPFSEL4;
    int alt43 = (sel >> 9) & 7;

    /* The firmware already routes GPIO43 -> ALT0 (GPCLK2) AND runs WIFI_CLK at
     * whatever frequency the chip expects (observed: GPIO43 fsel=4, GP2CTL
     * BUSY).  Reprogramming GPCLK2 ourselves risks feeding the chip the wrong
     * reference clock, so b5 TRUSTS the firmware: ensure GPIO43 is ALT0 and
     * leave the clock manager untouched. */
    if (alt43 != 4) {
        sel &= ~(7u << 9);
        sel |=  (4u << 9);
        GPFSEL4 = sel;
        wifi_log("[wifi] WIFI_CLK: set GPIO43 -> ALT0 (was fsel=%d)\r\n", alt43);
    }
    wifi_log("[wifi] WIFI_CLK: trusting firmware GPCLK2 (GP2CTL=0x%08x, GP2DIV=0x%08x)\r\n",
             CM_GP2CTL, CM_GP2DIV);
}

/* ================================================================== *
 *  GPIO 34-39 -> ALT3 (SDIO bus to the WiFi chip)                    *
 * ================================================================== */
static void wifi_gpio_sdio(void)
{
    uint32_t sel;
    int pin;

    /* ★ Disconnect the Arasan EMMC from the SD-card pins so it is free to
     * drive the WiFi SDIO bus.  The Arasan can mux to EITHER GPIO48-53 (SD
     * card) or GPIO34-39 (WiFi) via ALT3.  If the firmware left GPIO48-53 at
     * ALT3 (Arasan on the card — the Pi1/2 legacy default), the controller
     * stays on the card and our GPIO34-39 ALT3 never reaches the chip ->
     * CMD5 timeout.  plan9/Circle's Pi3 path sets GPIO48-53 to ALT0 (routes
     * the card to the separate SDHOST controller) precisely for this reason.
     * GPIO48-49 are in GPFSEL4 [24:29]; GPIO50-53 in GPFSEL5 [0:11].  ALT0=4.
     * (We do not use the SD card at runtime, so moving it is safe.) */
    sel = GPFSEL4;
    sel &= ~((7u << 24) | (7u << 27));
    sel |=  ((4u << 24) | (4u << 27));         /* GPIO48,49 -> ALT0 */
    GPFSEL4 = sel;
    sel = GPFSEL5;
    sel &= ~((7u << 0) | (7u << 3) | (7u << 6) | (7u << 9));
    sel |=  ((4u << 0) | (4u << 3) | (4u << 6) | (4u << 9)); /* GPIO50-53 -> ALT0 */
    GPFSEL5 = sel;

    sel = GPFSEL3;
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
    ew(&EMMC_CONTROL1, EMMC_CONTROL1 | C1_SRST_HC);
    for (t = 0; t < 100000; t++) {
        if (!(EMMC_CONTROL1 & C1_SRST_HC)) break;
        wifi_delay_us(100);
    }
    if (EMMC_CONTROL1 & C1_SRST_HC) {
        wifi_log("[wifi]   SRST_HC did not clear (controller dead?)\r\n");
        return -1;
    }

    /* SD bus power + voltage (standard SDHCI Power Control, byte at 0x29 =
     * CONTROL0 bits 8-11): bit8 = SD_BUS_POWER, bits9-11 = voltage select
     * (0b111 = 3.3V).  May be a no-op on the BCM EMMC (which powers the bus
     * via firmware/GPIO) but is harmless and required by spec on real SDHCI. */
    ew(&EMMC_CONTROL0, (EMMC_CONTROL0 & ~0xF00u) | (1u << 8) | (7u << 9));

    /* program the SD clock: 8-bit divided-clock mode (SDHCI v3 uses the
     * low 8 bits of the divisor in [15:8] plus the upper 2 bits in [7:6]). */
    c1 = EMMC_CONTROL1;
    c1 &= ~0xFFE0u;                       /* clear CLK_EN | DIV | GENSEL */
    c1 |= C1_CLK_INTLEN | C1_TOUNIT_MAX;
    c1 |= (divisor & 0xFF) << 8;
    c1 |= ((divisor >> 8) & 0x3) << 6;
    ew(&EMMC_CONTROL1, c1);

    for (t = 0; t < 100000; t++) {
        if (EMMC_CONTROL1 & C1_CLK_STABLE) break;
        wifi_delay_us(100);
    }
    if (!(EMMC_CONTROL1 & C1_CLK_STABLE)) {
        wifi_log("[wifi]   internal clock never stabilised\r\n");
        return -1;
    }
    ew(&EMMC_CONTROL1, EMMC_CONTROL1 | C1_CLK_EN);   /* gate clock to the chip */
    wifi_delay_us(2000);
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
    wifi_log("[wifi] EMMC base clock = %u Hz\r\n", base);
    if (base == 0) {
        base = 250000000u;   /* fallback to a common Pi default */
        wifi_log("[wifi]   (base=0 from firmware, assuming %u)\r\n", base);
    }

    ver = EMMC_SLOTISR_VER;
    wifi_log("[wifi] SDHCI HC version=0x%02x (SLOTISR_VER=0x%08x)\r\n",
            (ver >> 16) & 0xFF, ver);

    div = emmc_divisor(base, 400000);
    wifi_log("[wifi] SDIO init clock divisor=%u (~%u Hz)\r\n",
            div, base / (2 * div));
    if (emmc_reset_clock(div) != 0)
        return -1;

    /* enable all normal-interrupt status bits (polled, not IRQ) */
    ew(&EMMC_IRPT_EN,   0xFFFFFFFFu);
    ew(&EMMC_IRPT_MASK, 0xFFFFFFFFu);
    ew(&EMMC_INTERRUPT, 0xFFFFFFFFu);      /* clear stale */
    wifi_log("[wifi] SDHCI init done: CONTROL0=0x%08x CONTROL1=0x%08x STATUS=0x%08x\r\n",
             EMMC_CONTROL0, EMMC_CONTROL1, EMMC_STATUS);
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
    ew(&EMMC_INTERRUPT, 0xFFFFFFFFu);

    if (read_4byte) {
        ew(&EMMC_BLKSIZECNT, (1u << 16) | 4);   /* 1 block of 4 bytes */
    }

    ew(&EMMC_ARG1, arg);
    cmd = (cmd_idx << 24) | resp_flags;
    if (read_4byte)
        cmd |= CMD_ISDATA | TM_DAT_DIR_CH;
    ew(&EMMC_CMDTM, cmd);   /* erratum-safe: spaced write so the command issues */

    /* wait for CMD_DONE or error */
    for (t = 0; t < 1000000; t++) {
        intr = EMMC_INTERRUPT;
        if (intr & INT_ERR)      { wifi_log("[wifi]   cmd%d err intr=0x%08x\r\n", cmd_idx, intr); return -1; }
        if (intr & INT_CMD_DONE) { EMMC_INTERRUPT = INT_CMD_DONE; break; }
    }
    if (t >= 1000000) { wifi_log("[wifi]   cmd%d timeout (no CMD_DONE)\r\n", cmd_idx); return -1; }

    if (resp_out) *resp_out = EMMC_RESP0;

    if (read_4byte) {
        for (t = 0; t < 1000000; t++) {
            intr = EMMC_INTERRUPT;
            if (intr & INT_ERR)       { wifi_log("[wifi]   cmd%d data err 0x%08x\r\n", cmd_idx, intr); return -1; }
            if (intr & INT_READ_RDY)  { EMMC_INTERRUPT = INT_READ_RDY; break; }
        }
        if (t >= 1000000) { wifi_log("[wifi]   cmd%d no READ_RDY\r\n", cmd_idx); return -1; }
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

    wifi_tn = 0;                /* reset trace buffer for this run */
    wifi_log("[wifi] === BCM43455 SDIO bring-up (Stage 1+2) ===\r\n");
    wifi_log("[wifi] build: %s\r\n", WIFI_BUILD_ID);

    /* 0a. ROUTING CHECK: read the firmware's GPIO function-select for
     *     GPIO34-39 BEFORE we touch them.  If the firmware already has them
     *     as ALT3 (=7), the Arasan EMMC (0x3F300000) is wired to the WiFi
     *     SDIO bus and our controller choice is correct.  If they read as
     *     input(0)/something-else, the WiFi SDIO may instead be on the SDHOST
     *     controller (0x3F202000) and we are driving the wrong block. */
    {
        uint32_t f3 = GPFSEL3, f4 = GPFSEL4;
        int p;
        wifi_log("[wifi] firmware GPFSEL3=0x%08x GPFSEL4=0x%08x\r\n", f3, f4);
        for (p = 34; p <= 39; p++) {
            int alt = (f3 >> (3 * (p - 30))) & 7;
            wifi_log("[wifi]   GPIO%d fsel=%d (%s)\r\n", p, alt,
                     (alt == 7) ? "ALT3=SD1/Arasan" :
                     (alt == 0) ? "input" : "other");
        }
        wifi_log("[wifi]   GPIO43 (WIFI_CLK) fsel=%d\r\n", (f4 >> 9) & 7);
        /* SD-card pins 48-53: if these read ALT3(=7) the Arasan is on the
         * card (the conflict b7 fixes by moving them to ALT0). */
        {
            uint32_t f5 = GPFSEL5;
            wifi_log("[wifi]   GPIO48=%d 49=%d 50=%d 51=%d 52=%d 53=%d (card pins; 7=Arasan-on-card)\r\n",
                     (f4 >> 24) & 7, (f4 >> 27) & 7,
                     (f5 >> 0) & 7, (f5 >> 3) & 7, (f5 >> 6) & 7, (f5 >> 9) & 7);
        }
    }

    /* 0b. start the chip's reference clock (GPCLK2/WIFI_CLK) BEFORE releasing
     *    reset, so the chip has a clock as it boots. */
    wifi_clk_setup();

    /* 1. power-cycle the chip via WL_REG_ON (firmware expander gpio 129):
     *    drive reset asserted (0), settle, then deasserted (1), and read it
     *    back to confirm the firmware actually drove the expander pin. */
    {
        uint32_t st = 0xDEAD;
        wifi_log("[wifi] WL_REG_ON reset cycle (fw gpio %d)...\r\n", WIFI_GPIO_WL_ON_FW);
        wifi_fw_set_gpio(WIFI_GPIO_WL_ON_FW, 0);   /* hold in reset */
        sleep(50);
        if (wifi_fw_set_gpio(WIFI_GPIO_WL_ON_FW, 1) != 0)  /* release */
            wifi_log("[wifi]   warning: SET_GPIO_STATE response not OK\r\n");
        sleep(150);                                 /* regulators + LPO settle */
        if (wifi_fw_get_gpio(WIFI_GPIO_WL_ON_FW, &st) == 0)
            wifi_log("[wifi] WL_REG_ON readback state=%u (expect 1)\r\n", st);
        else
            wifi_log("[wifi]   warning: GET_GPIO_STATE failed\r\n");
    }

    /* 2. route GPIO34-39 to ALT3 (SDIO bus). */
    wifi_gpio_sdio();
    wifi_log("[wifi] GPIO34-39 -> ALT3 (GPFSEL3=0x%08x)\r\n", GPFSEL3);

    /* 3. bring up the Arasan SDHCI host. */
    if (emmc_host_init() != 0) {
        wifi_log("[wifi] FAILED: host controller init\r\n");
        return -1;
    }

    /* 4. CMD0 (go idle) then CMD5 loop to read the SDIO OCR. */
    emmc_cmd(SD_CMD0_GO_IDLE, 0, CMD_RSPNS_NONE, NULL, 0, NULL);
    sleep(2);

    /* first CMD5 with arg 0 reads the OCR; then repeat with the voltage
     * window until the chip reports ready (OCR bit 31). */
    r = emmc_cmd(SD_CMD5_IO_OP_COND, 0, CMD_RSPNS_48, &ocr, 0, NULL);
    if (r != 0) {
        wifi_log("[wifi] FAILED: CMD5 (no SDIO device responding)\r\n");
        return -1;
    }
    wifi_log("[wifi] CMD5 OCR=0x%08x (numfn=%d mempresent=%d)\r\n",
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
        wifi_log("[wifi] FAILED: SDIO OCR never ready (0x%08x)\r\n", ocr);
        return -1;
    }
    wifi_log("[wifi] SDIO power-up ready, OCR=0x%08x\r\n", ocr);

    /* 5. CMD3 -> relative card address, CMD7 -> select. */
    if (emmc_cmd(SD_CMD3_SEND_RCA, 0, CMD_RSPNS_48, &rca, 0, NULL) != 0) {
        wifi_log("[wifi] FAILED: CMD3 (no RCA)\r\n");
        return -1;
    }
    rca &= 0xFFFF0000u;
    wifi_log("[wifi] RCA=0x%08x\r\n", rca);
    if (emmc_cmd(SD_CMD7_SELECT, rca, CMD_RSPNS_48BUSY, NULL, 0, NULL) != 0) {
        wifi_log("[wifi] FAILED: CMD7 (select)\r\n");
        return -1;
    }
    wifi_log("[wifi] chip selected (SDIO command state)\r\n");

    /* 6. enable backplane function 1 (CCCR IOEx bit 1) and wait IOR1. */
    if (sdio_cmd52(0, SDIO_CCCR_IOEx, 1, 0x02) < 0) {
        wifi_log("[wifi] FAILED: enable F1 (CMD52 IOEx)\r\n");
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
        wifi_log("[wifi] FAILED: F1 not ready (IOR1=0x%02x)\r\n", ioe);
        return -1;
    }
    wifi_log("[wifi] backplane function 1 enabled+ready\r\n");

    /* 7. read the silicon chip-id over the backplane (chipcommon offset 0). */
    if (wifi_backplane_read32(SI_ENUM_BASE, &chipid) != 0) {
        wifi_log("[wifi] FAILED: backplane chip-id read\r\n");
        return -1;
    }
    wifi_log("[wifi] chipcommon[0]=0x%08x  chip-id=0x%04x  rev=%d\r\n",
            chipid, chipid & CID_ID_MASK, (chipid >> 16) & 0xF);

    if ((chipid & CID_ID_MASK) == BCM43455_CHIP_ID) {
        wifi_log("[wifi] *** SUCCESS: BCM43455 (0x4345) detected over SDIO ***\r\n");
        return 0;
    }
    wifi_log("[wifi] chip-id 0x%04x is not the expected 0x4345 "
            "(check window/access path)\r\n", chipid & CID_ID_MASK);
    return -1;
}
