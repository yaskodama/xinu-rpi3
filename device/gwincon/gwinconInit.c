/**
 * @file gwinconInit.c
 *
 * Window-console device: a SOFTWARE pseudo-device whose putc entry
 * funnels the windowed shell's stdout/stderr into the on-screen
 * shell window's text ring (see apps/gwm.c gwin_shell_record()).
 */
/* Embedded Xinu, arm-rpi3 window manager add-on. */

#include <gwincon.h>

/**
 * Initialize the window-console device.  No per-device state — the
 * ring buffer it feeds lives in apps/gwm.c and is reset when the
 * window is opened.
 */
devcall gwinconInit(device *devptr)
{
    (void)devptr;
    /* Bring up the keystroke-injection ring used for the shell's stdin
     * (fed over HTTP by apps/webactor.c -> gwincon_feed()). */
    gwincon_input_init();
    return OK;
}
