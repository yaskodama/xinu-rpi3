/**
 * @file xsh_kexec.c
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#include <conf.h>
#include <device.h>
#include <kexec.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <fat.h>

#if NETHER
#  include <tftp.h>
#  include <dhcpc.h>
#  include <network.h>
#endif

static void usage(const char *command);

static void kexec_from_network(int netdev);
static void kexec_from_uart(int uartdev);
static void kexec_from_sd(const char *file);

/**
 * @ingroup shell
 *
 * Kernel execute- Transfer control to a new kernel.
 */
shellcmd xsh_kexec(int nargs, char *args[])
{
    int dev;

    /* Output help, if '--help' argument was supplied */
    if (2 == nargs && 0 == strcmp(args[1], "--help"))
    {
        usage(args[0]);
        return SHELL_OK;
    }

    /* `kexec` with no arguments: boot the default (full) kernel, OS0.IMG. */
    if (1 == nargs)
    {
        kexec_from_sd("OS0.IMG");
        return SHELL_ERROR;               /* only returns if the load failed */
    }

    /* `kexec <PATH>` : load a kernel image directly off the SD and run it,
     * e.g.  kexec /sd/OS1.IMG  or  kexec OS2.IMG  (anything not a -flag). */
    if (2 == nargs && args[1][0] != '-')
    {
        kexec_from_sd(args[1]);
        return SHELL_ERROR;               /* only returns if the load failed */
    }

    if (3 != nargs)
    {
        fprintf(stderr, "ERROR: Wrong number of arguments.\n");
        usage(args[0]);
        return SHELL_ERROR;
    }

    if (0 == strcmp(args[1], "-n"))
    {
        dev = getdev(args[2]);
        if (SYSERR == dev)
        {
            fprintf(stderr, "ERROR: device \"%s\" not found.\n", args[2]);
            return SHELL_ERROR;
        }
    #if NETHER
        if (dev < ETH0 || dev >= ETH0 + NETHER)
    #endif
        {
            fprintf(stderr, "ERROR: \"%s\" is not a valid network device.\n",
                    args[2]);
            return SHELL_ERROR;
        }
        kexec_from_network(dev);
    }
    else if (0 == strcmp(args[1], "-u"))
    {
        dev = getdev(args[2]);
        if (SYSERR == dev)
        {
            fprintf(stderr, "ERROR: device \"%s\" not found.\n", args[2]);
            return SHELL_ERROR;
        }
    #if NUART
        if (dev < SERIAL0 || dev >= SERIAL0 + NUART)
    #endif
        {
            fprintf(stderr, "ERROR: \"%s\" is not a valid UART device.\n",
                    args[2]);
            return SHELL_ERROR;
        }
        kexec_from_uart(dev);
    }
    else if (0 == strcmp(args[1], "-s"))
    {
        kexec_from_sd(args[2]);          /* boot a kernel image off the SD card */
    }
    else
    {
        usage(args[0]);
    }

    return SHELL_ERROR;
}

/* Load a kernel image file (e.g. OS1.IMG / OS2.IMG) off the SD card into RAM
 * and transfer control to it with kexec().  Tries the on-board /microsd first,
 * then a USB card reader.  The 2 MB buffer lives in BSS (high RAM), well clear
 * of the 0x8000 load address kexec() copies into.  Does not return on success. */
static unsigned char kx_buf[2 * 1024 * 1024];
static int           kx_len;
static int kx_read_cb(const unsigned char *b, int len, void *ctx)
{
    (void)ctx;
    if (kx_len + len > (int)sizeof kx_buf) return 1;   /* image too big */
    memcpy(kx_buf + kx_len, b, len);
    kx_len += len;
    return 0;
}
static int kx_read_from(const char *file)
{
    struct fat_dirent e;
    kx_len = 0;
    if (fat_mount() != 0 || fat_find_root(file, &e) != 0 || e.is_dir)
        return -1;
    if (fat_read_file(e.cluster, e.size, kx_read_cb, 0) != 0)
        return -1;
    return kx_len;
}
/* Strip any directory prefix: "/sd/OS1.IMG" -> "OS1.IMG", "OS1.IMG" -> "OS1.IMG".
 * The FAT layer only knows root-directory entries, so the leading /sd, /microsd
 * etc. are cosmetic mount-point names we ignore. */
static const char *kx_basename(const char *p)
{
    const char *b = p, *s;
    for (s = p; *s; s++)
        if (*s == '/') b = s + 1;
    return b;
}

static void kexec_from_sd(const char *path)
{
    extern int usbmsc_fat_select(void);
    const char *file = kx_basename(path);
    int n;

    /* Read the chosen image off the SD into RAM (on-board /microsd, then USB). */
    fat_set_blkdev(0, 0);
    n = kx_read_from(file);
    if (n < 0 && usbmsc_fat_select() == 0)
        n = kx_read_from(file);
    fat_set_blkdev(0, 0);

    if (n <= 0)
    {
        fprintf(stderr, "ERROR: could not read \"%s\" from SD.\n", file);
        return;
    }

    /* Transfer control to the new kernel by copying it to 0x8000 and jumping
     * (warm kexec).  NOTE: this is a WARM restart — the USB host, SD and
     * network are NOT cold-reset, so the keyboard/mouse/Ethernet may not work
     * in the new kernel.  The HDMI framebuffer survives, so a no-window-system
     * kernel's on-screen text prompt does appear.  For a fully-reset switch,
     * copy the image to kernel.img on the SD from a host and power-cycle. */
    printf("Loaded %s (%d bytes); starting it (warm kexec)...\n", file, n);
    kexec(kx_buf, (uint)n);              /* copies to 0x8000 and jumps — no return */
    fprintf(stderr, "ERROR: kexec failed.\n");
}

static void usage(const char *command)
{
        printf(
"Usage: %s [PATH | -n DEV | -u DEV | -s FILE]\n\n"
"Description:\n"
"\tLoads and executes a new kernel.\n"
"\tkexec <PATH>   load and run an SD image directly, e.g.\n"
"\t                   kexec /sd/OS1.IMG     (minimal kernel)\n"
"\t                   kexec /sd/OS2.IMG     (no window system)\n"
"\t                   kexec OS0.IMG         (full kernel)\n"
"\tkexec          (no args) boots the default full kernel OS0.IMG.\n"
"\tNOTE: this is a WARM restart on the Pi3 - the HDMI text prompt appears\n"
"\tbut USB/network may not work in the new kernel (no cold reset).\n"
"Options:\n"
"\t-n <NETDEV>    Load the new kernel over the specified network device.\n"
"\t               This will bring down the corresponding network\n"
"\t               interface (if it's up), then use DHCP to get a network\n"
"\t               address and information about the TFTP server hosting\n"
"\t               the boot file.  The boot file (new kernel) will then be\n"
"\t               downloaded using TFTP and executed.\n"
#ifdef _XINU_PLATFORM_ARM_RPI_
"\t-u <UARTDEV>   Load the new kernel over the specified UART device.\n"
"\t               This is currently a Raspberry-Pi specific feature\n"
"\t               and is designed to be used with \"raspbootcom\"\n"
"\t               running on the other end of the serial connection.\n"
"\t-s <FILE>      Same as `kexec <FILE>` (load an SD image and run it).\n"
#endif
"\t--help         display this help and exit\n"

        , command);
}

static void kexec_from_network(int netdev)
{
#if defined(WITH_DHCPC) && NETHER != 0
    struct dhcpData data;
    int result;
    const struct netaddr *gatewayptr;
    struct netif *nif;
    void *kernel;
    uint size;
    char str_ip[20];
    char str_mask[20];
    char str_gateway[20];
    const char *netdevname = devtab[netdev].name;

    /* Bring network interface (if any) down.  */
    netDown(netdev);

    /* Run DHCP client on the device for at most 10 seconds.  */
    printf("Running DHCP on %s\n", netdevname);
    result = dhcpClient(netdev, 10, &data);
    if (OK != result)
    {
        fprintf(stderr, "ERROR: DHCP failed.\n");
        return;
    }

    /* Ensure the DHCP server provided the boot filename and TFTP server IP
     * address.  */
    if ('\0' == data.bootfile[0] || 0 == data.next_server.type)
    {
        fprintf(stderr, "ERROR: DHCP server did not provide boot file "
                "and TFTP server address.\n");
        return;
    }

    /* Bring up the network interface.  */
    netaddrsprintf(str_ip, &data.ip);
    netaddrsprintf(str_mask, &data.mask);
    if (0 != data.gateway.len)
    {
        netaddrsprintf(str_gateway, &data.gateway);
        printf("Bringing up %s as %s with mask %s (gateway %s)\n",
               netdevname, str_ip, str_mask, str_gateway);
        gatewayptr = &data.gateway;
    }
    else
    {
        printf("Bringing up %s as %s with mask %s (no gateway)\n",
               netdevname, str_ip, str_mask);
        gatewayptr = NULL;
    }
    result = netUp(netdev, &data.ip, &data.mask, gatewayptr);
    if (OK != result)
    {
        fprintf(stderr, "ERROR: failed to bring up %s.\n", netdevname);
        return;
    }
    nif = netLookup(netdev);

    /* Download new kernel using TFTP.  */
    netaddrsprintf(str_ip, &data.next_server);
    printf("Downloading bootfile \"%s\" from TFTP server %s\n",
           data.bootfile, str_ip);
    kernel = (void*)tftpGetIntoBuffer(data.bootfile, &nif->ip,
                                      &data.next_server, &size);

    if (SYSERR == (int)kernel)
    {
        fprintf(stderr, "ERROR: TFTP failed.\n");
        return;
    }

    /* Execute the new kernel.  */
    printf("Executing new kernel (size=%u)\n", size);
    sleep(100);  /* Wait just a fraction of a second for printf()s to finish
                    (no guarantees though).  */
    kexec(kernel, size);

    fprintf(stderr, "ERROR: kexec() returned!\n");

#else /* WITH_DHCPC && NETHER != 0 */
    fprintf(stderr,
            "ERROR: Network boot is not supported in this configuration.\n"
            "       Please make sure you have enabled one or more network\n"
            "       devices, along with the DHCP and TFTP clients.\n");
#endif /* !(WITH_DHCPC && NETHER != 0) */
}

static void kexec_from_uart(int uartdev)
{
#ifdef _XINU_PLATFORM_ARM_RPI_
    irqmask im;
    device *uart;
    ulong size;
    void *kernel;
    uchar *p;
    ulong n;

    im = disable();

    uart = (device*)&devtab[uartdev];

    /* Tell raspbootcom to send the new kernel.  */
    kputc('\x03', uart);
    kputc('\x03', uart);
    kputc('\x03', uart);

    /* Receive size of the new kernel.  */
    for (;;)
    {
        size = (ulong)kgetc(uart);
        size |= (ulong)kgetc(uart) << 8;
        size |= (ulong)kgetc(uart) << 16;
        size |= (ulong)kgetc(uart) << 24;
        if (size <= 99999999 && size != 0)
        {
            break;
        }

        /* Tell raspbootcom to re-send the size.  */
        kputc('S', uart);
        kputc('E', uart);
    }

    /* Tell raspbootcom the size was successfully received.  */
    kputc('O', uart);
    kputc('K', uart);

    /* Allocate buffer for new kernel.  */
    kernel = memget(size);
    if (kernel == (void*)SYSERR)
    {
        return;
    }

    /* Load new kernel over the UART, placing it into the buffer.  */
    p = kernel;
    n = size;
    while (n--)
    {
        *p++ = kgetc(uart);
    }

    /* Execute the new kernel.  */
    kexec(kernel, size);

    /* The following code should never actually be reached.  */
    memfree(kernel, size);
    restore(im);
#else /* _XINU_PLATFORM_ARM_RPI_ */
    fprintf(stderr, "ERROR: kexec from UART not supported on this platform.\n");
#endif /* !_XINU_PLATFORM_ARM_RPI_ */
}
