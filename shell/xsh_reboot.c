/**
 * @file xsh_reboot.c
 *
 * Shell command `reboot`: trigger a full BCM2837 watchdog reset, the same
 * mechanism as the webactor /reboot route.  The SoC resets and the firmware
 * re-loads kernel.img from the SD card — a clean cold-style restart (USB,
 * WiFi and ethernet all come back, unlike a /kexec warm jump).
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <watchdog.h>

shellcmd xsh_reboot(int nargs, char *args[])
{
    (void)nargs; (void)args;
    printf("Rebooting...\n");
    watchdogset(1);            /* full reset in ~1 ms */
    while (1) { }              /* never returns */
    return 0;
}
