/**
 * @file etherWrite.c
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#include "smsc9512.h"
#include "lan78xx.h"
#include <bufpool.h>
#include <ether.h>
#include <interrupt.h>
#include <string.h>
#include <usb_core_driver.h>

/* Implementation of etherWrite() for the SMSC LAN9512; see the documentation
 * for this function in ether.h.  */
devcall etherWrite(device *devptr, const void *buf, uint len)
{
    struct ether *ethptr;
    struct usb_xfer_request *req;
    uint8_t *sendbuf;
    uint32_t tx_cmd_a, tx_cmd_b;

    ethptr = &ethertab[devptr->minor];
    if (ethptr->state != ETH_STATE_UP ||
        len < ETH_HEADER_LEN || len > ETH_HDR_LEN + ETH_MTU)
    {
        return SYSERR;
    }

    /* Get a buffer for the packet.  (This may block.)  */
    req = bufget(ethptr->outPool);

    /* Copy the packet's data into the buffer, but also include two words at the
     * beginning that contain device-specific flags.  Both the LAN9512 and the
     * LAN78xx prepend an 8-byte two-word command header, but the bitfields
     * differ, so we branch on the chip type.  */
    sendbuf = req->sendbuf;
    if (ethptr->chiptype == ETH_CHIP_LAN78XX)
    {
        /* LAN78xx TX_CMD_A = frame_len (bits 0..19) | FCS request; TX_CMD_B=0. */
        tx_cmd_a = (len & LAN78XX_TX_CMD_A_LEN_MASK) | LAN78XX_TX_CMD_A_FCS;
        tx_cmd_b = 0;
    }
    else
    {
        /* LAN9512: single packet, first+last segment, byte length in cmd B. */
        tx_cmd_a = len | TX_CMD_A_FIRST_SEG | TX_CMD_A_LAST_SEG;
        tx_cmd_b = len;
    }
    sendbuf[0] = (tx_cmd_a >> 0)  & 0xff;
    sendbuf[1] = (tx_cmd_a >> 8)  & 0xff;
    sendbuf[2] = (tx_cmd_a >> 16) & 0xff;
    sendbuf[3] = (tx_cmd_a >> 24) & 0xff;
    sendbuf[4] = (tx_cmd_b >> 0)  & 0xff;
    sendbuf[5] = (tx_cmd_b >> 8)  & 0xff;
    sendbuf[6] = (tx_cmd_b >> 16) & 0xff;
    sendbuf[7] = (tx_cmd_b >> 24) & 0xff;
    STATIC_ASSERT(SMSC9512_TX_OVERHEAD == 8);
    STATIC_ASSERT(LAN78XX_TX_OVERHEAD == 8);
    memcpy(sendbuf + SMSC9512_TX_OVERHEAD, buf, len);

    /* Set total size of the data to send over the USB.  */
    req->size = len + SMSC9512_TX_OVERHEAD;

    /* Submit the data as an asynchronous bulk USB transfer.  In other words,
     * this tells the USB subsystem to send begin sending the data over the USB
     * to the SMSC LAN9512 USB Ethernet Adapter.  At some later time when all
     * the data has been transferred over the USB, smsc9512_tx_complete() will
     * be called by the USB subsystem.  */
    usb_submit_xfer_request(req);

    /* Return the length of the packet written (not including the
     * device-specific fields that were added). */
    return len;
}
