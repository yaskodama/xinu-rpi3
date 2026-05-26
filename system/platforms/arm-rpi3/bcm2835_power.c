/**
 * @file bcm2835_power.c
 *
 * This driver provides the ability to power on and power off hardware, such as
 * the USB Controller, on the BCM2835 SoC used on the Raspberry Pi.  This makes
 * use of the BCM2835's mailbox mechanism.
 */
#include "bcm2835.h"

static volatile uint *const mailbox_regs = (volatile uint*)MAILBOX_REGS_BASE;

/* BCM2835 mailbox register indices  */
#define MAILBOX_READ               0
#define MAILBOX_STATUS             6
#define MAILBOX_WRITE              8

/* BCM2835 mailbox status flags  */
#define MAILBOX_FULL               0x80000000
#define MAILBOX_EMPTY              0x40000000

/* BCM2835 mailbox channels  */
#define MAILBOX_CHANNEL_POWER_MGMT 0

/* The BCM2835 mailboxes are used for passing 28-bit messages.  The low 4 bits
 * of the 32-bit value are used to specify the channel over which the message is
 * being transferred  */
#define MAILBOX_CHANNEL_MASK       0xf

/* Write to the specified channel of the mailbox.  */
static void
bcm2835_mailbox_write(uint channel, uint value)
{
    while (mailbox_regs[MAILBOX_STATUS] & MAILBOX_FULL)
    {
    }
    mailbox_regs[MAILBOX_WRITE] = (value & ~MAILBOX_CHANNEL_MASK) | channel;
}

/* Read from the specified channel of the mailbox.  */
static uint
bcm2835_mailbox_read(uint channel)
{
    uint value;

    while (mailbox_regs[MAILBOX_STATUS] & MAILBOX_EMPTY)
    {
    }
    do
    {
        value = mailbox_regs[MAILBOX_READ];
    } while ((value & MAILBOX_CHANNEL_MASK) != channel);
    return (value & ~MAILBOX_CHANNEL_MASK);
}

/* Retrieve the bitmask of power on/off state.  */
static uint
bcm2835_get_power_mask(void)
{
    return (bcm2835_mailbox_read(MAILBOX_CHANNEL_POWER_MGMT) >> 4);
}

/* Set the bitmask of power on/off state.  */
static void
bcm2835_set_power_mask(uint mask)
{
    bcm2835_mailbox_write(MAILBOX_CHANNEL_POWER_MGMT, mask << 4);
}

/* Bitmask that gives the current on/off state of the BCM2835 hardware.
 * This is a cached value.  */
static uint bcm2835_power_mask;

/**
 * Powers on or powers off BCM2835 hardware.
 *
 * @param feature
 *      Device or hardware to power on or off.
 * @param on
 *      ::TRUE to power on; ::FALSE to power off.
 *
 * @return
 *      ::OK if successful; ::SYSERR otherwise.
 */
#ifdef _XINU_PLATFORM_ARM_RPI3_

/* Pi3 (BCM2837): the legacy channel-0 power-management mailbox is gone.
 * Power devices on/off via the VideoCore *property* mailbox (channel 8)
 * using the SET_POWER_STATE tag.  POWER_USB (==3) maps directly to the
 * property device id for the USB HCD. */
#define MAILBOX_CHANNEL_PROPERTY 8

/* 16-byte-aligned property message buffer (low RAM; D-cache is off). */
static volatile uint prop_buf[8] __attribute__((aligned(16)));

int bcm2835_setpower(enum board_power_feature feature, bool on)
{
    volatile uint *b = prop_buf;
    uint addr, val;

    b[0] = 8 * 4;            /* total buffer size in bytes        */
    b[1] = 0;                /* request code                      */
    b[2] = 0x00028001;       /* tag: SET_POWER_STATE              */
    b[3] = 8;                /* value buffer size (2 words)       */
    b[4] = 0;                /* tag request code                  */
    b[5] = (uint)feature;    /* device id (POWER_USB == 3 == HCD) */
    b[6] = on ? 0x3 : 0x2;   /* bit0 = on/off, bit1 = wait stable */
    b[7] = 0;                /* end tag                           */

    /* Hand the GPU the uncached bus alias so it reads RAM directly
     * (Xinu runs with the D-cache off). */
    addr = (uint)prop_buf + 0xC0000000;

    while (mailbox_regs[MAILBOX_STATUS] & MAILBOX_FULL)
        ;
    mailbox_regs[MAILBOX_WRITE] =
        (addr & ~MAILBOX_CHANNEL_MASK) | MAILBOX_CHANNEL_PROPERTY;

    do
    {
        while (mailbox_regs[MAILBOX_STATUS] & MAILBOX_EMPTY)
            ;
        val = mailbox_regs[MAILBOX_READ];
    } while ((val & MAILBOX_CHANNEL_MASK) != MAILBOX_CHANNEL_PROPERTY);

    return (b[1] == 0x80000000) ? OK : SYSERR;
}

void bcm2835_power_init(void)
{
    /* The property interface needs no pre-clear of a power mask. */
}

#else  /* Pi1 (BCM2835): legacy channel-0 power-management mailbox */

int bcm2835_setpower(enum board_power_feature feature, bool on)
{
    uint bit;
    uint newmask;
    bool is_on;

    bit = 1 << feature;
    is_on = (bcm2835_power_mask & bit) != 0;
    if (on != is_on)
    {
        newmask = bcm2835_power_mask ^ bit;
        bcm2835_set_power_mask(newmask);
        bcm2835_power_mask = bcm2835_get_power_mask();
        if (bcm2835_power_mask != newmask)
        {
            return SYSERR;
        }
    }
    return OK;
}

/**
 * Resets BCM2835 power to default state (all devices powered off).
 */
void bcm2835_power_init(void)
{
    bcm2835_set_power_mask(0);
    bcm2835_power_mask = 0;
}

#endif
