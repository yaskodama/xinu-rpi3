/**
 * @file xsh_win.c
 *
 * `win` shell command: open a new window on the HDMI desktop running an
 * interactive xsh shell.  Keyboard input comes from the USB keyboard
 * (KBDMON0), command output is rendered inside the window via the
 * GWINCON0 window-console device.
 */
/* Embedded Xinu, arm-rpi3 window manager add-on. */

#include <kernel.h>
#include <thread.h>
#include <stdio.h>
#include <shell.h>
#include <device.h>

#ifdef _XINU_PLATFORM_ARM_RPI3_
#include <gwm.h>

shellcmd xsh_win(int nargs, char *args[])
{
    (void)nargs;
    (void)args;

    /* Make sure the on-screen shell window exists (idempotent). */
    gwin_shell_window_open();

    /* Open the window-console output device. */
    open(GWINCON0);

    /* Spawn an interactive shell: input from the USB keyboard (KBDMON0),
     * stdout + stderr into the window (GWINCON0).  Cortex-A53 frames are
     * large and the render chain is deep, so use a big (64 KB) stack. */
    ready(create((void *)shell, 65536, INITPRIO, "winsh", 3,
                 KBDMON0, GWINCON0, GWINCON0), RESCHED_YES);

    return 0;
}

#else  /* not arm-rpi3: the WM / window-console devices don't exist */

shellcmd xsh_win(int nargs, char *args[])
{
    (void)nargs;
    (void)args;
    fprintf(stderr, "win: only supported on the arm-rpi3 (HDMI) platform\n");
    return 1;
}

#endif /* _XINU_PLATFORM_ARM_RPI3_ */
