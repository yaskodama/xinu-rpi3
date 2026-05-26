// include/gsoftkbd.h — on-screen soft keyboard window
// (ported from xinu-rpi5 softkbd.h for the Pi 3 / arm-rpi3 platform).
//
// Renders a QWERTY keyboard inside one wm window.  Input dispatch
// happens later (once a mouse cursor exists to click with); for now
// the window is purely visual.

#ifndef XINU_RPI3_GSOFTKBD_H
#define XINU_RPI3_GSOFTKBD_H

#include <gwm.h>

void softkbd_draw(window_t *self, unsigned int frame);

extern window_t softkbd_win;

#endif /* XINU_RPI3_GSOFTKBD_H */
