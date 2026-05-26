/**
 * @file telnetControl.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <stddef.h>
#include <device.h>
#include <semaphore.h>
#include <telnet.h>
#include <tty.h>

/**
 * @ingroup telnet
 *
 * Control function for TELNET pseudo devices.
 * @param devptr TELNET device table entry
 * @param func control function to execute
 * @param arg1 first argument for the control function
 * @param arg2 second argument for the control function
 * @return the result of the control function
 */
devcall telnetControl(device *devptr, int func, long arg1, long arg2)
{
    struct telnet *tntptr;
    device *phw;

    /* Setup and error check pointers to structures */
    tntptr = &telnettab[devptr->minor];
    phw = tntptr->phw;
    if (NULL == phw)
    {
        return SYSERR;
    }

    switch (func)
    {
    case TELNET_CTRL_FLUSH:
        /* Hold the output semaphore so this flush (called periodically by the
         * telnet server thread) is mutually exclusive with telnetWrite().
         * Without it, a command running in a child thread that streams a lot of
         * output (e.g. `help`) races with this flush over tntptr->out/ostart,
         * corrupting/losing the data — which is why telnet command output never
         * reached the client while the (short, race-free) prompt did.
         * telnetWrite() calls telnetFlush() directly while already holding the
         * semaphore, so this separate path does not deadlock. */
        wait(tntptr->osem);
        telnetFlush(devptr);
        signal(tntptr->osem);
        return OK;
    case TELNET_CTRL_CLRFLAG:
        /* arg1 is the flag we are clearing */
        tntptr->flags &= ~arg1;
        return OK;
    case TELNET_CTRL_SETFLAG:
        /* arg1 is the flag we are setting */
        tntptr->flags |= arg1;
        return OK;
    case TTY_CTRL_SET_IFLAG:
        return OK;
    case TTY_CTRL_CLR_IFLAG:
        return OK;
    case TTY_CTRL_SET_OFLAG:
        return OK;
    case TTY_CTRL_CLR_OFLAG:
        return OK;
    }

    return SYSERR;
}
