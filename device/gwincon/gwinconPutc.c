/**
 * @file gwinconPutc.c
 *
 * Write one character to the on-screen shell window by appending it
 * to the window's text ring.  The window manager thread renders the
 * ring incrementally (dirty rows only) to avoid framebuffer flicker.
 */
/* Embedded Xinu, arm-rpi3 window manager add-on. */

#include <gwincon.h>

/* Defined in apps/gwm.c (only built for arm-rpi3). */
extern void gwin_shell_record(char);

/**
 * Append the character to the shell window's ring buffer.
 *
 * @param devptr  device table entry (unused)
 * @param ch      character to record
 * @return        the character written, as an unsigned char
 */
devcall gwinconPutc(device *devptr, char ch)
{
    (void)devptr;
    gwin_shell_record(ch);
    return (uchar)ch;
}

/**
 * Write a buffer to the shell window's text ring.  shell_readline()
 * emits the prompt, character echo and line-redraw via write() (not
 * putc), so GWINCON0 MUST provide a real write entry — otherwise the
 * interactive shell produces no visible output.
 *
 * @param devptr  device table entry (unused)
 * @param buf     bytes to record
 * @param len     number of bytes
 * @return        number of bytes written
 */
devcall gwinconWrite(device *devptr, const void *buf, uint len)
{
    const uchar *p = (const uchar *)buf;
    uint i;
    (void)devptr;
    for (i = 0; i < len; i++) gwin_shell_record((char)p[i]);
    return (devcall)len;
}
