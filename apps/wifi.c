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
#define WIFI_BUILD_ID "wifi-stage6-b15 (assoc + observe EAPOL forwarding)"

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
#define SDIO_CCCR_INT_ENABLE    0x04   /* interrupt enable                  */
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
 *  Stage 3 — backplane bulk access + chip core/RAM enumeration       *
 *  (port of plan9 ether4330.c sbmem/sbrw/corescan/ramscan)           *
 * ================================================================== */
#define SB_WSIZE        0x8000u          /* backplane window size      */
#define SB_32BIT        0x8000u          /* 32-bit access flag in off  */
#define SB_ENUMBASE     0x18000000u
#define CORE_ARMCR4     0x83E
#define CORE_ARMCM3     0x82A
#define CORE_ARM7       0x825
#define CORE_CHIPCOMMON 0x800
#define CORE_SOCRAM     0x80E
#define CORE_SDIODEV    0x829
#define CORE_D11        0x812
#define REG_IOCTRL      0x408
#define REG_RESETCTRL   0x800
#define CR4_CAP         0x04
#define CR4_BANKIDX     0x40
#define CR4_BANKINFO    0x44
#define CR4_CPUHALT     0x20

/* discovered chip layout (filled by wifi_corescan / wifi_ramscan) */
static uint32_t chip_chipcommon, chip_armctl, chip_armregs, chip_armcore;
static uint32_t chip_d11ctl, chip_socramctl, chip_socramregs, chip_sdregs;
static uint32_t chip_rambase, chip_socramsize;

/* General CMD53 (IO_RW_EXTENDED) with PIO data transfer.  `off` is the F1
 * register offset (already including SB_32BIT when addressing the backplane).
 * Transfers up to 511*bsize bytes (bsize=64 for Fn1).  Returns 0 on success. */
static int wifi_cmd53_pio(int write, int fn, uint32_t off, uint8_t *buf,
                          int len, int incr)
{
    uint32_t arg, intr, t, total, w, words;
    uint32_t bsize = (fn == 2) ? 512u : 64u;
    int blkmode, blkcnt;

    if (len <= 0) return 0;
    if ((uint32_t)len > bsize) { blkmode = 1; blkcnt = len / bsize; total = blkcnt * bsize; }
    else                       { blkmode = 0; blkcnt = len;         total = len; }

    for (t = 0; t < 1000000; t++) if (!(EMMC_STATUS & SR_CMD_INHIBIT)) break;
    ew(&EMMC_INTERRUPT, 0xFFFFFFFFu);
    if (blkmode) ew(&EMMC_BLKSIZECNT, ((uint32_t)blkcnt << 16) | bsize);
    else         ew(&EMMC_BLKSIZECNT, (1u << 16) | (uint32_t)total);

    arg = ((uint32_t)(write & 1) << 31) | ((uint32_t)(fn & 7) << 28) |
          ((uint32_t)(blkmode & 1) << 27) | ((uint32_t)(incr & 1) << 26) |
          ((off & 0x1FFFF) << 9) |
          ((blkmode ? (uint32_t)blkcnt : (uint32_t)total) & 0x1FF);
    ew(&EMMC_ARG1, arg);
    {
        uint32_t cmd = (SD_CMD53_IO_RW_EXT << 24) | CMD_RSPNS_48 |
                       CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA;
        if (!write)  cmd |= TM_DAT_DIR_CH;       /* card -> host */
        if (blkmode) cmd |= (1u << 5) | (1u << 1); /* multiblock + blkcnt-en */
        ew(&EMMC_CMDTM, cmd);
    }
    for (t = 0; t < 1000000; t++) {
        intr = EMMC_INTERRUPT;
        if (intr & INT_ERR)      { wifi_log("[wifi]   cmd53 err 0x%08x\r\n", intr); return -1; }
        if (intr & INT_CMD_DONE) { ew(&EMMC_INTERRUPT, INT_CMD_DONE); break; }
    }
    if (t >= 1000000) { wifi_log("[wifi]   cmd53 no CMD_DONE\r\n"); return -1; }

    /* PIO: the controller raises READ_RDY/WRITE_RDY before each block-sized
     * buffer; transfer it word-by-word via the DATA register. */
    words = (total + 3) / 4;
    {
        uint32_t done = 0;
        while (done < words) {
            uint32_t flag = write ? INT_WRITE_RDY : INT_READ_RDY;
            uint32_t chunk = (total > bsize && (words - done) > (bsize / 4))
                             ? (bsize / 4) : (words - done);
            for (t = 0; t < 1000000; t++) {
                intr = EMMC_INTERRUPT;
                if (intr & INT_ERR)  { wifi_log("[wifi]   cmd53 data err 0x%08x\r\n", intr); return -1; }
                if (intr & flag)     { EMMC_INTERRUPT = flag; break; }
            }
            if (t >= 1000000) { wifi_log("[wifi]   cmd53 no data rdy\r\n"); return -1; }
            for (w = 0; w < chunk; w++) {
                uint32_t idx = (done + w) * 4;
                if (write) {
                    uint32_t v = (uint32_t)buf[idx] | ((uint32_t)buf[idx+1] << 8) |
                                 ((uint32_t)buf[idx+2] << 16) | ((uint32_t)buf[idx+3] << 24);
                    EMMC_DATA = v;
                } else {
                    uint32_t v = EMMC_DATA;
                    buf[idx] = v; buf[idx+1] = v >> 8; buf[idx+2] = v >> 16; buf[idx+3] = v >> 24;
                }
            }
            done += chunk;
        }
    }
    for (t = 0; t < 1000000; t++)
        if (EMMC_INTERRUPT & INT_DATA_DONE) { EMMC_INTERRUPT = INT_DATA_DONE; break; }
    return 0;
}

/* Bulk backplane read/write across 0x8000 windows (plan9 sbmem+sbrw).
 * Each CMD53 is capped at SB_XFER (2048) so it stays well under the 511-block
 * limit (32 blocks of 64) and within a single 0x8000 window. */
#define SB_XFER 2048
static int wifi_sbmem(int write, uint8_t *buf, int len, uint32_t addr)
{
    while (len > 0) {
        uint32_t woff;
        int wrem = (int)(SB_WSIZE - (addr & (SB_WSIZE - 1)));   /* to window end */
        int chunk = len;
        if (chunk > SB_XFER) chunk = SB_XFER;
        if (chunk > wrem)    chunk = wrem;
        if (sdio_set_window(addr) != 0) return -1;
        woff = (addr & (SB_WSIZE - 1)) | SB_32BIT;
        if (wifi_cmd53_pio(write, 1, woff, buf, (chunk + 3) & ~3, 1) != 0) return -1;
        addr += chunk; buf += chunk; len -= chunk;
    }
    return 0;
}

/* 32-bit backplane write at absolute address. */
static int wifi_backplane_write32(uint32_t addr, uint32_t val)
{
    uint8_t b[4];
    b[0] = val; b[1] = val >> 8; b[2] = val >> 16; b[3] = val >> 24;
    return wifi_sbmem(1, b, 4, addr);
}

/* Scan the SonicsSiliconBackplane EROM to locate the chip's cores. */
static int wifi_corescan(void)
{
    static uint8_t erom[512];
    uint32_t eromptr, i;
    int coreid = 0;

    /* EROM pointer lives at chipcommon enum base + 63*4 */
    if (wifi_backplane_read32(SB_ENUMBASE + 63 * 4, &eromptr) != 0) return -1;
    chip_chipcommon = chip_armctl = chip_armregs = chip_armcore = 0;
    chip_d11ctl = chip_socramctl = chip_socramregs = chip_sdregs = 0;
    if (wifi_sbmem(0, erom, sizeof(erom), eromptr) != 0) return -1;

    for (i = 0; i < sizeof(erom); i += 4) {
        uint32_t addr;
        switch (erom[i] & 0xF) {
        case 0xF:                                  /* end */
            return 0;
        case 0x1:                                  /* core info */
            if ((erom[i + 4] & 0xF) != 0x1) break;
            coreid = (erom[i + 1] | (erom[i + 2] << 8)) & 0xFFF;
            i += 4;
            break;
        case 0x5:                                  /* address descriptor */
            addr = (erom[i + 1] << 8) | (erom[i + 2] << 16) | (erom[i + 3] << 24);
            addr &= ~0xFFFu;
            switch (coreid) {
            case CORE_CHIPCOMMON:
                if ((erom[i] & 0xC0) == 0) chip_chipcommon = addr;
                break;
            case CORE_ARMCM3: case CORE_ARM7: case CORE_ARMCR4:
                chip_armcore = coreid;
                if (erom[i] & 0xC0) { if (!chip_armctl) chip_armctl = addr; }
                else                { if (!chip_armregs) chip_armregs = addr; }
                break;
            case CORE_SOCRAM:
                if (erom[i] & 0xC0) chip_socramctl = addr;
                else if (!chip_socramregs) chip_socramregs = addr;
                break;
            case CORE_SDIODEV:
                if ((erom[i] & 0xC0) == 0) chip_sdregs = addr;
                break;
            case CORE_D11:
                if (erom[i] & 0xC0) chip_d11ctl = addr;
                break;
            }
            break;
        }
    }
    return 0;
}

/* Determine on-chip RAM size (CR4 path for the BCM43455). */
static int wifi_ramscan(void)
{
    uint32_t r, n, size = 0;
    int banks, i;

    if (chip_armcore != CORE_ARMCR4) {
        wifi_log("[wifi]   ramscan: non-CR4 core 0x%x not handled here\r\n", chip_armcore);
        return -1;
    }
    r = chip_armregs;
    if (wifi_backplane_read32(r + CR4_CAP, &n) != 0) return -1;
    banks = ((n >> 4) & 0xF) + (n & 0xF);
    for (i = 0; i < banks; i++) {
        if (wifi_backplane_write32(r + CR4_BANKIDX, i) != 0) return -1;
        if (wifi_backplane_read32(r + CR4_BANKINFO, &n) != 0) return -1;
        size += 8192 * ((n & 0x3F) + 1);
    }
    chip_socramsize = size;
    chip_rambase = 0x198000;       /* BCM43455 TCM base */
    return 0;
}

/* ================================================================== *
 *  Stage 3b — firmware download + ARM core start                     *
 *  (port of plan9 ether4330.c sbinit/fwload/sbenable)                *
 * ================================================================== */
/* Clkcsr (F1 byte register 0x1000e) bits */
#define REG_CLKCSR   0x1000E
#define CLK_FORCEALP 0x01
#define CLK_FORCEHT  0x02
#define CLK_REQALP   0x08
#define CLK_REQHT    0x10
#define CLK_NOHWREQ  0x20
#define CLK_ALPAVAIL 0x40
#define CLK_HTAVAIL  0x80
#define REG_PULLUPS  0x1000F
/* sdio core (0x829) registers */
#define SDR_INTSTATUS 0x20
#define SDR_INTMASK   0x24
#define SDR_MBOXDATA  0x48

extern uint8_t wifi_fw_bin[],   wifi_fw_bin_end[];
extern uint8_t wifi_nvram_txt[], wifi_nvram_txt_end[];
extern uint8_t wifi_clm_blob[],  wifi_clm_blob_end[];

/* single-byte F1 register access (plan9 cfgr/cfgw) */
static int  cfgr(uint32_t off) { return sdio_cmd52(1, off, 0, 0); }
static void cfgw(uint32_t off, int v) { sdio_cmd52(1, off, 1, v); }

/* core enable/reset via the SonicsSiliconBackplane wrapper (Ioctrl/Resetctrl) */
static void sb_disable(uint32_t regs, int pre, int ioctl)
{
    uint32_t v;
    if (wifi_backplane_read32(regs + REG_RESETCTRL, &v) == 0 && (v & 1)) {
        wifi_backplane_write32(regs + REG_IOCTRL, 3 | ioctl);
        wifi_backplane_read32(regs + REG_IOCTRL, &v);
        return;
    }
    wifi_backplane_write32(regs + REG_IOCTRL, 3 | pre);
    wifi_backplane_read32(regs + REG_IOCTRL, &v);
    wifi_backplane_write32(regs + REG_RESETCTRL, 1);
    wifi_delay_us(10);
    do { wifi_backplane_read32(regs + REG_RESETCTRL, &v); } while (!(v & 1));
    wifi_backplane_write32(regs + REG_IOCTRL, 3 | ioctl);
    wifi_backplane_read32(regs + REG_IOCTRL, &v);
}

static void sb_reset(uint32_t regs, int pre, int ioctl)
{
    uint32_t v;
    sb_disable(regs, pre, ioctl);
    do {
        wifi_backplane_read32(regs + REG_RESETCTRL, &v);
        if (v & 1) { wifi_backplane_write32(regs + REG_RESETCTRL, 0); wifi_delay_us(40); }
    } while (v & 1);
    wifi_backplane_write32(regs + REG_IOCTRL, 1 | ioctl);
    wifi_backplane_read32(regs + REG_IOCTRL, &v);
}

/* condense nvram text to a 'var=value\0...\0' list (plan9 condense). */
static int wifi_condense(uint8_t *buf, int n)
{
    uint8_t *p, *ep = buf + n, *op = buf, *lp = buf;
    int c, skipping = 0;
    for (p = buf; p < ep; p++) {
        c = *p;
        if (c == '#') skipping = 1;
        else if (c == '\0' || c == '\n') {
            skipping = 0;
            if (op != lp) { *op++ = '\0'; lp = op; }
        } else if (c == '\r') { /* drop */ }
        else if (!skipping) *op++ = (uint8_t)c;
    }
    if (!skipping && op != lp) *op++ = '\0';
    *op++ = '\0';
    for (n = op - buf; n & 3; n++) *op++ = '\0';
    return n;
}

/* Download firmware + NVRAM into chip RAM and start the ARM CR4 core. */
static int wifi_fwload(void)
{
    static uint8_t nvbuf[3072];
    uint8_t trailer[4];
    uint32_t resetvec, t, fwlen, nvlen, off;
    int i;

    fwlen = (uint32_t)(wifi_fw_bin_end - wifi_fw_bin);
    nvlen = (uint32_t)(wifi_nvram_txt_end - wifi_nvram_txt);
    wifi_log("[wifi] fwload: fw=%u B nvram=%u B -> rambase 0x%x (ramsize %u)\r\n",
             fwlen, nvlen, chip_rambase, chip_socramsize);

    /* request ALP clock */
    cfgw(REG_CLKCSR, CLK_REQALP);
    for (i = 0; i < 2000 && !(cfgr(REG_CLKCSR) & CLK_ALPAVAIL); i++) wifi_delay_us(100);

    /* clear the nvram trailer word */
    trailer[0] = trailer[1] = trailer[2] = trailer[3] = 0;
    if (wifi_sbmem(1, trailer, 4, chip_rambase + chip_socramsize - 4) != 0) return -1;

    /* write the firmware to RAM base; first 4 bytes are the reset vector */
    resetvec = (uint32_t)wifi_fw_bin[0] | ((uint32_t)wifi_fw_bin[1] << 8) |
               ((uint32_t)wifi_fw_bin[2] << 16) | ((uint32_t)wifi_fw_bin[3] << 24);
    if (wifi_sbmem(1, wifi_fw_bin, (int)fwlen, chip_rambase) != 0) {
        wifi_log("[wifi] fwload: firmware write failed\r\n");
        return -1;
    }
    wifi_log("[wifi] fwload: firmware written\r\n");

    /* condense nvram, write near the top of RAM, then the length token */
    if (nvlen > sizeof(nvbuf)) nvlen = sizeof(nvbuf);
    for (i = 0; i < (int)nvlen; i++) nvbuf[i] = wifi_nvram_txt[i];
    nvlen = wifi_condense(nvbuf, (int)nvlen);
    off = chip_socramsize - nvlen - 4;
    if (wifi_sbmem(1, nvbuf, (int)nvlen, chip_rambase + off) != 0) return -1;
    t = nvlen / 4;
    t = (t & 0xFFFF) | (~t << 16);
    trailer[0] = t; trailer[1] = t >> 8; trailer[2] = t >> 16; trailer[3] = t >> 24;
    if (wifi_sbmem(1, trailer, 4, chip_rambase + chip_socramsize - 4) != 0) return -1;
    wifi_log("[wifi] fwload: nvram %u B + trailer 0x%x written\r\n", nvlen, t);

    /* start the CR4: clear sdio intstatus, plant reset vector at 0, run */
    wifi_backplane_write32(chip_sdregs + SDR_INTSTATUS, 0xFFFFFFFFu);
    if (resetvec != 0) {
        trailer[0] = resetvec; trailer[1] = resetvec >> 8;
        trailer[2] = resetvec >> 16; trailer[3] = resetvec >> 24;
        wifi_sbmem(1, trailer, 4, 0);
    }
    sb_reset(chip_armctl, CR4_CPUHALT, 0);
    wifi_log("[wifi] fwload: CR4 released (resetvec=0x%x) — firmware running\r\n", resetvec);
    return 0;
}

/* After fw is running: bring up the HT clock and enable WLAN data (Fn2). */
static int wifi_sbenable(void)
{
    int i, fn2;
    cfgw(REG_CLKCSR, 0);
    wifi_delay_us(1000);
    cfgw(REG_CLKCSR, CLK_REQHT);
    for (i = 0; i < 500 && !(cfgr(REG_CLKCSR) & CLK_HTAVAIL); i++) wifi_delay_us(2000);
    if (!(cfgr(REG_CLKCSR) & CLK_HTAVAIL)) {
        wifi_log("[wifi] sbenable: HT clock never came up (csr=0x%02x)\r\n", cfgr(REG_CLKCSR));
        return -1;
    }
    cfgw(REG_CLKCSR, cfgr(REG_CLKCSR) | CLK_FORCEHT);
    wifi_delay_us(10000);
    wifi_log("[wifi] sbenable: HT clock up (csr=0x%02x)\r\n", cfgr(REG_CLKCSR));

    /* sdio core: protocol version + interrupt mask */
    wifi_backplane_write32(chip_sdregs + SDR_MBOXDATA, 4 << 16);
    wifi_backplane_write32(chip_sdregs + SDR_INTMASK, (1u<<7)|(1u<<6)|(1u<<5));

    /* enable WLAN data function 2 */
    sdio_cmd52(0, SDIO_CCCR_IOEx, 1, (1 << 1) | (1 << 2));   /* enable F1+F2 */
    for (i = 0, fn2 = 0; i < 50; i++) {
        fn2 = sdio_cmd52(0, SDIO_CCCR_IORx, 0, 0);
        if (fn2 >= 0 && (fn2 & (1 << 2))) break;
        wifi_delay_us(2000);
    }
    if (!(fn2 & (1 << 2))) {
        wifi_log("[wifi] sbenable: Fn2 not ready (IOR=0x%02x)\r\n", fn2);
        return -1;
    }
    sdio_cmd52(0, SDIO_CCCR_INT_ENABLE, 1, (1<<1)|(1<<2)|1);
    wifi_log("[wifi] sbenable: WLAN function 2 enabled+ready\r\n");
    return 0;
}

/* ================================================================== *
 *  Stage 4 — SDPCM control channel (ioctl / iovar over Fn2)          *
 *  (synchronous, polled port of plan9 ether4330.c wlcmd/packetrw)    *
 * ================================================================== */
#define SDPCM_HDR   12          /* len[2] lenck[2] seq chan nextlen doff fc win ver pad */
#define BCDC_HDR    16          /* cmd[4] len[4] flags[2] id[2] status[4] */
#define SD_INT_FRAME  (1u << 6)
#define SD_INT_MBOX   (1u << 7)
#define SDR_SBMBOX    0x40
#define SDR_HOSTMBOX  0x4c
#define WLC_GET_VAR   262
#define WLC_SET_VAR   263

static uint8_t  wl_txseq;
static uint16_t wl_reqid;

/* Fn2 bulk transfer to/from the fixed FIFO address (incr=0).  Split into
 * <=512-byte byte-mode CMD53s: block-mode truncates non-block-aligned lengths
 * and SDPCM frames are arbitrary lengths (e.g. the ~1.4 KB clmload frame). */
static int wifi_packetrw(int write, uint8_t *buf, int len)
{
    while (len > 0) {
        int chunk = (len > 512) ? 512 : len;
        if (wifi_cmd53_pio(write, 2, 0, buf, (chunk + 3) & ~3, 0) != 0) return -1;
        buf += chunk; len -= chunk;
    }
    return 0;
}

/* Issue a firmware ioctl over the SDPCM control channel and return its reply.
 * write!=0 sends `data` (dlen) as the ioctl payload; otherwise `data` is the
 * request (e.g. an iovar name) and the reply is copied into res/rlen.
 * Returns 0 on success. */
static int wifi_wlcmd(int write, int op, const uint8_t *data, int dlen,
                      uint8_t *res, int rlen)
{
    static uint8_t pkt[2048];
    int tlen = write ? (dlen + rlen) : (dlen > rlen ? dlen : rlen);
    int len = SDPCM_HDR + BCDC_HDR + tlen;
    int i, tries;

    if (len > (int)sizeof(pkt)) { wifi_log("[wifi] wlcmd: pkt too big %d\r\n", len); return -1; }
    for (i = 0; i < len; i++) pkt[i] = 0;

    /* SDPCM header (control channel = 0) */
    pkt[0] = len & 0xFF; pkt[1] = (len >> 8) & 0xFF;
    pkt[2] = ~len & 0xFF; pkt[3] = (~len >> 8) & 0xFF;
    pkt[4] = wl_txseq;          /* seq */
    pkt[5] = 0;                 /* chanflg: channel 0 = control */
    pkt[7] = SDPCM_HDR;         /* doffset */

    /* BCDC command header */
    {
        uint8_t *q = pkt + SDPCM_HDR;
        q[0] = op; q[1] = op >> 8; q[2] = op >> 16; q[3] = op >> 24;
        q[4] = tlen; q[5] = tlen >> 8; q[6] = tlen >> 16; q[7] = tlen >> 24;
        q[8] = write ? 2 : 0;   /* flags */
        wl_reqid++;
        q[10] = wl_reqid; q[11] = wl_reqid >> 8;
    }
    if (dlen > 0) for (i = 0; i < dlen; i++) pkt[SDPCM_HDR + BCDC_HDR + i] = data[i];

    if (wifi_packetrw(1, pkt, len) != 0) { wifi_log("[wifi] wlcmd: tx failed\r\n"); return -1; }
    wl_txseq++;

    /* wait for the control-channel response by polling the Fn2 FIFO directly:
     * a read of the 12-byte SDPCM header returns len==0 when no frame is
     * queued (plan9 wlreadpkt), so we don't depend on the interrupt status. */
    for (tries = 0; tries < 400; tries++) {
        uint32_t ints;
        int plen, lenck, chan, doff;

        /* best-effort: ack any sdio-core interrupt + firmware mailbox */
        if (wifi_backplane_read32(chip_sdregs + SDR_INTSTATUS, &ints) == 0 && ints) {
            wifi_backplane_write32(chip_sdregs + SDR_INTSTATUS, ints);
            if (ints & SD_INT_MBOX) {
                uint32_t mb;
                wifi_backplane_read32(chip_sdregs + SDR_HOSTMBOX, &mb);
                wifi_backplane_write32(chip_sdregs + SDR_SBMBOX, 2);   /* ack */
            }
        }

        /* read the SDPCM header from the FIFO; len==0 => nothing queued yet */
        if (wifi_packetrw(0, pkt, SDPCM_HDR) != 0) { wifi_delay_us(2000); continue; }
        plen = pkt[0] | (pkt[1] << 8);
        lenck = pkt[2] | (pkt[3] << 8);
        if (tries < 3)
            wifi_log("[wifi] wlcmd rx try%d: len=0x%x lenck=0x%x ints=0x%x\r\n",
                     tries, plen, lenck, ints);
        if (plen == 0) { wifi_delay_us(2000); continue; }
        if (lenck != ((plen ^ 0xFFFF) & 0xFFFF) || plen < SDPCM_HDR || plen > (int)sizeof(pkt)) {
            wifi_log("[wifi] wlcmd: bad frame len=0x%x lenck=0x%x\r\n", plen, lenck);
            wifi_delay_us(2000);
            continue;
        }
        chan = pkt[5] & 0xF;
        doff = pkt[7];
        if (plen > SDPCM_HDR)
            if (wifi_packetrw(0, pkt + SDPCM_HDR, plen - SDPCM_HDR) != 0) return -1;
        if (chan != 0) { wifi_log("[wifi] wlcmd: drained chan %d frame\r\n", chan); continue; }

        /* BCDC reply at doff */
        {
            uint8_t *q = pkt + doff;
            uint32_t st = q[12] | (q[13] << 8) | (q[14] << 16) | (q[15] << 24);
            if (st != 0) { wifi_log("[wifi] wlcmd: op %d status %u\r\n", op, st); return -1; }
            if (!write && rlen > 0) {
                uint8_t *d = q + BCDC_HDR;
                for (i = 0; i < rlen; i++) res[i] = d[i];
            }
            return 0;
        }
    }
    wifi_log("[wifi] wlcmd: op %d timed out (no control response)\r\n", op);
    return -1;
}

/* iovar GET (e.g. "ver"). */
static int wifi_get_iovar(const char *name, uint8_t *val, int len)
{
    int n = 0;
    while (name[n]) n++;
    n++;                         /* include NUL */
    return wifi_wlcmd(0, WLC_GET_VAR, (const uint8_t *)name, n, val, len);
}

/* WLC ioctl taking a single 4-byte int (e.g. PASSIVE_SCAN). */
static int wifi_cmd_int(int op, uint32_t v)
{
    uint8_t b[4];
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
    return wifi_wlcmd(1, op, b, 4, NULL, 0);
}

/* iovar SET: name\0 + value as one payload. */
static int wifi_set_iovar(const char *name, const uint8_t *val, int vlen)
{
    static uint8_t b[1600];
    int n = 0, i;
    while (name[n]) { b[n] = name[n]; n++; }
    b[n++] = '\0';
    for (i = 0; i < vlen && (n + i) < (int)sizeof(b); i++) b[n + i] = val[i];
    return wifi_wlcmd(1, WLC_SET_VAR, b, n + vlen, NULL, 0);
}

/* iovar SET with a 4-byte int value. */
static int wifi_set_iovar_int(const char *name, uint32_t v)
{
    uint8_t b[4];
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
    return wifi_set_iovar(name, b, 4);
}

/* Upload the regulatory (CLM) blob via the "clmload" iovar.  Without valid
 * regulatory data the firmware refuses "country" (-2) and the radio won't
 * come up for scanning (-4).  Packet = flag[2] type[2] len[4] crc[4] data
 * (plan9 reguload); chunked at 1400 bytes. */
#define CLM_CHUNK   1400
#define CLM_FLAG    0x1000          /* DL_BEGIN-ish: clm data flag */
#define CLM_FIRST   0x0002
#define CLM_LAST    0x0004
static int wifi_clmload(void)
{
    static uint8_t buf[12 + CLM_CHUNK];
    uint32_t total = (uint32_t)(wifi_clm_blob_end - wifi_clm_blob);
    uint32_t off = 0;
    int flag = CLM_FLAG | CLM_FIRST;

    wifi_log("[wifi] clmload: %u bytes regulatory\r\n", total);
    while (off < total || flag == (CLM_FLAG | CLM_FIRST)) {
        uint32_t n = total - off;
        int i, pad;
        if (n > CLM_CHUNK) n = CLM_CHUNK;
        else flag |= CLM_LAST;
        for (i = 0; i < (int)n; i++) buf[12 + i] = wifi_clm_blob[off + i];
        pad = 0;
        if (flag & CLM_LAST) while ((n + pad) & 7) buf[12 + n + pad++] = 0;
        buf[0] = flag; buf[1] = flag >> 8;        /* flag */
        buf[2] = 2; buf[3] = 0;                   /* type = 2 */
        buf[4] = n; buf[5] = n>>8; buf[6] = n>>16; buf[7] = n>>24;   /* len */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;  /* crc (unused) */
        if (wifi_set_iovar("clmload", buf, 12 + n + pad) != 0) {
            wifi_log("[wifi] clmload: chunk at %u failed\r\n", off);
            return -1;
        }
        off += n;
        flag &= ~CLM_FIRST;
        if (off >= total) break;
    }
    wifi_log("[wifi] clmload: done\r\n");
    return 0;
}

/* ================================================================== *
 *  Stage 5 — scan for access points (escan + event parsing)         *
 * ================================================================== */
#define WLC_PASSIVE_SCAN 49
#define WLC_E_ESCAN_RESULT 69
#define WLC_E_SCAN_COMPLETE 26

/* escan_params template (little-endian; plan9 ether4330.c wlscanstart):
 * version=1 action=START(1) sync_id=0x1234, ssid(any), bssid=ff..ff,
 * bss_type=any(2), defaults, 14 2.4GHz channels, 1 (any) ssid. */
static const uint8_t escan_params[] = {
    1,0,0,0,            /* version */
    1,0,                /* action = WL_SCAN_ACTION_START */
    0x34,0x12,          /* sync_id */
    0,0,0,0,            /* ssid len */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* ssid[32] */
    0xff,0xff,0xff,0xff,0xff,0xff,   /* bssid = broadcast (any) */
    2,                  /* bss_type = any */
    0,                  /* scan_type = active */
    0xff,0xff,0xff,0xff, /* nprobes (default) */
    0xff,0xff,0xff,0xff, /* active_time */
    0xff,0xff,0xff,0xff, /* passive_time */
    0xff,0xff,0xff,0xff, /* home_time */
    14,0,               /* nchannels */
    0,0,                /* nssids = 0 (broadcast scan; no directed-SSID entries
                           — avoids the per-ssid 36-byte buffer requirement
                           that made fw 7.45.265 return BCME_BUFTOOSHORT -14) */
    0x01,0x2b,0x02,0x2b,0x03,0x2b,0x04,0x2b,0x05,0x2e,0x06,0x2e,0x07,0x2e,
    0x08,0x2b,0x09,0x2b,0x0a,0x2b,0x0b,0x2b,0x0c,0x2b,0x0d,0x2b,0x0e,0x2b,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* ssid */
};

/* Read one SDPCM frame from the Fn2 FIFO into `buf` (cap bytes).
 * Returns the frame length (>=12), 0 if nothing queued, -1 on error.
 * On success *chan = channel, *doff = data offset. */
static int wifi_read_frame(uint8_t *buf, int cap, int *chan, int *doff)
{
    int plen, lenck;
    if (wifi_packetrw(0, buf, SDPCM_HDR) != 0) return -1;
    plen = buf[0] | (buf[1] << 8);
    lenck = buf[2] | (buf[3] << 8);
    if (plen == 0) return 0;
    if (lenck != ((plen ^ 0xFFFF) & 0xFFFF) || plen < SDPCM_HDR || plen > cap)
        return 0;                              /* junk; treat as empty */
    *chan = buf[5] & 0xF;
    *doff = buf[7];
    if (plen > SDPCM_HDR)
        if (wifi_packetrw(0, buf + SDPCM_HDR, plen - SDPCM_HDR) != 0) return -1;
    return plen;
}

/* Run a scan and append the discovered APs to the trace.  out_count gets the
 * number of unique APs found.  Returns 0 on success. */
/* One-time radio bring-up needed before scan/join: enable fw events, load
 * regulatory (CLM), set country + power management.  Idempotent. */
static int wifi_radio_done = 0;
static void wifi_radio_up(void)
{
    int i;
    if (wifi_radio_done) return;
    {
        uint8_t em[16];                           /* enable all fw events */
        for (i = 0; i < 16; i++) em[i] = 0xFF;
        if (wifi_set_iovar("event_msgs", em, 16) != 0)
            wifi_log("[wifi] radio: event_msgs failed (continuing)\r\n");
        else
            wifi_log("[wifi] radio: event_msgs enabled\r\n");
    }
    wifi_cmd_int(0x56, 0);                         /* WLC_SET_PM = 0 */
    if (wifi_clmload() != 0)                       /* regulatory firmware */
        wifi_log("[wifi] radio: clmload failed (continuing)\r\n");
    {
        uint8_t cc[12];
        for (i = 0; i < 12; i++) cc[i] = 0;
        cc[0] = 'U'; cc[1] = 'S';
        cc[4] = cc[5] = cc[6] = cc[7] = 0xFF;      /* revision = -1 */
        cc[8] = 'U'; cc[9] = 'S';
        if (wifi_set_iovar("country", cc, 12) != 0)
            wifi_log("[wifi] radio: set country failed (continuing)\r\n");
    }
    if (wifi_set_iovar_int("mpc", 0) != 0)
        wifi_log("[wifi] radio: set mpc=0 failed (continuing)\r\n");
    wifi_radio_done = 1;
}

static int wifi_scan(int *out_count)
{
    static uint8_t fr[2048];
    static uint8_t seen[32][6];     /* dedup by BSSID */
    int nseen = 0, tries, chan, doff, i;

    *out_count = 0;
    wifi_radio_up();
    if (wifi_cmd_int(2, 1) != 0)                  /* WLC_UP */
        wifi_log("[wifi] scan: WLC_UP returned error (continuing)\r\n");
    else
        wifi_log("[wifi] scan: WLC_UP ok\r\n");
    wifi_delay_us(150000);
    wifi_cmd_int(WLC_PASSIVE_SCAN, 0);
    if (wifi_set_iovar("escan", escan_params, sizeof(escan_params)) != 0) {
        wifi_log("[wifi] scan: escan start failed\r\n");
        return -1;
    }
    wifi_log("[wifi] scan: escan started, collecting results...\r\n");

    {
    int nframes = 0, nchan1 = 0, nev = 0;
    for (tries = 0; tries < 2000; tries++) {     /* ~up to several seconds */
        uint8_t *evp, *escan, *bss;
        int len, event, nbss, bdc;

        len = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
        if (len < 0) break;
        if (len == 0) { wifi_delay_us(5000); continue; }
        nframes++;
        if (chan != 1) {
            if (nframes <= 12) wifi_log("[wifi] scan rx: chan=%d len=%d (skip)\r\n", chan, len);
            continue;
        }
        nchan1++;
        if (len < doff + 4) continue;
        bdc = 4 + (fr[doff + 3] << 2);
        evp = fr + doff + bdc;                    /* 802.3 event frame */
        if ((int)(evp - fr) + 24 + 64 > len) continue;
        event = (evp[24 + 6] << 8) | evp[24 + 7]; /* big-endian event number */
        if (nev < 12) { wifi_log("[wifi] scan rx: chan1 len=%d doff=%d bdc=%d event=%d\r\n",
                                 len, doff, bdc, event); nev++; }
        if (event == WLC_E_SCAN_COMPLETE) { wifi_log("[wifi] scan: complete event\r\n"); break; }
        if (event != WLC_E_ESCAN_RESULT) continue;
        escan = evp + 24 + 48;
        if ((int)(escan - fr) + 12 > len) continue;
        nbss = escan[10] | (escan[11] << 8);
        if (nbss == 0) { wifi_log("[wifi] scan: escan done (nbss=0)\r\n"); break; }
        bss = escan + 12;
        if ((int)(bss - fr) + 82 > len) continue;
        {
            uint8_t *bssid = bss + 8;
            int ssidlen = bss[18]; if (ssidlen > 32) ssidlen = 32;
            short rssi = (short)(bss[78] | (bss[79] << 8));
            int ch = (bss[72] | (bss[73] << 8)) & 0xFF;
            int dup = 0;
            for (i = 0; i < nseen; i++)
                if (seen[i][0]==bssid[0]&&seen[i][1]==bssid[1]&&seen[i][2]==bssid[2]&&
                    seen[i][3]==bssid[3]&&seen[i][4]==bssid[4]&&seen[i][5]==bssid[5]) { dup=1; break; }
            if (dup) continue;
            if (nseen < 32) { for (i=0;i<6;i++) seen[nseen][i]=bssid[i]; nseen++; }
            {
                char ssid[33];
                for (i = 0; i < ssidlen; i++) {
                    uint8_t c = bss[19 + i];
                    ssid[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
                }
                ssid[ssidlen] = '\0';
                wifi_log("[wifi] AP %2d: \"%s\" %02x:%02x:%02x:%02x:%02x:%02x ch=%d rssi=%d\r\n",
                         nseen, ssidlen ? ssid : "(hidden)",
                         bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5], ch, rssi);
            }
        }
    }
    wifi_log("[wifi] scan: frames=%d chan1=%d (collected %d AP)\r\n",
             nframes, nchan1, nseen);
    }
    *out_count = nseen;
    return 0;
}

/* ================================================================== *
 *  Bring the chip fully up (Stages 1-4): SDIO host, chip detect,     *
 *  firmware download, SDPCM ioctl.  Idempotent per boot (wifi_ready). *
 * ================================================================== */
static int wifi_ready = 0;
static int wifi_bringup(void)
{
    uint32_t ocr = 0, rca = 0, chipid = 0;
    int r, ioe;

    if (wifi_ready) { wifi_log("[wifi] (chip already up)\r\n"); return 0; }
    wifi_log("[wifi] === BCM43455 SDIO bring-up ===\r\n");
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

    /* 6b. set SDIO function block sizes.  Block-mode CMD53 (used by the bulk
     * backplane transfers / firmware download) frames data into fixed-size
     * blocks each with its own CRC16; without programming the function block
     * size the device CRC-mismatches (the b8 DCRC error 0x208000).  plan9
     * uses 64 for F1 (backplane) and 512 for F2 (WLAN data).
     * FBR1 blksize @ CCCR 0x110, FBR2 @ 0x210. */
    sdio_cmd52(0, 0x110, 1, 64);
    sdio_cmd52(0, 0x111, 1, 0);
    sdio_cmd52(0, 0x210, 1, 512 & 0xFF);
    sdio_cmd52(0, 0x211, 1, (512 >> 8) & 0xFF);
    wifi_log("[wifi] F1 blksize=64, F2 blksize=512\r\n");

    /* 7. read the silicon chip-id over the backplane (chipcommon offset 0). */
    if (wifi_backplane_read32(SI_ENUM_BASE, &chipid) != 0) {
        wifi_log("[wifi] FAILED: backplane chip-id read\r\n");
        return -1;
    }
    wifi_log("[wifi] chipcommon[0]=0x%08x  chip-id=0x%04x  rev=%d\r\n",
            chipid, chipid & CID_ID_MASK, (chipid >> 16) & 0xF);

    if ((chipid & CID_ID_MASK) != BCM43455_CHIP_ID) {
        wifi_log("[wifi] chip-id 0x%04x is not the expected 0x4345 "
                "(check window/access path)\r\n", chipid & CID_ID_MASK);
        return -1;
    }
    wifi_log("[wifi] BCM43455 (0x4345) detected over SDIO\r\n");

    /* ---- Stage 3a: enumerate the chip's cores + on-chip RAM ----
     * Validates the bulk backplane path (CMD53 PIO + sbmem) by reading the
     * 512-byte EROM and the CR4 RAM banks.  This is the foundation for the
     * firmware download (Stage 3b). */
    if (wifi_corescan() != 0) {
        wifi_log("[wifi] FAILED: corescan (EROM bulk read)\r\n");
        return -1;
    }
    wifi_log("[wifi] cores: chipcommon=0x%x armcore=0x%x armctl=0x%x armregs=0x%x\r\n",
             chip_chipcommon, chip_armcore, chip_armctl, chip_armregs);
    wifi_log("[wifi]        d11ctl=0x%x socramctl=0x%x sdregs=0x%x\r\n",
             chip_d11ctl, chip_socramctl, chip_sdregs);
    if (chip_armctl == 0 || chip_d11ctl == 0) {
        wifi_log("[wifi] FAILED: corescan missing essential cores\r\n");
        return -1;
    }
    if (wifi_ramscan() != 0) {
        wifi_log("[wifi] FAILED: ramscan\r\n");
        return -1;
    }
    wifi_log("[wifi] RAM: base=0x%x size=%u bytes (%u KB)\r\n",
             chip_rambase, chip_socramsize, chip_socramsize / 1024);

    /* ---- Stage 3b: download firmware + start the ARM CR4 ----
     * sbinit-equivalent: halt the ARM core, reset the d11 (MAC) core, then
     * force the ALP clock; then fwload writes fw+nvram and releases the CR4;
     * sbenable brings up the HT clock and enables WLAN function 2. */
    sb_reset(chip_armctl, CR4_CPUHALT, CR4_CPUHALT);   /* halt CR4 */
    sb_reset(chip_d11ctl, 8 | 4, 4);                   /* reset d11 MAC */
    cfgw(REG_CLKCSR, 0);
    wifi_delay_us(10);
    cfgw(REG_CLKCSR, CLK_NOHWREQ | CLK_REQALP);
    for (r = 0; r < 2000 && !(cfgr(REG_CLKCSR) & (CLK_HTAVAIL | CLK_ALPAVAIL)); r++)
        wifi_delay_us(10);
    cfgw(REG_CLKCSR, CLK_NOHWREQ | CLK_FORCEALP);
    wifi_delay_us(65);
    cfgw(REG_PULLUPS, 0);
    wifi_backplane_write32(chip_chipcommon + 0x58, 0);   /* gpio pullup   */
    wifi_backplane_write32(chip_chipcommon + 0x5c, 0);   /* gpio pulldown */
    wifi_log("[wifi] chip halted + ALP clock forced (csr=0x%02x)\r\n", cfgr(REG_CLKCSR));

    if (wifi_fwload() != 0) {
        wifi_log("[wifi] FAILED: firmware download\r\n");
        return -1;
    }
    if (wifi_sbenable() != 0) {
        wifi_log("[wifi] FAILED: sbenable (HT clock / Fn2)\r\n");
        return -1;
    }
    wifi_log("[wifi] Stage 3 — firmware loaded, chip running\r\n");

    /* ---- Stage 4: SDPCM control channel — ask the firmware its version ----
     * A successful "ver" iovar GET proves the host<->firmware ioctl path
     * (SDPCM framing + BCDC control header over Fn2) works end to end. */
    {
        static uint8_t ver[128];
        int i;
        for (i = 0; i < (int)sizeof(ver); i++) ver[i] = 0;
        wifi_delay_us(50000);       /* let the firmware settle after boot */
        if (wifi_get_iovar("ver", ver, sizeof(ver) - 1) != 0) {
            wifi_log("[wifi] FAILED: SDPCM ioctl (get 'ver')\r\n");
            return -1;
        }
        /* trim the version string to its first line for the trace */
        for (i = 0; i < (int)sizeof(ver) - 1; i++)
            if (ver[i] == '\r' || ver[i] == '\n') { ver[i] = '\0'; break; }
        wifi_log("[wifi] firmware version: %s\r\n", (char *)ver);
        wifi_log("[wifi] Stage 4 — SDPCM ioctl works (fw responds)\r\n");
    }
    wifi_ready = 1;
    return 0;
}

/* ================================================================== *
 *  SHA1 / HMAC-SHA1 / PBKDF2 — to derive the WPA2 PMK from the        *
 *  passphrase (PMK = PBKDF2-SHA1(passphrase, ssid, 4096, 32)).        *
 * ================================================================== */
struct sha1 { uint32_t h[5]; uint64_t len; uint8_t buf[64]; int n; };

static uint32_t sha1_rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(struct sha1 *c, const uint8_t *p)
{
    uint32_t w[80], a, b, d, e, f, k, t; int i;
    for (i = 0; i < 16; i++)
        w[i] = (p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
    for (i = 16; i < 80; i++) w[i] = sha1_rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    a=c->h[0]; b=c->h[1]; d=c->h[2]; e=c->h[3]; f=c->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t fn;
        if (i < 20)      { fn = (b & d) | (~b & e); k = 0x5A827999; }
        else if (i < 40) { fn = b ^ d ^ e;          k = 0x6ED9EBA1; }
        else if (i < 60) { fn = (b&d)|(b&e)|(d&e);  k = 0x8F1BBCDC; }
        else             { fn = b ^ d ^ e;          k = 0xCA62C1D6; }
        t = sha1_rol(a,5) + fn + f + k + w[i];
        f = e; e = d; d = sha1_rol(b,30); b = a; a = t;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=d; c->h[3]+=e; c->h[4]+=f;
}

static void sha1_init(struct sha1 *c)
{
    c->h[0]=0x67452301; c->h[1]=0xEFCDAB89; c->h[2]=0x98BADCFE;
    c->h[3]=0x10325476; c->h[4]=0xC3D2E1F0; c->len=0; c->n=0;
}

static void sha1_update(struct sha1 *c, const uint8_t *p, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        c->buf[c->n++] = p[i];
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
    c->len += len;
}

static void sha1_final(struct sha1 *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8; int i;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) sha1_update(c, &pad, 1);
    for (i = 7; i >= 0; i--) { uint8_t b = (bits >> (i*8)) & 0xFF; sha1_update(c, &b, 1); }
    for (i = 0; i < 5; i++) {
        out[i*4]   = c->h[i] >> 24; out[i*4+1] = c->h[i] >> 16;
        out[i*4+2] = c->h[i] >> 8;  out[i*4+3] = c->h[i];
    }
}

/* HMAC-SHA1(key, msg) -> 20-byte mac */
static void hmac_sha1(const uint8_t *key, int klen, const uint8_t *msg, int mlen,
                      uint8_t mac[20])
{
    uint8_t k[64], ipad[64], opad[64], inner[20];
    struct sha1 c; int i;
    for (i = 0; i < 64; i++) k[i] = 0;
    if (klen > 64) { sha1_init(&c); sha1_update(&c, key, klen); sha1_final(&c, k); }
    else for (i = 0; i < klen; i++) k[i] = key[i];
    for (i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    sha1_init(&c); sha1_update(&c, ipad, 64); sha1_update(&c, msg, mlen); sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20); sha1_final(&c, mac);
}

/* PBKDF2-SHA1: derive `dklen` bytes from passphrase + salt (ssid), 4096 iters. */
static void pbkdf2_sha1(const uint8_t *pass, int plen, const uint8_t *salt, int slen,
                        int iter, uint8_t *dk, int dklen)
{
    int block = 1, off = 0;
    while (off < dklen) {
        uint8_t salt_i[40], u[20], t[20]; int i, j, cpy;
        for (i = 0; i < slen && i < 36; i++) salt_i[i] = salt[i];
        salt_i[slen]   = block >> 24; salt_i[slen+1] = block >> 16;
        salt_i[slen+2] = block >> 8;  salt_i[slen+3] = block;
        hmac_sha1(pass, plen, salt_i, slen + 4, u);
        for (i = 0; i < 20; i++) t[i] = u[i];
        for (j = 1; j < iter; j++) {
            hmac_sha1(pass, plen, u, 20, u);
            for (i = 0; i < 20; i++) t[i] ^= u[i];
        }
        cpy = (dklen - off < 20) ? (dklen - off) : 20;
        for (i = 0; i < cpy; i++) dk[off + i] = t[i];
        off += cpy; block++;
    }
}

/* ================================================================== *
 *  Stage 6 — join a (WPA2-PSK) access point                          *
 * ================================================================== */
#define WLC_DOWN        3
#define WLC_SET_INFRA   20
#define WLC_SET_WSEC    134
#define WLC_SET_WPA_AUTH 165
#define WLC_SET_WSEC_PMK 268
#define WLC_SET_SSID    26

/* Derive the WPA2 PMK (PBKDF2-SHA1 of passphrase+ssid) and give it to the
 * firmware via WLC_SET_WSEC_PMK.  This fw rejected the passphrase-flag form
 * (-2), so we send the real 32-byte PMK (flags=0). */
static int wifi_set_pmk(const char *ssid, int slen, const char *pass, int plen)
{
    uint8_t pmk32[32];
    uint8_t msg[2 + 2 + 32];              /* brcmf_wsec_pmk_le: key[32] => 36 B */
    int i;
    pbkdf2_sha1((const uint8_t *)pass, plen, (const uint8_t *)ssid, slen, 4096, pmk32, 32);
    wifi_log("[wifi] join: PMK %02x%02x%02x%02x...%02x%02x derived\r\n",
             pmk32[0], pmk32[1], pmk32[2], pmk32[3], pmk32[30], pmk32[31]);
    for (i = 0; i < (int)sizeof(msg); i++) msg[i] = 0;
    msg[0] = 32; msg[1] = 0;              /* key_len = 32 (raw PMK) */
    msg[2] = 0;  msg[3] = 0;              /* flags = 0 */
    for (i = 0; i < 32; i++) msg[4 + i] = pmk32[i];
    return wifi_wlcmd(1, WLC_SET_WSEC_PMK, msg, sizeof(msg), NULL, 0);
}

/* Join an AP.  pass==NULL/"" => open network; otherwise WPA2-PSK. */
static int wifi_do_join(const char *ssid, const char *pass)
{
    uint8_t jp[114];
    int sl = 0, i, ev, secured = (pass && pass[0]), pl = 0;

    while (ssid[sl] && sl < 32) sl++;
    if (pass) while (pass[pl] && pl < 63) pl++;
    wifi_log("[wifi] join: ssid=\"%s\" %s\r\n", ssid, secured ? "WPA2-PSK" : "open");

    wifi_radio_up();                             /* event_msgs/clm/country/pm */
    wifi_cmd_int(WLC_DOWN, 1);
    wifi_cmd_int(WLC_SET_INFRA, 1);
    if (secured) {
        wifi_cmd_int(WLC_SET_WSEC, 4);          /* AES (while down) */
        wifi_set_iovar_int("wpa_auth", 0x80);   /* WPA2-PSK */
        wifi_cmd_int(165 /*WLC_SET_WPA_AUTH*/, 0x80);
    } else {
        wifi_cmd_int(WLC_SET_WSEC, 0);
        wifi_set_iovar_int("wpa_auth", 0);
    }
    wifi_cmd_int(2, 1);                          /* WLC_UP */
    wifi_delay_us(50000);
    /* set the PMK while the interface is UP (WSEC_PMK -2'd when down) */
    if (secured && wifi_set_pmk(ssid, sl, pass, pl) != 0)
        wifi_log("[wifi] join: WSEC_PMK returned error (continuing)\r\n");

    /* build the extended "join" iovar (brcmf_ext_join_params_le, 114 B):
     *   [0..35]   ssid_le (len + ssid[32])
     *   [36..101] scan_params (ssid + bssid + bss_type + scan_type + nprobes
     *             + active/passive/home + channel_num + channel_list[1])
     *   [102..113] assoc_params (bssid + chanspec_num + chanspec_list[1]) */
    for (i = 0; i < (int)sizeof(jp); i++) jp[i] = 0;
    jp[0] = sl;
    for (i = 0; i < sl; i++) jp[4 + i] = ssid[i];
    /* scan_params.ssid (directed) */
    jp[36] = sl;
    for (i = 0; i < sl; i++) jp[40 + i] = ssid[i];
    for (i = 72; i < 78; i++) jp[i] = 0xFF;      /* scan bssid = broadcast */
    jp[78] = 2;                                   /* bss_type = any */
    jp[79] = 0;                                   /* scan_type = active */
    for (i = 80; i < 96; i++) jp[i] = 0xFF;      /* nprobes/active/passive/home = -1 */
    /* [96..99] channel_num = 0, [100..101] channel_list[0] = 0 */
    for (i = 102; i < 108; i++) jp[i] = 0xFF;    /* assoc bssid = broadcast */
    /* [108..111] chanspec_num = 0, [112..113] chanspec_list[0] = 0 */
    if (wifi_set_iovar("join", jp, sizeof(jp)) != 0) {
        wifi_log("[wifi] join: 'join' iovar failed\r\n");
        return -1;
    }
    wifi_log("[wifi] join: request sent, waiting for link event...\r\n");

    /* Observe association events (channel 1) and whether the AP's EAPOL 4-way
     * handshake frames (channel 2 data, ethertype 0x888E) are forwarded to the
     * host — the viability test for a host-side supplicant. */
    {
        static uint8_t fr[2048];
        int chan, doff, tries, nev = 0, ndata = 0, eapol = 0;
        for (tries = 0; tries < 1500; tries++) {
            int len = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
            if (len < 0) break;
            if (len == 0) { wifi_delay_us(10000); continue; }
            if (len < doff + 4) continue;
            {
                int bdc = 4 + (fr[doff + 3] << 2);
                uint8_t *p = fr + doff + bdc;     /* 802.3 frame */
                if ((int)(p - fr) + 16 > len) continue;
                if (chan == 1) {                  /* event */
                    long status = (p[24+8]<<24)|(p[24+9]<<16)|(p[24+10]<<8)|p[24+11];
                    ev = (p[24 + 6] << 8) | p[24 + 7];
                    if (nev++ < 16) wifi_log("[wifi] join: event %d status %ld\r\n", ev, status);
                    if (ev == 16) {
                        int up = (p[24+2] << 8 | p[24+3]) & 1;
                        wifi_log("[wifi] join: E_LINK %s\r\n", up ? "UP" : "down");
                        if (up) { wifi_log("[wifi] *** associated (link up) ***\r\n"); return 0; }
                    } else if (ev == 5 || ev == 6 || ev == 12 || ev == 24) {
                        wifi_log("[wifi] join: deauth/disassoc event %d\r\n", ev);
                    }
                } else if (chan == 2) {           /* data frame */
                    int et = (p[12] << 8) | p[13];
                    ndata++;
                    if (et == 0x888E) {           /* EAPOL — the 4-way handshake */
                        eapol++;
                        wifi_log("[wifi] join: EAPOL frame #%d (len=%d) — AP started 4-way!\r\n",
                                 eapol, len);
                        if (eapol == 1)
                            wifi_log("[wifi] *** host-supplicant VIABLE: fw forwards EAPOL ***\r\n");
                    } else if (ndata <= 6) {
                        wifi_log("[wifi] join: data frame ethertype=0x%04x len=%d\r\n", et, len);
                    }
                }
            }
        }
        wifi_log("[wifi] join: timeout (events=%d data=%d eapol=%d)\r\n", nev, ndata, eapol);
    }
    return -1;
}

/* ================================================================== *
 *  Public entry points (called from the HTTP routes in webactor.c)   *
 * ================================================================== */
int wifi_probe(void)
{
    int n = 0;
    wifi_tn = 0;
    if (wifi_bringup() != 0) return -1;
    if (wifi_scan(&n) != 0) { wifi_log("[wifi] FAILED: scan\r\n"); return -1; }
    wifi_log("[wifi] *** SUCCESS: scan found %d access point(s) ***\r\n", n);
    return 0;
}

/* Scan and emit the AP list as JSON into out (for the selection UI). */
int wifi_scan_json(char *out, int cap)
{
    /* run a fresh scan (bring-up is cached), then format wifi_tbuf's AP lines */
    int n = 0, len = 0, i;
    wifi_tn = 0;
    if (wifi_bringup() != 0) return sprintf(out, "{\"error\":\"bringup failed\"}");
    wifi_scan(&n);
    /* parse the "[wifi] AP  N: \"ssid\" bssid ch=.. rssi=.." lines from the trace */
    len += sprintf(out + len, "{\"aps\":[");
    {
        const char *t = wifi_tbuf;
        int first = 1;
        for (i = 0; t[i] && len < cap - 200; i++) {
            if (t[i]=='A'&&t[i+1]=='P'&&t[i+2]==' ') {
                const char *q = &t[i];
                int j = 0; char line[160];
                while (q[j] && q[j] != '\r' && q[j] != '\n' && j < 159) { line[j]=q[j]; j++; }
                line[j] = '\0';
                len += sprintf(out + len, "%s\"%s\"", first ? "" : ",", line);
                first = 0;
                i += j;
            }
        }
    }
    len += sprintf(out + len, "],\"count\":%d}", n);
    return len;
}

/* Join (connect to) an access point. */
int wifi_join(const char *ssid, const char *pass)
{
    wifi_tn = 0;
    if (wifi_bringup() != 0) return -1;
    return wifi_do_join(ssid, pass);
}
