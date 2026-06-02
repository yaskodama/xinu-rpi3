/**
 * @file     xsh_wine.c
 *
 * Shell command `wine`: spin a wire-frame 3-D wine glass 30 times in the
 * gwm "graphics" window (arm-rpi3 HDMI desktop).
 */
/* Embedded Xinu, arm-rpi3 window manager add-on. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/**
 * @ingroup shell
 *
 * @param nargs number of arguments
 * @param args  argument vector
 * @return 0
 */
shellcmd xsh_wine(int nargs, char *args[])
{
    if (nargs == 2 && strncmp(args[1], "--help", 6) == 0) {
        printf("Usage: %s\n\n", args[0]);
        printf("Spin a wire-frame 3-D wine glass 30 times in the graphics window.\n");
        return 0;
    }
#ifdef _XINU_PLATFORM_ARM_RPI3_
    {
        extern void gwm_start_wine(void);
        gwm_start_wine();
        printf("wine: spinning the wire-frame glass 30x in the graphics window\n");
    }
#else
    printf("wine: only available on the arm-rpi3 HDMI desktop\n");
#endif
    return 0;
}
