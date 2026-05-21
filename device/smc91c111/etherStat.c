/**
 * @file etherStat.c
 */
#include "smc91c111.h"
#include <ether.h>
#include <stdio.h>

#ifdef NETHER
void etherStat(ushort minor)
{
    struct ether *ethptr = &ethertab[minor];
    uchar *m = ethptr->devAddress;

    fprintf(stdout, "eth%d:\n", minor);
    fprintf(stdout,
            "  MAC          %02X:%02X:%02X:%02X:%02X:%02X\n",
            m[0], m[1], m[2], m[3], m[4], m[5]);
    fprintf(stdout, "  MTU          %d\n", ethptr->mtu);
    fprintf(stdout, "  State        %s\n",
            ethptr->state == ETH_STATE_UP   ? "UP"   :
            ethptr->state == ETH_STATE_DOWN ? "DOWN" : "FREE");
    fprintf(stdout, "  Rx IRQ       %d\n", ethptr->rxirq);
    fprintf(stdout, "  Tx IRQ       %d\n", ethptr->txirq);
    fprintf(stdout, "  Rx errors    %d\n", ethptr->rxErrors);
    fprintf(stdout, "  Overruns     %d\n", ethptr->ovrrun);
    fprintf(stdout, "  Errors       %d\n", ethptr->errors);
}

void etherThroughput(ushort minor)
{
    fprintf(stdout, "Throughput monitoring not implemented for "
            "SMSC LAN91C111\n");
}
#endif
