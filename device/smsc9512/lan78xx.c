/**
 * @file lan78xx.c
 *
 * Microchip LAN78xx (LAN7800) USB Gigabit Ethernet support for the Raspberry
 * Pi 3 Model B+ onboard NIC.  This shares the single Xinu ETH0 device with the
 * older SMSC LAN9512 driver (smsc9512.c); only the register map and the TX/RX
 * framing differ.  See lan78xx.h for register definitions.
 *
 * Every init step prints a [lan78xx] diagnostic via kprintf so the sequence can
 * be followed on the serial console (we cannot observe the network on the Pi).
 */
/* Embedded Xinu. */

#include "lan78xx.h"
#include <clock.h>
#include <ether.h>
#include <kernel.h>
#include <usb_core_driver.h>

/* ----------------------------------------------------------------------- *
 * Register access helpers (USB vendor control transfers)                  *
 * ----------------------------------------------------------------------- */

usb_status_t
lan78xx_read_reg(struct usb_device *udev, uint32_t index, uint32_t *data)
{
    return usb_control_msg(udev, NULL,
                           LAN78XX_VENDOR_REQUEST_READ_REGISTER,
                           USB_BMREQUESTTYPE_DIR_IN |
                               USB_BMREQUESTTYPE_TYPE_VENDOR |
                               USB_BMREQUESTTYPE_RECIPIENT_DEVICE,
                           0, index, data, sizeof(uint32_t));
}

usb_status_t
lan78xx_write_reg(struct usb_device *udev, uint32_t index, uint32_t data)
{
    return usb_control_msg(udev, NULL,
                           LAN78XX_VENDOR_REQUEST_WRITE_REGISTER,
                           USB_BMREQUESTTYPE_DIR_OUT |
                               USB_BMREQUESTTYPE_TYPE_VENDOR |
                               USB_BMREQUESTTYPE_RECIPIENT_DEVICE,
                           0, index, &data, sizeof(uint32_t));
}

usb_status_t
lan78xx_modify_reg(struct usb_device *udev, uint32_t index,
                   uint32_t mask, uint32_t set)
{
    usb_status_t status;
    uint32_t val;

    status = lan78xx_read_reg(udev, index, &val);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    val &= mask;
    val |= set;
    return lan78xx_write_reg(udev, index, val);
}

usb_status_t
lan78xx_set_reg_bits(struct usb_device *udev, uint32_t index, uint32_t set)
{
    return lan78xx_modify_reg(udev, index, 0xffffffff, set);
}

/* ----------------------------------------------------------------------- *
 * MAC address                                                             *
 * ----------------------------------------------------------------------- */

usb_status_t
lan78xx_set_mac_address(struct usb_device *udev, const uint8_t *macaddr)
{
    usb_status_t status;
    uint32_t addrl, addrh;

    addrl = macaddr[0] | macaddr[1] << 8 | macaddr[2] << 16 | macaddr[3] << 24;
    addrh = macaddr[4] | macaddr[5] << 8;

    /* Program the perfect-filter MAC into RX_ADDRL/H ... */
    status = lan78xx_write_reg(udev, LAN78XX_RX_ADDRL, addrl);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    status = lan78xx_write_reg(udev, LAN78XX_RX_ADDRH, addrh);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }

    /* ... and into address-filter slot 0 (used by the DA_PERFECT filter). */
    status = lan78xx_write_reg(udev, LAN78XX_MAF_LO_0, addrl);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    return lan78xx_write_reg(udev, LAN78XX_MAF_HI_0,
                             LAN78XX_MAF_HI_AF_EN | addrh);
}

usb_status_t
lan78xx_get_mac_address(struct usb_device *udev, uint8_t *macaddr)
{
    usb_status_t status;
    uint32_t addrl, addrh;

    status = lan78xx_read_reg(udev, LAN78XX_RX_ADDRL, &addrl);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    status = lan78xx_read_reg(udev, LAN78XX_RX_ADDRH, &addrh);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    macaddr[0] = (addrl >> 0)  & 0xff;
    macaddr[1] = (addrl >> 8)  & 0xff;
    macaddr[2] = (addrl >> 16) & 0xff;
    macaddr[3] = (addrl >> 24) & 0xff;
    macaddr[4] = (addrh >> 0)  & 0xff;
    macaddr[5] = (addrh >> 8)  & 0xff;
    return USB_STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------- *
 * MII / internal PHY access via MII_ACC / MII_DATA                        *
 * ----------------------------------------------------------------------- */

/* Poll MII_BUSY clear with a bounded loop.  Returns USB_STATUS_SUCCESS or
 * USB_STATUS_TIMEOUT. */
static usb_status_t
lan78xx_mii_wait(struct usb_device *udev)
{
    uint32_t val;
    uint i;

    for (i = 0; i < 1000; i++)
    {
        if (lan78xx_read_reg(udev, LAN78XX_MII_ACC, &val) != USB_STATUS_SUCCESS)
        {
            return USB_STATUS_HARDWARE_ERROR;
        }
        if (!(val & LAN78XX_MII_ACC_BUSY))
        {
            return USB_STATUS_SUCCESS;
        }
        udelay(100);
    }
    return USB_STATUS_TIMEOUT;
}

static usb_status_t
lan78xx_mii_read(struct usb_device *udev, uint phy_reg, uint16_t *out)
{
    usb_status_t status;
    uint32_t acc, data;

    status = lan78xx_mii_wait(udev);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    acc = (LAN78XX_INTERNAL_PHY_ADDR << LAN78XX_MII_ACC_PHY_ADDR_SHIFT) |
          (phy_reg << LAN78XX_MII_ACC_MII_REG_SHIFT) |
          LAN78XX_MII_ACC_BUSY;   /* read: WRITE bit clear */
    status = lan78xx_write_reg(udev, LAN78XX_MII_ACC, acc);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    status = lan78xx_mii_wait(udev);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    status = lan78xx_read_reg(udev, LAN78XX_MII_DATA, &data);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    *out = data & 0xffff;
    return USB_STATUS_SUCCESS;
}

static usb_status_t
lan78xx_mii_write(struct usb_device *udev, uint phy_reg, uint16_t val)
{
    usb_status_t status;
    uint32_t acc;

    status = lan78xx_mii_wait(udev);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    status = lan78xx_write_reg(udev, LAN78XX_MII_DATA, val);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    acc = (LAN78XX_INTERNAL_PHY_ADDR << LAN78XX_MII_ACC_PHY_ADDR_SHIFT) |
          (phy_reg << LAN78XX_MII_ACC_MII_REG_SHIFT) |
          LAN78XX_MII_ACC_WRITE | LAN78XX_MII_ACC_BUSY;
    status = lan78xx_write_reg(udev, LAN78XX_MII_ACC, acc);
    if (status != USB_STATUS_SUCCESS)
    {
        return status;
    }
    return lan78xx_mii_wait(udev);
}

/* ----------------------------------------------------------------------- *
 * Bind-time init: chip ID, reset, MAC, filtering, FIFOs                    *
 * ----------------------------------------------------------------------- */

usb_status_t
lan78xx_bind(struct usb_device *udev, const uint8_t *macaddr)
{
    uint32_t v;
    uint i;
    bool ok;

    udev->last_error = USB_STATUS_SUCCESS;

    /* Step 1: read ID_REV (sanity: high half ~0x7800 for LAN7800). */
    v = 0;
    lan78xx_read_reg(udev, LAN78XX_ID_REV, &v);
    kprintf("[lan78xx] ID_REV=%08x\r\n", v);

    /* Step 2: lite reset (HW_CFG.LRST), poll until it self-clears. */
    lan78xx_set_reg_bits(udev, LAN78XX_HW_CFG, LAN78XX_HW_CFG_LRST);
    ok = FALSE;
    for (i = 0; i < 1000; i++)
    {
        v = 0;
        lan78xx_read_reg(udev, LAN78XX_HW_CFG, &v);
        if (!(v & LAN78XX_HW_CFG_LRST))
        {
            ok = TRUE;
            break;
        }
        udelay(1000);
    }
    kprintf("[lan78xx] HW_CFG LRST %s (HW_CFG=%08x)\r\n",
            ok ? "cleared" : "TIMEOUT", v);

    /* Step 3: PHY reset (PMT_CTL.PHY_RST), poll clear, then wait READY. */
    lan78xx_set_reg_bits(udev, LAN78XX_PMT_CTL, LAN78XX_PMT_CTL_PHY_RST);
    ok = FALSE;
    for (i = 0; i < 1000; i++)
    {
        v = 0;
        lan78xx_read_reg(udev, LAN78XX_PMT_CTL, &v);
        if (!(v & LAN78XX_PMT_CTL_PHY_RST))
        {
            ok = TRUE;
            break;
        }
        udelay(1000);
    }
    kprintf("[lan78xx] PMT_CTL PHY_RST %s (PMT_CTL=%08x)\r\n",
            ok ? "cleared" : "TIMEOUT", v);

    ok = FALSE;
    for (i = 0; i < 1000; i++)
    {
        v = 0;
        lan78xx_read_reg(udev, LAN78XX_PMT_CTL, &v);
        if (v & LAN78XX_PMT_CTL_READY)
        {
            ok = TRUE;
            break;
        }
        udelay(1000);
    }
    kprintf("[lan78xx] PMT_CTL READY %s (PMT_CTL=%08x)\r\n",
            ok ? "set" : "TIMEOUT", v);

    /* Step 4: program MAC address (RX_ADDRL/H + MAF_LO/HI(0)). */
    lan78xx_set_mac_address(udev, macaddr);
    kprintf("[lan78xx] MAC set to %02x:%02x:%02x:%02x:%02x:%02x\r\n",
            macaddr[0], macaddr[1], macaddr[2],
            macaddr[3], macaddr[4], macaddr[5]);

    /* Step 5: HW_CFG Multiple-Ethernet-Frames + USB_CFG Bulk-In Response, and
     * Step 6: Rx burst cap + bulk-in delay + FIFO end markers.
     *
     * This mirrors the proven-working smsc9512 path (which sets HW_CFG_MEF |
     * HW_CFG_BIR and a non-zero BURST_CAP).  CRITICAL: BURST_CAP must be the
     * burst-buffer size in USB packets (NOT 0).  With MEF set and BURST_CAP=0
     * the device emits empty bulk-IN responses back-to-back; our RX completion
     * callback re-submits immediately, so it spins and starves the system
     * (the serial shell goes dead the moment ETH0 is opened). */
    lan78xx_set_reg_bits(udev, LAN78XX_HW_CFG, LAN78XX_HW_CFG_MEF);
    lan78xx_set_reg_bits(udev, LAN78XX_USB_CFG0, LAN78XX_USB_CFG_BIR);
    lan78xx_write_reg(udev, LAN78XX_BURST_CAP, LAN78XX_BURST_CAP_VAL);
    lan78xx_write_reg(udev, LAN78XX_BULK_IN_DLY, LAN78XX_BULK_IN_DLY_VAL);
    lan78xx_write_reg(udev, LAN78XX_FCT_RX_FIFO_END, LAN78XX_FCT_RX_FIFO_END_VAL);
    lan78xx_write_reg(udev, LAN78XX_FCT_TX_FIFO_END, LAN78XX_FCT_TX_FIFO_END_VAL);
    kprintf("[lan78xx] MEF+BIR set, FIFO/burst configured (burst_cap=%02x "
            "rx_fifo_end=%02x tx_fifo_end=%02x bulk_in_dly=%04x)\r\n",
            LAN78XX_BURST_CAP_VAL, LAN78XX_FCT_RX_FIFO_END_VAL,
            LAN78XX_FCT_TX_FIFO_END_VAL, LAN78XX_BULK_IN_DLY_VAL);

    /* Step 7: receive filter = broadcast + perfect-DA (our own MAC via MAF[0]).
     * UCAST_EN (accept ALL unicast) makes the RX path handle every host's
     * traffic, which on a busy network degrades the serial console; and it did
     * NOT fix ping anyway (the problem is in network-input processing, not the
     * RX filter).  Keep it off; DA_PERFECT + MAF[0] receives our own unicast. */
    lan78xx_write_reg(udev, LAN78XX_RFE_CTL,
                      LAN78XX_RFE_CTL_BCAST_EN |
                      LAN78XX_RFE_CTL_DA_PERFECT);
    v = 0;
    lan78xx_read_reg(udev, LAN78XX_RFE_CTL, &v);
    kprintf("[lan78xx] RFE_CTL=%08x (bcast+perfect)\r\n", v);

    /* Step 8: MAC_CR auto duplex + auto speed. */
    lan78xx_set_reg_bits(udev, LAN78XX_MAC_CR,
                         LAN78XX_MAC_CR_AUTO_DUPLEX | LAN78XX_MAC_CR_AUTO_SPEED);
    v = 0;
    lan78xx_read_reg(udev, LAN78XX_MAC_CR, &v);
    kprintf("[lan78xx] MAC_CR=%08x (auto-duplex/auto-speed)\r\n", v);

    if (udev->last_error != USB_STATUS_SUCCESS)
    {
        kprintf("[lan78xx] bind: USB error %d during init\r\n",
                udev->last_error);
        return udev->last_error;
    }
    kprintf("[lan78xx] bind complete\r\n");
    return USB_STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------- *
 * Open-time init: PHY autoneg + link wait + enable RX/TX                   *
 * ----------------------------------------------------------------------- */

usb_status_t
lan78xx_open(struct usb_device *udev)
{
    uint16_t id1 = 0, id2 = 0, bmsr = 0;
    uint i;
    bool link;
    uint32_t v;

    udev->last_error = USB_STATUS_SUCCESS;

    /* Step 9: read PHY id regs and kick off auto-negotiation. */
    lan78xx_mii_read(udev, LAN78XX_PHY_ID1, &id1);
    lan78xx_mii_read(udev, LAN78XX_PHY_ID2, &id2);
    kprintf("[lan78xx] PHY id1=%04x id2=%04x\r\n", id1, id2);

    lan78xx_mii_write(udev, LAN78XX_PHY_BMCR,
                      LAN78XX_BMCR_ANENABLE | LAN78XX_BMCR_ANRESTART);
    kprintf("[lan78xx] BMCR autoneg restart issued\r\n");

    /* Poll link up (BMSR bit 2) for ~3 seconds. */
    link = FALSE;
    for (i = 0; i < 300; i++)
    {
        bmsr = 0;
        lan78xx_mii_read(udev, LAN78XX_PHY_BMSR, &bmsr);
        if (bmsr & LAN78XX_BMSR_LSTATUS)
        {
            link = TRUE;
            break;
        }
        mdelay(10);
    }
    kprintf("[lan78xx] link=%s (BMSR=%04x)\r\n", link ? "up" : "down", bmsr);

    /* Step 10: enable RX (max frame + FCS strip + RXEN), TX, and FIFO ctls. */
    lan78xx_write_reg(udev, LAN78XX_MAC_RX,
                      ((uint32_t)LAN78XX_MAX_FRAME_SIZE << LAN78XX_MAC_RX_MAX_SIZE_SHIFT) |
                      LAN78XX_MAC_RX_FCS_STRIP |
                      LAN78XX_MAC_RX_RXEN);
    lan78xx_set_reg_bits(udev, LAN78XX_MAC_TX, LAN78XX_MAC_TX_TXEN);
    lan78xx_set_reg_bits(udev, LAN78XX_FCT_RX_CTL, LAN78XX_FCT_RX_CTL_EN);
    lan78xx_set_reg_bits(udev, LAN78XX_FCT_TX_CTL, LAN78XX_FCT_TX_CTL_EN);

    v = 0;
    lan78xx_read_reg(udev, LAN78XX_MAC_RX, &v);
    kprintf("[lan78xx] rx/tx enabled (MAC_RX=%08x)\r\n", v);

    if (udev->last_error != USB_STATUS_SUCCESS)
    {
        kprintf("[lan78xx] open: USB error %d enabling rx/tx\r\n",
                udev->last_error);
        return udev->last_error;
    }
    return USB_STATUS_SUCCESS;
}
