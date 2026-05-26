/**
 * @file usbMouseUnbindDevice.c
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#include <usb_core_driver.h>
#include <usbmouse.h>

/**
 * Called by the USB subsystem when a previously bound USB mouse is detached.
 * The pending interrupt transfer has already been cancelled by the core, so
 * we only need to mark the control block free for re-use.
 */
void usbMouseUnbindDevice(struct usb_device *dev)
{
    struct usbmouse *m = dev->driver_private;

    if (NULL != m)
    {
        m->attached = FALSE;
    }
    usbmouse_buttons = 0;
}
