// include/gsoftkbd.h — on-screen soft keyboard window
// (ported from xinu-rpi5 softkbd.h for the Pi 3 / arm-rpi3 platform).
//
// Renders a QWERTY keyboard inside one wm window.  Clicking a key feeds
// the character to the last-focused window (softkbd_hit + gwm_feed_key).

#ifndef XINU_RPI3_GSOFTKBD_H
#define XINU_RPI3_GSOFTKBD_H

#include <gwm.h>

void softkbd_draw(window_t *self, unsigned int frame);

/* Hit-test a click at desktop coords; on a key, sets *out_char (0 for a
 * modifier), sets *out_repaint when the Shift/Caps display changed, and
 * returns 1.  Wired into the wm click handler so clicking a key feeds it to
 * the focused window.  Defined in apps/gsoftkbd.c. */
int softkbd_hit(int dx, int dy, int *out_char, int *out_repaint);

extern window_t softkbd_win;

#endif /* XINU_RPI3_GSOFTKBD_H */
