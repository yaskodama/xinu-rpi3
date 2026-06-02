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
devcall gwinconWrite(device *devptr, const void *buf, uint len);
devcall gwinconGetc(device *devptr);

/* Input ring: apps/webactor.c injects keys via gwincon_feed(); the
 * windowed xsh shell consumes them through gwinconGetc(). */
void gwincon_input_init(void);
void gwincon_feed(int c);
void gwincon_input_stat(int *head, int *tail, int *ready);

#endif /* _GWINCON_H_ */
