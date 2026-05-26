/**
 * @file usbMouseInterrupt.c
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#include <usbmouse.h>
#include <usb_core_driver.h>

/* Window-manager hooks (apps/gwm.c, apps/gvideo.c).  Declared here to
 * avoid pulling the WM headers into the device driver. */
extern void         wm_cursor_set(int x, int y, int visible);
extern unsigned int video_screen_width(void);
extern unsigned int video_screen_height(void);

/**
 * Called when the IN interrupt transfer from a USB mouse completes.
 * Boot-protocol report layout: [0]=buttons, [1]=dx, [2]=dy, [3]=wheel.
 * The X/Y deltas are signed 8-bit relative movements.
 */
void usbMouseInterrupt(struct usb_xfer_request *req)
{
    static int inited = 0;

    if (req->status == USB_STATUS_SUCCESS && req->actual_size >= 3)
    {
        const uchar *data = req->recvbuf;
        int dx = (int)(signed char)data[1];
        int dy = (int)(signed char)data[2];
        int w  = (int)video_screen_width();
        int h  = (int)video_screen_height();

        if (w <= 0) w = 1024;
        if (h <= 0) h = 768;

        /* Centre the cursor on the first report. */
        if (!inited)
        {
            usbmouse_x = w / 2;
            usbmouse_y = h / 2;
            inited = 1;
        }

        usbmouse_x += dx;
        usbmouse_y += dy;
        if (usbmouse_x < 0)      usbmouse_x = 0;
        if (usbmouse_y < 0)      usbmouse_y = 0;
        if (usbmouse_x > w - 1)  usbmouse_x = w - 1;
        if (usbmouse_y > h - 1)  usbmouse_y = h - 1;

        usbmouse_buttons = data[0];

        wm_cursor_set(usbmouse_x, usbmouse_y, 1);

        USBMOUSE_TRACE("dx=%d dy=%d btn=%d -> (%d,%d)",
                       dx, dy, data[0], usbmouse_x, usbmouse_y);
    }

    /* Re-arm the transfer for the next movement/button event. */
    usb_submit_xfer_request(req);
}
