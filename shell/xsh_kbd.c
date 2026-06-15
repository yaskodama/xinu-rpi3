/**
 * @file     xsh_kbd.c
 *
 * Shell command `kbd` — recover a wedged USB keyboard.
 *
 * The Pi's physical USB keyboard can hang at the bus level (a split-transaction
 * HARDWARE_ERROR provoked by other USB traffic — e.g. after using the soft
 * keyboard or an AIPL window).  Running `kbd` resets the keyboard's interrupt
 * transfer and re-arms it cleanly, bringing physical typing back without a
 * reboot.  `kbd stat` prints the keyboard's diagnostic counters.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <usbkbd.h>

shellcmd xsh_kbd(int nargs, char *args[])
{
    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s [hard|stat]\n\n", args[0]);
        printf("Description:\n");
        printf("\tRecover a wedged USB keyboard.  Use this if physical\n");
        printf("\ttyping stops working after the soft keyboard / AIPL window.\n");
        printf("Options:\n");
        printf("\t(none)\t light re-arm of the interrupt transfer\n");
        printf("\thard\t stronger: re-init the HID endpoint (for status -3)\n");
        printf("\tstat\t show keyboard diagnostic counters\n");
        printf("\t--help\t display this help and exit\n");
        return 0;
    }

    if (nargs == 2 && strcmp(args[1], "stat") == 0)
    {
        unsigned calls = 0, reports = 0, injects = 0, resub_fail = 0;
        int last = 0, icount = 0, istart = 0;
        usbKbdDiag(&calls, &reports, &last, &injects, &icount, &istart,
                   &resub_fail);
        printf("USB keyboard status:\n");
        printf("  int callbacks : %u\n", calls);
        printf("  reports parsed: %u\n", reports);
        printf("  last status   : %d\n", last);
        printf("  soft injects  : %u\n", injects);
        printf("  ring count    : %d\n", icount);
        printf("  resubmit fail : %u\n", resub_fail);
        return 0;
    }

    if (nargs == 2 && strcmp(args[1], "hard") == 0)
    {
        if (usbKbdReviveHard() == OK)
        {
            printf("keyboard endpoint re-initialized — try typing now.\n");
            return 0;
        }
        fprintf(stderr, "hard recovery failed (no keyboard, or EP0 unreachable).\n");
        return 1;
    }

    if (nargs > 1)
    {
        fprintf(stderr, "%s: unknown argument '%s'\n", args[0], args[1]);
        fprintf(stderr, "Try '%s --help'.\n", args[0]);
        return 1;
    }

    if (usbKbdRevive() == OK)
    {
        printf("keyboard re-armed — try typing now.\n");
        return 0;
    }
    fprintf(stderr, "no initialized USB keyboard to recover.\n");
    return 1;
}
