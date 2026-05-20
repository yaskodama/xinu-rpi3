/**
 * @file wmcon.h
 *
 * WM-Console pseudo-device.  Backs a "Console" window inside the
 * window manager so a full xsh shell (with `edit`, history, etc.)
 * can run alongside the UART shell — see apps/wm.c.
 *
 * Output bytes from the shell are routed through a small VT100-lite
 * cell grid so escape sequences emitted by the readline + edit
 * features render correctly.  Input bytes come from the WM PS/2
 * keyboard handler when the Console window has focus.
 */
#ifndef _WMCON_H_
#define _WMCON_H_

#include <device.h>

#define WMCON_COLS  60
#define WMCON_ROWS  16

/* Standard Xinu device entry points (registered via xinu.conf). */
devcall wmconInit   (device *devptr);
devcall wmconOpen   (device *devptr, int dev2);
devcall wmconClose  (device *devptr);
devcall wmconRead   (device *devptr, void *buf, uint count);
devcall wmconWrite  (device *devptr, const void *buf, uint count);
devcall wmconGetc   (device *devptr);
devcall wmconPutc   (device *devptr, char c);
devcall wmconControl(device *devptr, int func, long arg1, long arg2);

/* Called by apps/wm.c when the Console window has focus and a
 * keystroke arrives from the PS/2 handler. */
void wm_console_feed_key (int c);

/* Snapshot the cell grid for rendering. */
void wm_console_get_state(int *rows, int *cols, const char **cells,
                          int *cur_r, int *cur_c);

#endif /* _WMCON_H_ */
