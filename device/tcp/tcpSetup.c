/**
 * @file tcpSetup.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <stddef.h>
#include <clock.h>
#include <network.h>
#include <semaphore.h>
#include <tcp.h>

static uint tcpIss(void);

/**
 * @ingroup tcp
 *
 * Intializes a transmission control block.
 * @prarm tcbptr TCB for connection
 * @return OK if TCB is initialized properly, otherwise SYSERR
 * @pre-condition TCB mutex is already held
 * @post-condition TCB mutex is still held
 */
int tcpSetup(struct tcb *tcbptr)
{
    /* Error check parameters */
    if (NULL == tcbptr)
    {
        return SYSERR;
    }

    /* Intialize connection semaphore */
    tcbptr->openclose = semcreate(0);

    /* Initialize input buffer */
    tcbptr->istart = 0;
    tcbptr->inxt = 0;
    tcbptr->icount = 0;
    tcbptr->ibytes = 0;
    tcbptr->readers = semcreate(0);

    /* Initialize output buffer */
    tcbptr->ostart = 0;
    tcbptr->ocount = 0;
    tcbptr->obytes = 0;
    tcbptr->writers = semcreate(1);

    /* Initialize send fields */
    tcbptr->iss = tcpIss();
    tcbptr->snduna = tcbptr->iss;
    tcbptr->sndnxt = tcbptr->iss;
    tcbptr->sndwl2 = tcbptr->iss;
    tcbptr->sndmss = TCP_INIT_MSS;
    tcbptr->sndflg = NULL;
    tcbptr->sndcwn = tcbptr->sndmss;
    tcbptr->sndsst = TCP_MAX_WND;
    tcbptr->rxttime = TCP_RXT_INITTIME;
    tcbptr->rxtcount = 0;
    tcbptr->psttime = TCP_PST_INITTIME;

    /* Initialize receive fields */
    tcbptr->rcvmss = TCP_INIT_MSS - TCP_HDR_LEN;
    tcbptr->rcvflg = NULL;

    /* Verify creation of semaphores */
    if ((SYSERR == (int)tcbptr->openclose)
        || (SYSERR == (int)tcbptr->readers)
        || (SYSERR == (int)tcbptr->writers))
    {
        return SYSERR;
    }

    return OK;
}

/*
 * Provides an initial send sequence number.
 * @return initial send sequence number
 */
static uint tcpIss(void)
{
    static uint base = 0;
    static uint counter = 0;

    /* RFC 793: the initial send sequence number should track a fast clock so
     * successive connections use widely separated sequence space.  The old
     * version seeded from clktime (whole seconds) and only added TCP_SEQINCR
     * (904) per connection, producing tiny, nearly-adjacent ISS values; a
     * reconnect within the TIME_WAIT window then landed in the peer's recent
     * sequence space and got reset (observed on real hardware: SYN-ACK seq
     * ~2820 incrementing, peer RSTs, telnet connections failed to recycle).
     * Mix a one-time pseudo-random base (cycle counter) with a fast time
     * component (clkticks is in ms; <<10 advances ~1 M/s) and a per-connection
     * bump so even two connections in the same millisecond stay separated. */
    if (0 == base)
    {
        base = (uint)clkcount() | 1;
    }
    counter++;
    return base + ((uint)clkticks << 10) + counter * TCP_SEQINCR;
}
