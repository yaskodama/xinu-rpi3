/**
 * @file     main.c
 */
/* Embedded Xinu, Copyright (C) 2009, 2013.  All rights reserved. */

#include <device.h>
#include <ether.h>
#include <platform.h>
#include <shell.h>
#include <stdio.h>
#include <thread.h>
#include <version.h>
#include <xfs.h>

static void print_os_info(void);

/**
 * Main thread.  You can modify this routine to customize what Embedded Xinu
 * does when it starts up.  The default is designed to do something reasonable
 * on all platforms based on the devices and features configured.
 */
thread main(void)
{
#if HAVE_SHELL
    int shelldevs[4][3];
    uint nshells = 0;
#endif

    /* Print information about the operating system  */
    print_os_info();

    /* Initialize xfs and mount RAMDISK0 at "/" */
    if (OK != xfsBootstrap())
    {
        kprintf("WARNING: xfsBootstrap failed\r\n");
    }
    else
    {
        /* Seed sample C sources so they're available out of the box. */
        static const char hello_src[] =
            "#include <stdio.h>\n"
            "int main(void) {\n"
            "    printf(\"hello...\\n\");\n"
            "    return 0;\n"
            "}\n";
        static const char sum_src[] =
            "#include <stdio.h>\n"
            "int main(void) {\n"
            "    int i;\n"
            "    int s;\n"
            "    s = 0;\n"
            "    for (i = 1; i <= 10; i = i + 1) {\n"
            "        s = s + i;\n"
            "    }\n"
            "    printf(\"sum 1..10 = %d\\n\", s);\n"
            "    return 0;\n"
            "}\n";
        int fd;
        xfsMkdir("/home");
        fd = xfsOpen("/home/hello.c",
                     XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
        if (fd >= 0)
        {
            xfsWrite(fd, hello_src, sizeof(hello_src) - 1);
            xfsClose(fd);
        }
        fd = xfsOpen("/home/sum.c",
                     XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
        if (fd >= 0)
        {
            xfsWrite(fd, sum_src, sizeof(sum_src) - 1);
            xfsClose(fd);
        }

        /* ABCL/c+ workspace: /home/abclcp/abclc/PingPong.abcl */
        static const char pingpong_src[] =
            "/* PingPong.abcl - two actors bouncing a counter */\n"
            "class Player {\n"
            "    var hits;\n"
            "    method bounce(other, n) {\n"
            "        if (n > 0) {\n"
            "            printf(\"Player %d: tick (n=%d) hits=%d\\n\","
                            " self, n, hits);\n"
            "            hits = hits + 1;\n"
            "            send other.bounce(self, n - 1);\n"
            "        }\n"
            "    }\n"
            "}\n"
            "\n"
            "main {\n"
            "    new Player p1;\n"
            "    new Player p2;\n"
            "    send p1.bounce(p2, 6);\n"
            "}\n";
        xfsMkdir("/home/abclcp");
        xfsMkdir("/home/abclcp/abclc");
        fd = xfsOpen("/home/abclcp/abclc/PingPong.abcl",
                     XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
        if (fd >= 0)
        {
            xfsWrite(fd, pingpong_src, sizeof(pingpong_src) - 1);
            xfsClose(fd);
        }

        /* RotLines.abcl: 4 rotating lines on top of the WM */
        static const char rotlines_src[] =
            "/* RotLines.abcl - 4 lines rotating on the WM */\n"
            "class Rotator {\n"
            "    var angle;\n"
            "    method spin(n) {\n"
            "        if (n > 0) {\n"
            "            wm_line(0, 512, 384,\n"
            "                    512 + icos(angle) * 320 / 4096,\n"
            "                    384 + isin(angle) * 320 / 4096,\n"
            "                    rgb(255, 80, 80));\n"
            "            wm_line(1, 512, 384,\n"
            "                    512 + icos(angle + 90) * 320 / 4096,\n"
            "                    384 + isin(angle + 90) * 320 / 4096,\n"
            "                    rgb(80, 255, 80));\n"
            "            wm_line(2, 512, 384,\n"
            "                    512 + icos(angle + 180) * 320 / 4096,\n"
            "                    384 + isin(angle + 180) * 320 / 4096,\n"
            "                    rgb(80, 160, 255));\n"
            "            wm_line(3, 512, 384,\n"
            "                    512 + icos(angle + 270) * 320 / 4096,\n"
            "                    384 + isin(angle + 270) * 320 / 4096,\n"
            "                    rgb(255, 220, 60));\n"
            "            sleep_ms(16);\n"
            "            angle = angle + 3;\n"
            "            send self.spin(n - 1);\n"
            "        } else {\n"
            "            wm_render(0);\n"
            "        }\n"
            "    }\n"
            "    method start(n) {\n"
            "        wm_render(1);\n"
            "        send self.spin(n);\n"
            "    }\n"
            "}\n"
            "main {\n"
            "    new Rotator r;\n"
            "    send r.start(360);\n"
            "}\n";
        fd = xfsOpen("/home/abclcp/abclc/RotLines.abcl",
                     XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
        if (fd >= 0)
        {
            xfsWrite(fd, rotlines_src, sizeof(rotlines_src) - 1);
            xfsClose(fd);
        }
    }

    /* Open all ethernet devices */
#if NETHER
    {
        uint i;

        for (i = 0; i < NETHER; i++)
        {
            if (SYSERR == open(ethertab[i].dev->num))
            {
                kprintf("WARNING: Failed to open %s\r\n",
                        ethertab[i].dev->name);
            }
        }
    }
#endif /* NETHER */

    /* Set up the first TTY (CONSOLE)  */
#if defined(CONSOLE) && defined(SERIAL0)
    if (OK == open(CONSOLE, SERIAL0))
    {
  #if HAVE_SHELL
        shelldevs[nshells][0] = CONSOLE;
        shelldevs[nshells][1] = CONSOLE;
        shelldevs[nshells][2] = CONSOLE;
        nshells++;
  #endif
    }
    else
    {
        kprintf("WARNING: Can't open CONSOLE over SERIAL0\r\n");
    }
#elif defined(SERIAL0)
  #warning "No TTY for SERIAL0"
#endif

    /* Set up the second TTY (TTY1) if possible  */
#if defined(TTY1)
  #if defined(KBDMON0)
    /* Associate TTY1 with keyboard and use framebuffer output  */
    if (OK == open(TTY1, KBDMON0))
    {
    #if HAVE_SHELL
        shelldevs[nshells][0] = TTY1;
        shelldevs[nshells][1] = TTY1;
        shelldevs[nshells][2] = TTY1;
        nshells++;
    #endif
    }
    else
    {
        kprintf("WARNING: Can't open TTY1 over KBDMON0\r\n");
    }
  #elif defined(SERIAL1)
    /* Associate TTY1 with SERIAL1  */
    if (OK == open(TTY1, SERIAL1))
    {
    #if HAVE_SHELL
        shelldevs[nshells][0] = TTY1;
        shelldevs[nshells][1] = TTY1;
        shelldevs[nshells][2] = TTY1;
        nshells++;
    #endif
    }
    else
    {
        kprintf("WARNING: Can't open TTY1 over SERIAL1\r\n");
    }
  #endif /* SERIAL1 */
#else /* TTY1 */
  #if defined(KBDMON0)
    #warning "No TTY for KBDMON0"
  #elif defined(SERIAL1)
    #warning "No TTY for SERIAL1"
  #endif
#endif /* TTY1 */

    /* Auto-launch the window manager demo on arm-qemu (Versatile PB). */
#if defined(_XINU_PLATFORM_ARM_QEMU_)
    {
        extern thread wm_main(void);
        tid_typ wm_tid = create((void *)wm_main, INITSTK, INITPRIO, "WM", 0);
        if (SYSERR == ready(wm_tid, RESCHED_NO))
            kprintf("WARNING: Failed to create WM thread\r\n");
    }

    /* WM-Console device — gives a 2nd xsh shell that renders inside a
     * window on the LCD.  Add to the shell pool so the shell-spawn
     * loop below creates one for it. */
  #if defined(WMCON0) && HAVE_SHELL
    if (OK == open(WMCON0))
    {
        shelldevs[nshells][0] = WMCON0;
        shelldevs[nshells][1] = WMCON0;
        shelldevs[nshells][2] = WMCON0;
        nshells++;
    }
    else
    {
        kprintf("WARNING: Can't open WMCON0\r\n");
    }
  #endif
#endif

    /* Auto-start the bundled AIPL program (apps/abcl_program.c) when
     * compiled with -DAIPL_AUTOSTART.  Without this define, run an
     * AIPL/abcl program by hand from xsh:
     *     abclc /home/abclcp/abclc/PingPong.abcl
     *     /home/abclcp/abclc/PingPong
     *
     * The autostart path is used by the abclcp-project R1 smoke
     * harness (QEMU -nographic + serial-stdio + grep on [aipl] start).
     */
#ifdef AIPL_AUTOSTART
    {
        extern thread aipl_main(void);
        tid_typ aipl_tid = create((void *)aipl_main, 16384, INITPRIO,
                                  "AIPL", 0);
        if (SYSERR == ready(aipl_tid, RESCHED_NO))
            kprintf("WARNING: Failed to create AIPL thread\r\n");
        /* H1+H3 RemoteRPC: bring up the UART1 host-RPC dispatcher.
         * It is safe to start before AIPL actors finish booting —
         * its SEND handler bounds-checks against the current
         * n_objects, so messages that arrive too early get
         * "ERR oob id" rather than wandering into uninitialised
         * memory. */
        {
            extern void abcl_rpc_start(void);
            abcl_rpc_start();
        }
        /* Auto-start Xinu-side HTTP dashboard on port 80.  We first
         * bring up the NIC ourselves (kernel-side helper that doesn't
         * need the AIPL program to call net_init), then spawn the
         * listener thread. */
        {
            extern int  abcl_net_autoinit(void);
            extern int  abcl_http_start(int port);
            abcl_net_autoinit();
            abcl_http_start(80);
        }
    }
#endif

    /* Start shells  */
#if HAVE_SHELL
    {
        uint i;
        char name[16];

        for (i = 0; i < nshells; i++)
        {
            sprintf(name, "SHELL%u", i);
            if (SYSERR == ready(create
                                (shell, INITSTK, INITPRIO, name, 3,
                                 shelldevs[i][0],
                                 shelldevs[i][1],
                                 shelldevs[i][2]),
                                RESCHED_NO))
            {
                kprintf("WARNING: Failed to create %s", name);
            }
        }
    }
#endif

    return 0;
}

/* Start of kernel in memory (provided by linker)  */
extern void _start(void);

static void print_os_info(void)
{
    kprintf(VERSION);
    kprintf("\r\n\r\n");

#ifdef DETAIL
    /* Output detected platform. */
    kprintf("Processor identification: 0x%08X\r\n", cpuid);
    kprintf("Detected platform as: %s, %s\r\n\r\n",
            platform.family, platform.name);
#endif

    /* Output Xinu memory layout */
    kprintf("%10d bytes physical memory.\r\n",
            (ulong)platform.maxaddr - (ulong)platform.minaddr);
#ifdef DETAIL
    kprintf("           [0x%08X to 0x%08X]\r\n",
            (ulong)platform.minaddr, (ulong)(platform.maxaddr - 1));
#endif


    kprintf("%10d bytes reserved system area.\r\n",
            (ulong)_start - (ulong)platform.minaddr);
#ifdef DETAIL
    kprintf("           [0x%08X to 0x%08X]\r\n",
            (ulong)platform.minaddr, (ulong)_start - 1);
#endif

    kprintf("%10d bytes Xinu code.\r\n", (ulong)&_etext - (ulong)_start);
#ifdef DETAIL
    kprintf("           [0x%08X to 0x%08X]\r\n",
            (ulong)_start, (ulong)&_end - 1);
#endif

    kprintf("%10d bytes stack space.\r\n", (ulong)memheap - (ulong)&_end);
#ifdef DETAIL
    kprintf("           [0x%08X to 0x%08X]\r\n",
            (ulong)&_end, (ulong)memheap - 1);
#endif

    kprintf("%10d bytes heap space.\r\n",
            (ulong)platform.maxaddr - (ulong)memheap);
#ifdef DETAIL
    kprintf("           [0x%08X to 0x%08X]\r\n\r\n",
            (ulong)memheap, (ulong)platform.maxaddr - 1);
#endif
    kprintf("\r\n");
}
