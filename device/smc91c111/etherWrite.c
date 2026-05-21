/**
 * @file etherWrite.c
 *
 * Transmit a packet on the SMSC LAN91C111.  The chip exposes an internal
 * 8KB RAM divided into 256-byte pages.  Sending a frame is:
 *
 *   1. MMU_ALLOC_TX  with the required page count.
 *   2. Poll PNR.ARR until allocation completes (or fails).
 *   3. Move allocated page into PNR.PN (current packet for I/O).
 *   4. Write status (0), byte-count, frame data, control byte via auto-inc
 *      pointer at 0x4000.
 *   5. MMU_ENQUEUE_TX hands the packet off to the MAC for transmission.
 */
#include "smc91c111.h"
#include <ether.h>
#include <interrupt.h>
#include <kernel.h>
#include <stddef.h>

#define ALLOC_TIMEOUT_LOOPS  200000

devcall etherWrite(device *devptr, const void *buf, uint len)
{
    struct ether     *ethptr;
    struct smc91c111 *chip;
    const uchar      *p = buf;
    irqmask im;
    uint pages;
    ushort byte_count;
    ushort arr;
    ushort packet_num;
    uint i;
    int timeout;

    ethptr = &ethertab[devptr->minor];
    if (ethptr->state != ETH_STATE_UP)
    {
        return SYSERR;
    }
    if (len < ETH_HEADER_LEN || len > ETH_MAX_PKT_LEN)
    {
        return SYSERR;
    }
    chip = ethptr->csr;
    kprintf("[smc91c111] tx len=%d\r\n", len);

    im = disable();

    /* Total bytes the chip stores: status(2) + count(2) + data + ctl(1) [+pad(1)] */
    if (len & 1)
    {
        byte_count = (ushort)(len + 5);  /* 4 hdr + 1 ctl-with-byte         */
    }
    else
    {
        byte_count = (ushort)(len + 6);  /* 4 hdr + 1 ctl + 1 pad           */
    }
    pages = (byte_count + 255) / 256;
    if (pages > 0)
    {
        pages -= 1;
    }

    /* Request memory */
    smc_select_bank(chip, 2);
    smc_write16(chip, SMC_MMU_CMD, (ushort)(MMU_ALLOC_TX | (pages & 0x1F)));

    /* Wait for allocation result.  QEMU's smc91c111 model leaks TX
     * pages (AUTO_RELEASE does not free them on completion); on alloc
     * failure, fall back to MMU_RESET_TX (= reset TX FIFO + free all
     * TX pages) and retry once. */
    for (timeout = ALLOC_TIMEOUT_LOOPS; timeout > 0; timeout--)
    {
        arr = (ushort)(smc_read16(chip, SMC_PNR) >> 8);
        if (!(arr & 0x80)) {
            break;
        }
    }
    if (timeout == 0)
    {
        smc_write16(chip, SMC_MMU_CMD, MMU_RESET_TX);
        /* wait briefly */
        { volatile int j = 1000; while (j--) { asm volatile("nop"); } }
        smc_write16(chip, SMC_MMU_CMD,
                    (ushort)(MMU_ALLOC_TX | (pages & 0x1F)));
        for (timeout = ALLOC_TIMEOUT_LOOPS; timeout > 0; timeout--)
        {
            arr = (ushort)(smc_read16(chip, SMC_PNR) >> 8);
            if (!(arr & 0x80)) {
                break;
            }
        }
        if (timeout == 0) {
            kprintf("[smc91c111] tx alloc still failed after reset, arr=0x%02x\r\n", arr);
            restore(im);
            return SYSERR;
        }
    }
    kprintf("[smc91c111] tx alloc ok arr=0x%02x\r\n", arr);
    packet_num = (ushort)(arr & 0x3F);

    /* Become the owner of that page */
    smc_write16(chip, SMC_PNR,
                (ushort)((smc_read16(chip, SMC_PNR) & 0xFF00) | packet_num));

    /* Auto-incrementing pointer into the TX area, write mode */
    smc_write16(chip, SMC_PTR, PTR_AUTOINCR);

    /* Status (will be filled by hardware on TX) and byte count */
    smc_write32(chip, SMC_DATA, ((ulong)byte_count << 16));

    /* Bulk-copy as 16-bit words */
    for (i = 0; i + 1 < len; i += 2)
    {
        ushort w = (ushort)(p[i] | (p[i + 1] << 8));
        smc_write16(chip, SMC_DATA, w);
    }

    /* Trailing control byte + optional last data byte (if len is odd) */
    if (len & 1)
    {
        /* ODD-byte flag (0x20) | last-data-byte in low half (datasheet: high
         * byte = control, low byte = last data byte for odd lengths). */
        smc_write16(chip, SMC_DATA, (ushort)(p[len - 1] | (0x20 << 8)));
    }
    else
    {
        smc_write16(chip, SMC_DATA, 0x0000);
    }

    /* Hand off to the MAC */
    smc_write16(chip, SMC_MMU_CMD, MMU_ENQUEUE_TX);

    /* Poll INT_STAT for a brief moment to see what's happening */
    for (i = 0; i < 100000; i++)
    {
        ushort sm = smc_read16(chip, SMC_INT);
        if (sm & INT_TX)
        {
            kprintf("[smc91c111] tx done int=0x%04x mir=0x%04x\r\n",
                    sm, (smc_select_bank(chip, 0), smc_read16(chip, SMC_MIR)));
            smc_select_bank(chip, 2);
            break;
        }
    }
    if (i == 100000)
    {
        ulong vic_irq_status = *((volatile ulong *)0x10140000);
        ulong vic_rawintr    = *((volatile ulong *)0x10140008);
        ulong vic_enable     = *((volatile ulong *)0x10140010);
        kprintf("[smc91c111] tx never done int=0x%04x "
                "vic_status=0x%08x raw=0x%08x enable=0x%08x\r\n",
                smc_read16(chip, SMC_INT),
                (uint)vic_irq_status,
                (uint)vic_rawintr,
                (uint)vic_enable);
    }

    restore(im);
    return (devcall)len;
}
