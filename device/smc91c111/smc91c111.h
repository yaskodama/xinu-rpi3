/**
 * @file smc91c111.h
 *
 * SMSC LAN91C111 10/100 Ethernet controller (memory-mapped) — minimal
 * register definitions for the QEMU versatilepb platform.
 *
 * The chip exposes a 16-byte register window banked into four sets.  The Bank
 * Select Register lives at offset 0x0E and is accessible from any bank.
 */
#ifndef _SMC91C111_H_
#define _SMC91C111_H_

#include <stddef.h>
#include <ether.h>

/* ---------- versatilepb wiring ------------------------------------------- */
#define SMC91C111_BASE      0x10010000UL
#define SMC91C111_IRQ       25

/* ---------- register offsets (within the 16-byte window) ----------------- */
#define SMC_BANK            0x0E    /* Bank Select Register (all banks) */

/* Bank 0 */
#define SMC_TCR             0x00    /* Transmit Control */
#define SMC_EPH_STATUS      0x02
#define SMC_RCR             0x04    /* Receive  Control */
#define SMC_COUNTER         0x06
#define SMC_MIR             0x08    /* Memory Information */
#define SMC_RPCR            0x0A

/* Bank 1 */
#define SMC_CONFIG          0x00
#define SMC_BASE            0x02
#define SMC_IA0_1           0x04    /* MAC bytes 0,1   */
#define SMC_IA2_3           0x06    /* MAC bytes 2,3   */
#define SMC_IA4_5           0x08    /* MAC bytes 4,5   */
#define SMC_GENERAL         0x0A
#define SMC_CTL             0x0C

/* Bank 2 */
#define SMC_MMU_CMD         0x00
#define SMC_PNR             0x02    /* lo: packet#  hi: alloc result */
#define SMC_FIFO            0x04    /* lo: TX FIFO   hi: RX FIFO     */
#define SMC_PTR             0x06
#define SMC_DATA            0x08    /* 32-bit data port              */
#define SMC_INT             0x0C    /* lo: int status  hi: int mask  */

/* Bank 3 */
#define SMC_MT0_1           0x00
#define SMC_MT2_3           0x02
#define SMC_MT4_5           0x04
#define SMC_MT6_7           0x06
#define SMC_MGMT            0x08
#define SMC_REVISION        0x0A
#define SMC_ERCV            0x0C

/* ---------- TCR bits (Bank 0) -------------------------------------------- */
#define TCR_TXENA           0x0001
#define TCR_LOOP            0x0002
#define TCR_PAD_EN          0x0080
#define TCR_FDUPLX          0x0800

/* ---------- RCR bits (Bank 0) -------------------------------------------- */
#define RCR_RX_ABORT        0x0001
#define RCR_PRMS            0x0002    /* promiscuous            */
#define RCR_ALMUL           0x0004    /* accept all multicast   */
#define RCR_RXEN            0x0100
#define RCR_STRIP_CRC       0x0200
#define RCR_SOFT_RST        0x8000

/* ---------- CTL bits (Bank 1) -------------------------------------------- */
#define CTL_AUTO_RELEASE    0x0800

/* ---------- RPC bits (Bank 0) -------------------------------------------- */
#define RPCR_ANEG           0x0800    /* enable auto-negotiation */
#define RPCR_LED_SHIFT      2
#define RPCR_LED_LINK       0x0000    /* link status            */
#define RPCR_LED_TX_RX      0x0002    /* tx+rx activity         */

/* ---------- MMU command (Bank 2, low byte) ------------------------------- */
#define MMU_NOOP            0x0000
#define MMU_ALLOC_TX        0x0020
#define MMU_RESET           0x0040
#define MMU_REMOVE_RX       0x0060
#define MMU_REMOVE_RELEASE  0x0080
#define MMU_RELEASE         0x00A0    /* release packet (by PNR)*/
#define MMU_ENQUEUE_TX      0x00C0
#define MMU_RESET_TX        0x00E0

/* ---------- PNR/ARR (Bank 2) --------------------------------------------- */
#define PNR_NUMBER_MASK     0x003F
#define ARR_FAILED          0x8000    /* allocation failed (high byte 0x80) */

/* ---------- POINTER (Bank 2) --------------------------------------------- */
#define PTR_READ            0x2000
#define PTR_AUTOINCR        0x4000
#define PTR_RCV             0x8000

/* ---------- interrupt bits (Bank 2 SMC_INT low byte = status,
 *                                          high byte = mask) ---------------*/
#define INT_RCV             0x01
#define INT_TX              0x02
#define INT_TX_EMPTY        0x04
#define INT_ALLOC           0x08
#define INT_RX_OVRN         0x10
#define INT_EPH             0x20
#define INT_ERCV            0x40
#define INT_MDINT           0x80

/* ---------- RX status word (first 16 bits of every received packet) ------ */
#define RXSTAT_ALIGNERR     0x8000
#define RXSTAT_BROADCAST    0x4000
#define RXSTAT_BADCRC       0x2000
#define RXSTAT_ODDFRAME     0x1000
#define RXSTAT_TOO_LONG     0x0800
#define RXSTAT_TOO_SHORT    0x0400
#define RXSTAT_MULTICAST    0x0001

/* ---------- per-chip software state -------------------------------------- */
struct smc91c111
{
    volatile void *base;        /* MMIO base address     */
    int           irq;          /* IRQ line              */
};

/* ---------- low-level chip ops (smc91c111.c) ----------------------------- */
void   smc_select_bank(struct smc91c111 *chip, int bank);
ushort smc_read16(struct smc91c111 *chip, int off);
void   smc_write16(struct smc91c111 *chip, int off, ushort val);
ulong  smc_read32(struct smc91c111 *chip, int off);
void   smc_write32(struct smc91c111 *chip, int off, ulong val);
int    smc_reset(struct smc91c111 *chip);
void   smc_set_mac(struct smc91c111 *chip, const uchar mac[ETH_ADDR_LEN]);
void   smc_enable(struct smc91c111 *chip);
void   smc_disable(struct smc91c111 *chip);

#endif /* _SMC91C111_H_ */
