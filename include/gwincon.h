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

/* Input ring (one per minor / shell window): apps/webactor.c and the
 * USB keyboard inject keys via gwincon_feed(); the windowed xsh shell
 * consumes them through gwinconGetc(). */
void gwincon_input_init(int m);
void gwincon_feed(int m, int c);
void gwincon_input_stat(int *head, int *tail, int *ready);

/* Stdout side, per minor: defined in apps/gwm.c. */
void gwin_shell_record_m(int m, char c);
void gwin_shell_record(char c);

#endif /* _GWINCON_H_ */
