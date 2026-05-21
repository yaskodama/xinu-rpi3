/**
 * @file etherControl.c
 */
#include "smc91c111.h"
#include <ether.h>
#include <ethernet.h>
#include <network.h>
#include <stddef.h>
#include <string.h>

devcall etherControl(device *devptr, int func, long arg1, long arg2)
{
    struct ether     *ethptr;
    struct smc91c111 *chip;
    struct netaddr   *addr;
    uchar *macptr;

    ethptr = &ethertab[devptr->minor];
    chip   = ethptr->csr;
    if (chip == NULL)
    {
        return SYSERR;
    }

    switch (func)
    {
    case ETH_CTRL_SET_MAC:
        macptr = (uchar *)arg1;
        memcpy(ethptr->devAddress, macptr, ETH_ADDR_LEN);
        smc_set_mac(chip, ethptr->devAddress);
        break;

    case ETH_CTRL_GET_MAC:
        macptr = (uchar *)arg1;
        memcpy(macptr, ethptr->devAddress, ETH_ADDR_LEN);
        break;

    case NET_GET_LINKHDRLEN:
        return ETH_HDR_LEN;

    case NET_GET_MTU:
        return ETH_MTU;

    case NET_GET_HWADDR:
        addr = (struct netaddr *)arg1;
        addr->type = NETADDR_ETHERNET;
        addr->len  = ETH_ADDR_LEN;
        memcpy(addr->addr, ethptr->devAddress, ETH_ADDR_LEN);
        break;

    case NET_GET_HWBRC:
        addr = (struct netaddr *)arg1;
        addr->type = NETADDR_ETHERNET;
        addr->len  = ETH_ADDR_LEN;
        memset(addr->addr, 0xFF, ETH_ADDR_LEN);
        break;

    case ETH_CTRL_RESET:
        smc_disable(chip);
        smc_reset(chip);
        smc_set_mac(chip, ethptr->devAddress);
        if (ethptr->state == ETH_STATE_UP)
        {
            smc_enable(chip);
        }
        break;

    case ETH_CTRL_SET_LOOPBK:
        smc_select_bank(chip, 0);
        if (arg1)
        {
            smc_write16(chip, SMC_TCR,
                        (ushort)(smc_read16(chip, SMC_TCR) | TCR_LOOP));
        }
        else
        {
            smc_write16(chip, SMC_TCR,
                        (ushort)(smc_read16(chip, SMC_TCR) & ~TCR_LOOP));
        }
        break;

    default:
        return SYSERR;
    }
    return OK;
}
