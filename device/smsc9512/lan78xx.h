/**
 * @file lan78xx.h
 *
 * Register definitions and helper prototypes for the Microchip LAN78xx
 * (LAN7800) USB Gigabit Ethernet controller.  This is the onboard NIC on the
 * Raspberry Pi 3 Model B+ (USB VID 0x0424, PID 0x7800).
 *
 * The LAN78xx shares the USB control-transfer register access mechanism and the
 * bulk IN/OUT plumbing with the older SMSC LAN9512 (see smsc9512.h / smsc9512.c),
 * but has a completely different register map and a different TX/RX framing.  We
 * therefore drive both chips from the single Xinu ETH0 device, branching on
 * struct ether.chiptype (ETH_CHIP_SMSC9512 vs ETH_CHIP_LAN78XX).
 *
 * Register definitions and the init sequence were derived from the Linux
 * drivers/net/usb/lan78xx.c driver and the task spec.
 */
#ifndef _LAN78XX_H_
#define _LAN78XX_H_

#include "usb_util.h"
#include <stdint.h>

/** idVendor in the USB device descriptor (same Microchip VID as smsc9512). */
#define LAN78XX_VENDOR_ID   0x0424

/** idProduct in the USB device descriptor for the LAN7800. */
#define LAN78XX_PRODUCT_ID  0x7800

/* LAN78xx USB vendor requests for register access (differ from smsc9512's
 * 0xA0/0xA1... actually they happen to share 0xA0/0xA1 but we keep separate
 * names for clarity).  */
#define LAN78XX_VENDOR_REQUEST_WRITE_REGISTER  0xA0
#define LAN78XX_VENDOR_REQUEST_READ_REGISTER   0xA1

/* ----------------------------------------------------------------------- *
 * LAN78xx register offsets (system control / MAC / FIFO controller)       *
 * ----------------------------------------------------------------------- */
#define LAN78XX_ID_REV          0x000   /**< top 16 bits = 0x7800 for LAN7800 */
#define LAN78XX_INT_STS         0x00C
#define LAN78XX_HW_CFG          0x010
#define LAN78XX_PMT_CTL         0x014
#define LAN78XX_USB_CFG0        0x080
#define LAN78XX_BURST_CAP       0x090
#define LAN78XX_BULK_IN_DLY     0x094
#define LAN78XX_RFE_CTL         0x0B0
#define LAN78XX_FCT_RX_CTL      0x0C0
#define LAN78XX_FCT_TX_CTL      0x0C4
#define LAN78XX_FCT_RX_FIFO_END 0x0C8
#define LAN78XX_FCT_TX_FIFO_END 0x0CC
#define LAN78XX_FCT_FLOW        0x0D0
#define LAN78XX_MAC_CR          0x100
#define LAN78XX_MAC_RX          0x104
#define LAN78XX_MAC_TX          0x108
#define LAN78XX_FLOW            0x10C
#define LAN78XX_RX_ADDRH        0x118   /**< MAC bytes 4-5 */
#define LAN78XX_RX_ADDRL        0x11C   /**< MAC bytes 0-3 */
#define LAN78XX_MII_ACC         0x120
#define LAN78XX_MII_DATA        0x124
/* MAC Address Filter perfect-filter table.  Per the LAN78xx datasheet / Linux
 * lan78xx driver, MAF_BASE=0x400 and entry n is at 0x400+8n (HI) / 0x404+8n
 * (LO).  Entry 0 holds our own MAC for the RFE DA_PERFECT filter.  (The earlier
 * 0x150/0x154 were wrong registers, so MAF[0] was never programmed and every
 * unicast addressed to us was dropped — only broadcast got through.) */
#define LAN78XX_MAF_HI_0        0x400   /**< Address-Filter 0 hi (AF_EN | hi16) */
#define LAN78XX_MAF_LO_0        0x404   /**< Address-Filter 0 lo (MAC lo 32)    */

/* HW_CFG bits */
#define LAN78XX_HW_CFG_LRST     0x00000002  /**< BIT1: lite reset            */
#define LAN78XX_HW_CFG_MEF      0x00000010  /**< BIT4: Multiple Ethernet Frames per bulk-IN */

/* USB_CFG0 bits */
#define LAN78XX_USB_CFG_BIR     0x00000040  /**< BIT6: Bulk-In emptY Response (ZLP, paced by BULK_IN_DLY) */

/* PMT_CTL bits */
#define LAN78XX_PMT_CTL_PHY_RST 0x00000010  /**< BIT4: PHY reset             */
#define LAN78XX_PMT_CTL_READY   0x00000080  /**< BIT7: device ready          */

/* RFE_CTL (receive filtering engine) bits */
#define LAN78XX_RFE_CTL_DA_PERFECT 0x00000002 /**< BIT1: perfect DA filter   */
#define LAN78XX_RFE_CTL_UCAST_EN   0x00000100 /**< BIT8                       */
#define LAN78XX_RFE_CTL_MCAST_EN   0x00000200 /**< BIT9                       */
#define LAN78XX_RFE_CTL_BCAST_EN   0x00000400 /**< BIT10                      */

/* FCT_RX_CTL / FCT_TX_CTL enable bit */
#define LAN78XX_FCT_RX_CTL_EN   0x80000000  /**< BIT31                       */
#define LAN78XX_FCT_TX_CTL_EN   0x80000000  /**< BIT31                       */

/* MAC_CR bits */
#define LAN78XX_MAC_CR_AUTO_SPEED  0x00000800 /**< BIT11                      */
#define LAN78XX_MAC_CR_AUTO_DUPLEX 0x00001000 /**< BIT12                      */

/* MAC_RX bits */
#define LAN78XX_MAC_RX_RXEN        0x00000001 /**< BIT0: receiver enable      */
#define LAN78XX_MAC_RX_FCS_STRIP   0x00000010 /**< BIT4: strip FCS from frame */
#define LAN78XX_MAC_RX_MAX_SIZE_SHIFT 16      /**< max-frame-size bits 16..29 */

/* MAC_TX bits */
#define LAN78XX_MAC_TX_TXEN        0x00000001 /**< BIT0: transmitter enable   */

/* Address-Filter perfect-filter enable bit (in MAF_HI(n)) */
#define LAN78XX_MAF_HI_AF_EN       0x80000000 /**< BIT31                      */

/* MII_ACC fields */
#define LAN78XX_MII_ACC_BUSY       0x00000001 /**< BIT0: MII busy             */
#define LAN78XX_MII_ACC_WRITE      0x00000002 /**< BIT1: 1=write, 0=read      */
#define LAN78XX_MII_ACC_PHY_ADDR_SHIFT  11
#define LAN78XX_MII_ACC_MII_REG_SHIFT   6

/** Internal PHY address on the LAN78xx. */
#define LAN78XX_INTERNAL_PHY_ADDR  1

/* Standard MII/PHY register numbers */
#define LAN78XX_PHY_BMCR        0   /**< Basic Mode Control Register          */
#define LAN78XX_PHY_BMSR        1   /**< Basic Mode Status Register           */
#define LAN78XX_PHY_ID1         2   /**< PHY identifier 1                     */
#define LAN78XX_PHY_ID2         3   /**< PHY identifier 2                     */

#define LAN78XX_BMCR_ANRESTART  0x0200  /**< restart auto-negotiation          */
#define LAN78XX_BMCR_ANENABLE   0x1000  /**< enable auto-negotiation           */
#define LAN78XX_BMSR_LSTATUS    0x0004  /**< link up (BMSR bit 2)              */

/* ----------------------------------------------------------------------- *
 * TX/RX framing                                                           *
 * ----------------------------------------------------------------------- */

/** TX command header size (TX_CMD_A + TX_CMD_B), prepended to each frame. */
#define LAN78XX_TX_OVERHEAD     8

/** RX command header size (RX_CMD_A u32 + RX_CMD_B u32 + RX_CMD_C u16). */
#define LAN78XX_RX_OVERHEAD     10

/* TX_CMD_A fields */
#define LAN78XX_TX_CMD_A_LEN_MASK  0x000FFFFF  /**< frame length bits 0..19   */
#define LAN78XX_TX_CMD_A_FCS       0x00400000  /**< BIT22: request HW FCS     */

/* RX_CMD_A fields */
#define LAN78XX_RX_CMD_A_LEN_MASK  0x00003FFF  /**< frame length bits 0..13   */
#define LAN78XX_RX_CMD_A_ERR       0x00400000  /**< BIT22: receive error      */

/* USB high-speed packet size and Rx burst size (same scheme as smsc9512). */
#define LAN78XX_HS_USB_PKT_SIZE    512
#define LAN78XX_DEFAULT_HS_BURST_CAP_SIZE  (16 * 1024 + 5 * LAN78XX_HS_USB_PKT_SIZE)

/* FIFO sizes, in units of 512 bytes, per the Linux lan78xx driver. */
#define LAN78XX_FCT_RX_FIFO_END_VAL  0x27   /**< 10 KB */
#define LAN78XX_FCT_TX_FIFO_END_VAL  0x11   /**< 4 KB  */
#define LAN78XX_BULK_IN_DLY_VAL      0x0800
/* BURST_CAP: max USB packets batched per bulk-IN.  MUST be non-zero when
 * HW_CFG_MEF is set, else the device returns empty bulk-IN responses back to
 * back and the (immediately re-submitting) RX completion callback spins,
 * starving the whole system.  = burst buffer size / HS packet size. */
#define LAN78XX_BURST_CAP_VAL  (LAN78XX_DEFAULT_HS_BURST_CAP_SIZE / LAN78XX_HS_USB_PKT_SIZE)

/** Max ethernet frame the MAC will accept (incl. header + VLAN + FCS slack). */
#define LAN78XX_MAX_FRAME_SIZE       1522

/* ----------------------------------------------------------------------- *
 * Prototypes (implemented in lan78xx.c)                                   *
 * ----------------------------------------------------------------------- */
struct usb_device;

usb_status_t lan78xx_read_reg(struct usb_device *udev, uint32_t index, uint32_t *data);
usb_status_t lan78xx_write_reg(struct usb_device *udev, uint32_t index, uint32_t data);
usb_status_t lan78xx_modify_reg(struct usb_device *udev, uint32_t index,
                                uint32_t mask, uint32_t set);
usb_status_t lan78xx_set_reg_bits(struct usb_device *udev, uint32_t index, uint32_t set);

usb_status_t lan78xx_set_mac_address(struct usb_device *udev, const uint8_t *macaddr);
usb_status_t lan78xx_get_mac_address(struct usb_device *udev, uint8_t *macaddr);

/** Per-device init done at USB bind time (reset + MAC + filtering setup). */
usb_status_t lan78xx_bind(struct usb_device *udev, const uint8_t *macaddr);

/** Per-open init: PHY autoneg, link wait, enable RX/TX.  Returns USB status. */
usb_status_t lan78xx_open(struct usb_device *udev);

#endif /* _LAN78XX_H_ */
