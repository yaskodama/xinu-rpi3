/**
 * @file uartPutc.c
 */
/* Embedded Xinu, Copyright (C) 2009, 2013.  All rights reserved. */

#include <device.h>
#include <stddef.h>
#include <uart.h>

/* ---- remote-shell output capture (for the /shell HTTP route) --------
 * When capture is on, every byte written through a UART is also copied
 * into a buffer so the web gateway (apps/webactor.c) can run a shell
 * command with its stdout routed to SERIAL0 and return the console
 * output over HTTP.  The UART still emits the byte — capture is a passive
 * tee, exactly like the Pi-4 uart_putc capture.  Not reentrant (single
 * shared buffer); the web server runs one command at a time. */
static char uart_cap[8192];
static int  uart_cap_n  = 0;
static volatile int uart_cap_on = 0;

void uart_capture_begin(void)
{
    uart_cap_n = 0;
    uart_cap_on = 1;
}

int uart_capture_end(char *dst, int max)
{
    int n = uart_cap_n, i;
    uart_cap_on = 0;
    if (n > max)
    {
        n = max;
    }
    for (i = 0; i < n; i++)
    {
        dst[i] = uart_cap[i];
    }
    return n;
}

/**
 * @ingroup uartgeneric
 *
 * Write a single character to a UART.
 *
 * @param devptr
 *      Pointer to the device table entry for a UART.
 * @param ch
 *      The character to write.
 *
 * @return
 *      On success, returns the character written as an <code>unsigned
 *      char</code> cast to an @c int.  On failure, returns SYSERR.
 */
devcall uartPutc(device *devptr, char ch)
{
    int retval;

    if (uart_cap_on && uart_cap_n < (int)sizeof(uart_cap))
    {
        uart_cap[uart_cap_n++] = ch;       /* tee into the capture buffer */
    }

    retval = uartWrite(devptr, &ch, 1);
    if (retval == 1)
    {
        return (uchar)ch;
    }
    else
    {
        return SYSERR;
    }
}
