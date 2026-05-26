/**
 * @file gwincon.h
 *
 * Window-console pseudo-device.  A windowed xsh shell opens GWINCON0
 * for stdout/stderr; gwinconPutc() forwards every character to the
 * on-screen shell window's text ring (apps/gwm.c).
 */
#ifndef _GWINCON_H_
#define _GWINCON_H_

#include <conf.h>
#include <device.h>

/* Device entry points (call only through the device table). */
devcall gwinconInit(device *devptr);
devcall gwinconPutc(device *devptr, char ch);

#endif /* _GWINCON_H_ */
