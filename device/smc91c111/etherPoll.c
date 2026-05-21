/**
 * @file etherPoll.c
 *
 * QEMU の smc91c111 模倣では、チップ内 INT_STAT に保留がある状態でも
 * VIC IRQ 25 のラインがアサートされず、`etherInterrupt()` が一度も
 * 走らないという問題がある (実機 + 一部 QEMU build では発火する).
 *
 * Workaround として、ここで polling スレッドを 1 つ立ち上げ、5ms
 * ごとに INT_STAT を確認、masked & pending があれば
 * `etherInterrupt()` を呼んで実質同じ処理パスを通す.
 *
 * 実機 (RaspberryPi, IRQ が正常) では負荷が増えるが、5ms × n_actor
 * 程度なので問題はない.将来 IRQ 経路が直れば、本ファイル + etherOpen
 * 側の `spawn_poller` を削除すれば良い.
 */
#include "smc91c111.h"
#include <ether.h>
#include <thread.h>

extern void etherInterrupt(void);

#define POLL_INTERVAL_MS  5

static int g_poller_tid = -1;

static thread smc_poll_main(void)
{
    struct ether     *ethptr = &ethertab[0];
    struct smc91c111 *chip;
    ushort intregs;
    uchar  st, mask;

    /* etherOpen が終わって state == UP になるまで待つ. */
    while (ethptr->state != ETH_STATE_UP) {
        sleep(POLL_INTERVAL_MS);
    }
    chip = ethptr->csr;

    kprintf("[smc91c111] poller started tid=%d (every %dms)\r\n",
            thrcurrent, POLL_INTERVAL_MS);

    for (;;)
    {
        if (ethptr->state != ETH_STATE_UP) {
            sleep(POLL_INTERVAL_MS);
            continue;
        }
        smc_select_bank(chip, 2);
        intregs = smc_read16(chip, SMC_INT);
        st   = (uchar)(intregs & 0xFF);
        mask = (uchar)(intregs >> 8);
        if ((st & mask) != 0) {
            /* IRQ would have fired here — drive the handler directly. */
            etherInterrupt();
        }
        sleep(POLL_INTERVAL_MS);
    }
}

void smc_spawn_poller(void)
{
    if (g_poller_tid >= 0) {
        return;
    }
    g_poller_tid = create((void *)smc_poll_main, INITSTK, INITPRIO,
                          "smc-poller", 0);
    if (g_poller_tid >= 0) {
        ready(g_poller_tid, RESCHED_NO);
    }
}
