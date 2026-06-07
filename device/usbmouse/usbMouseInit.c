/**
 * @file usbMouseInit.c
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#include <usb_core_driver.h>
#include <usbmouse.h>

/** Table of USB mouse control blocks. */
struct usbmouse usbmice[NUSBMOUSE];

/* Cursor / button state shared with the window manager. */
volatile int usbmouse_x = 0;
volatile int usbmouse_y = 0;
volatile int usbmouse_buttons = 0;

/** USB device driver structure for the usbmouse driver. */
static struct usb_device_driver usbmouse_driver = {
    .name          = "USB mouse driver (HID boot protocol)",
    .bind_device   = usbMouseBindDevice,
    .unbind_device = usbMouseUnbindDevice,
};

/**
 * Initialize the specified USB mouse.  Prepares the control block and
 * registers the driver with the USB subsystem; the physical mouse is
 * recognized only when actually attached (usbMouseBindDevice()).
 */
devcall usbMouseInit(device *devptr)
{
    usb_status_t status;
    struct usbmouse *m;

    m = &usbmice[devptr->minor];

    if (m->initialized)
    {
        goto err;
    }

    /* Boot-protocol mouse reports are 3-4 bytes; 8 is plenty. */
    m->intr = usb_alloc_xfer_request(8);
    if (NULL == m->intr)
    {
        goto err;
    }

    m->attached = FALSE;
    m->initialized = TRUE;

    /* Register the mouse USB device driver (no-op if already done). */
    status = usb_register_device_driver(&usbmouse_driver);
    if (status != USB_STATUS_SUCCESS)
    {
        goto err_free_req;
    }

    return OK;

err_free_req:
    m->initialized = FALSE;
    usb_free_xfer_request(m->intr);
err:
    return SYSERR;
}
