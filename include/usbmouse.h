/**
 * @file usbmouse.h
 *
 * Header for the USB mouse driver (HID boot protocol only).
 *
 * Boot-protocol mouse reports are 3+ bytes: byte 0 = button bitmap
 * (bit0 left, bit1 right, bit2 middle), byte 1 = signed X delta,
 * byte 2 = signed Y delta, byte 3 (if present) = wheel.  The driver
 * accumulates the deltas into a screen-space cursor position and
 * hands it to the window manager via wm_cursor_set().
 */
#ifndef _USBMOUSE_H_
#define _USBMOUSE_H_

#include <conf.h>
#include <device.h>
#include <usb_util.h>

struct usb_xfer_request;
struct usb_device;

/* Tracing macro (off by default). */
//#define ENABLE_TRACE_USBMOUSE
#ifdef ENABLE_TRACE_USBMOUSE
#  include <kernel.h>
#  include <thread.h>
#  define USBMOUSE_TRACE(...)   { \
        kprintf("%s:%d (%d) ", __FILE__, __LINE__, gettid()); \
        kprintf(__VA_ARGS__); \
        kprintf("\n"); }
#else
#  define USBMOUSE_TRACE(...)
#endif

/** USB mouse control block. */
struct usbmouse
{
    bool initialized;               /**< control block initialized?      */
    bool attached;                  /**< physical mouse attached?         */
    struct usb_xfer_request *intr;  /**< IN interrupt transfer request    */
};

extern struct usbmouse usbmice[NUSBMOUSE];

/* Latest cursor / button state, updated by usbMouseInterrupt().
 * Coordinates are clamped to the live framebuffer size. */
extern volatile int usbmouse_x;
extern volatile int usbmouse_y;
extern volatile int usbmouse_buttons;

/* Device entry points (call only through the device table). */
devcall usbMouseInit(device *devptr);
void    usbMouseInterrupt(struct usb_xfer_request *req);

/* USB core callbacks. */
usb_status_t usbMouseBindDevice(struct usb_device *dev);
void         usbMouseUnbindDevice(struct usb_device *dev);

#endif /* _USBMOUSE_H_ */
