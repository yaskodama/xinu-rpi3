/**
 * @file     xsh_wifi_invest.c
 *
 * Shell command `wifi-invest`: when WiFi is connected but ping does not
 * get through, investigate + try to repair the data path (restart the
 * ARP/ICMP responder, send a gratuitous ARP, probe the gateway) and, if
 * it still cannot be made to work, print the most likely reason.
 */
/* Embedded Xinu, arm-rpi3 WiFi (BCM43455) add-on. */

#include <stddef.h>
#include <stdio.h>

shellcmd xsh_wifi_invest(int nargs, char *args[])
{
    (void)nargs; (void)args;
#ifdef _XINU_PLATFORM_ARM_RPI3_
    {
        extern int wifi_investigate(char *out, int cap);
        static char buf[1024];
        printf("Investigating the WiFi data path (probes the gateway, ~2s)...\n");
        wifi_investigate(buf, sizeof(buf));
        printf("%s", buf);
    }
#else
    printf("wifi-invest: only available on the arm-rpi3 (BCM43455) build\n");
#endif
    return 0;
}
