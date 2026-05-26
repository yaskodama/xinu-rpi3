/**
 * @file usbMouseBindDevice.c
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#include <usb_core_driver.h>
#include <usbmouse.h>

/* Constants from the USB HID v1.11 specification. */
#define HID_SUBCLASS_BOOT        1     /* Section 4.2   */
#define HID_BOOT_PROTOCOL_MOUSE  2     /* Section 4.3   */
#define HID_REQUEST_SET_PROTOCOL 0x0B  /* Section 7.2   */
#define HID_BOOT_PROTOCOL        0     /* Section 7.2.5 */

/**
 * Called by the USB subsystem when a device that may be a mouse is attached.
 * Binds to the first HID interface that supports the mouse boot protocol.
 */
usb_status_t
usbMouseBindDevice(struct usb_device *dev)
{
    uint i, j;
    const struct usb_interface_descriptor *mouse_interface;
    const struct usb_endpoint_descriptor *in_interrupt_endpoint;
    struct usbmouse *m;
    usb_status_t status;

    USBMOUSE_TRACE("Attempting to bind USB device (%s %s: address %u)",
                   dev->manufacturer, dev->product, dev->address);

    /* HID class lives per-interface, never in the device descriptor. */
    if (USB_CLASS_CODE_INTERFACE_SPECIFIC != dev->descriptor.bDeviceClass)
    {
        return USB_STATUS_DEVICE_UNSUPPORTED;
    }

    mouse_interface = NULL;
    in_interrupt_endpoint = NULL;
    for (i = 0; i < dev->config_descriptor->bNumInterfaces; i++)
    {
        struct usb_interface_descriptor *interface = dev->interfaces[i];

        if (USB_CLASS_CODE_HID != interface->bInterfaceClass)
        {
            continue;
        }
        if (HID_SUBCLASS_BOOT != interface->bInterfaceSubClass ||
            HID_BOOT_PROTOCOL_MOUSE != interface->bInterfaceProtocol)
        {
            continue;
        }

        /* Find the IN interrupt endpoint. */
        for (j = 0; j < interface->bNumEndpoints; j++)
        {
            if ((dev->endpoints[i][j]->bmAttributes & 0x3) ==
                    USB_TRANSFER_TYPE_INTERRUPT &&
                (dev->endpoints[i][j]->bEndpointAddress >> 7) ==
                    USB_DIRECTION_IN)
            {
                in_interrupt_endpoint = dev->endpoints[i][j];
                break;
            }
        }
        if (NULL == in_interrupt_endpoint)
        {
            continue;
        }
        mouse_interface = interface;
        break;
    }
    if (NULL == mouse_interface)
    {
        USBMOUSE_TRACE("No HID interface with mouse boot protocol found");
        return USB_STATUS_DEVICE_UNSUPPORTED;
    }

    /* Map the mouse to a free control block. */
    m = NULL;
    for (i = 0; i < NUSBMOUSE; i++)
    {
        if (usbmice[i].initialized && !usbmice[i].attached)
        {
            m = &usbmice[i];
            break;
        }
    }
    if (NULL == m)
    {
        return USB_STATUS_DEVICE_UNSUPPORTED;
    }

    /* Put the mouse in boot protocol mode. */
    status = usb_control_msg(dev, NULL, HID_REQUEST_SET_PROTOCOL,
                             USB_BMREQUESTTYPE_TYPE_CLASS |
                                 USB_BMREQUESTTYPE_DIR_OUT |
                                 USB_BMREQUESTTYPE_RECIPIENT_INTERFACE,
                             HID_BOOT_PROTOCOL,
                             mouse_interface->bInterfaceNumber, NULL, 0);
    if (USB_STATUS_SUCCESS != status)
    {
        USBMOUSE_TRACE("Failed to set boot protocol: %s",
                       usb_status_string(status));
        return USB_STATUS_DEVICE_UNSUPPORTED;
    }

    dev->driver_private = m;

    /* Submit the IN interrupt transfer; usbMouseInterrupt() runs on each
     * completion (i.e. whenever the mouse moves or a button changes). */
    m->intr->dev = dev;
    m->intr->endpoint_desc = in_interrupt_endpoint;
    m->intr->completion_cb_func = usbMouseInterrupt;
    m->intr->private = m;

    status = usb_submit_xfer_request(m->intr);
    if (USB_STATUS_SUCCESS != status)
    {
        return status;
    }
    m->attached = TRUE;

    return USB_STATUS_SUCCESS;
}
