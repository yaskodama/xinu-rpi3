/**
 * @file     xsh_wifi.c
 *
 * Shell command `wifi`:
 *   wifi status  — show the current WiFi connection (SSID + IP)
 *   wifi on      — if not connected, scan, list candidate APs and let the
 *                  user pick one by number, then prompt for a password
 *                  and connect (join + DHCP)
 *   wifi off     — disconnect (bring the BSS down)
 */
/* Embedded Xinu, arm-rpi3 WiFi (BCM43455) add-on. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _XINU_PLATFORM_ARM_RPI3_

extern int         wifi_connected(void);
extern const char *wifi_ssid(void);
extern void        wifi_dhcp_diag(unsigned char *ip, unsigned char *gw, int *have);
extern int         wifi_scan_ssids(char ssids[][40], int max);
extern int         wifi_join(const char *ssid, const char *pass);
extern int         wifi_dhcp(void);
extern void        wifi_disconnect(void);

/* Read a line from `fd` into buf (echoing to stdout).  `mask` prints '*'
 * instead of the typed character (for passwords).  Stops on CR/LF. */
static void wifi_readline(int fd, char *buf, int max, int mask)
{
    int i = 0, c;
    while ((c = fgetc(fd)) != EOF && c != '\n' && c != '\r') {
        if (c == '\b' || c == 0x7F) {            /* backspace */
            if (i > 0) { i--; printf("\b \b"); }
            continue;
        }
        if (c < 0x20) continue;                  /* drop other controls */
        if (i < max - 1) {
            buf[i++] = (char)c;
            printf("%c", mask ? '*' : c);        /* local echo */
        }
    }
    buf[i] = '\0';
    printf("\n");
}

static void wifi_show_status(void)
{
    if (wifi_connected()) {
        unsigned char ip[4], gw[4]; int have;
        wifi_dhcp_diag(ip, gw, &have);
        printf("WiFi: connected to \"%s\"\n", wifi_ssid());
        printf("  ip %u.%u.%u.%u  gw %u.%u.%u.%u\n",
               ip[0], ip[1], ip[2], ip[3], gw[0], gw[1], gw[2], gw[3]);
    } else {
        printf("WiFi: not connected\n");
    }
}

shellcmd xsh_wifi(int nargs, char *args[])
{
    if (nargs < 2) {
        printf("Usage: %s on|off|status\n", args[0]);
        return 0;
    }

    if (strcmp(args[1], "status") == 0) {
        wifi_show_status();
        return 0;
    }

    if (strcmp(args[1], "off") == 0) {
        wifi_disconnect();
        printf("WiFi: disconnected\n");
        return 0;
    }

    if (strcmp(args[1], "adhoc") == 0) {        /* MANET ad-hoc (IBSS) node */
        extern int wifi_adhoc(const char *ssid, int channel, int n);
        int ch = (nargs >= 4) ? atoi(args[3]) : 6;
        int node = (nargs >= 5) ? atoi(args[4]) : 2;
        if (nargs < 3) { printf("Usage: %s adhoc <ssid> [ch] [node]\n", args[0]); return 0; }
        printf("Joining ad-hoc cell \"%s\" ch=%d as 10.0.0.%d ...\n", args[2], ch, node);
        if (wifi_adhoc(args[2], ch, node) != 0) { printf("WiFi: adhoc failed\n"); return 0; }
        printf("WiFi: ad-hoc up, ip 10.0.0.%d (responder running)\n", node);
        return 0;
    }

    if (strcmp(args[1], "on") == 0) {
        static char ssids[24][40];
        char sel[8], pass[68];
        int n, num;

        if (wifi_connected()) {
            printf("WiFi: already connected to \"%s\"\n", wifi_ssid());
            printf("(run 'wifi off' first to switch networks)\n");
            return 0;
        }
        printf("Scanning for access points (~30s; the radio drops briefly)...\n");
        n = wifi_scan_ssids(ssids, 24);
        if (n <= 0) { printf("No access points found.\n"); return 0; }

        printf("Available networks:\n");
        for (num = 0; num < n; num++)
            printf("  %d: %s\n", num + 1, ssids[num]);

        printf("Select AP number (0 = cancel): ");
        wifi_readline(stdin, sel, sizeof(sel), 0);
        num = atoi(sel);
        if (num < 1 || num > n) { printf("Cancelled.\n"); return 0; }

        printf("Password for \"%s\" (blank = open): ", ssids[num - 1]);
        wifi_readline(stdin, pass, sizeof(pass), 1);

        printf("Connecting to \"%s\"...\n", ssids[num - 1]);
        if (wifi_join(ssids[num - 1], pass) != 0) {
            printf("WiFi: join failed.\n");
            return 0;
        }
        if (wifi_dhcp() != 0 || !wifi_connected()) {
            printf("WiFi: DHCP failed (no IP).\n");
            return 0;
        }
        wifi_show_status();
        return 0;
    }

    printf("Usage: %s on|off|status\n", args[0]);
    return 0;
}

#else  /* not arm-rpi3 */

shellcmd xsh_wifi(int nargs, char *args[])
{
    (void)nargs; (void)args;
    printf("wifi: only available on the arm-rpi3 (BCM43455) build\n");
    return 0;
}

#endif
