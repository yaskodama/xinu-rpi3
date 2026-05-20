/**
 * @file     xsh_halt.c
 *
 * Shell command (halt) — stop the system.  The actual platform-specific
 * shutdown lives in system/halt.c (see include/halt.h) because the same
 * sequence is also reachable from the WM mini-shell and topbar button.
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <halt.h>

/**
 * @ingroup shell
 *
 * Shell command (halt) stops the system.
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error (never returns on success)
 */
shellcmd xsh_halt(int nargs, char *args[])
{
    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tHalts the system.  On QEMU this terminates the emulator;\n");
        printf("\ton bare-metal Raspberry Pi the CPU is parked in WFI.\n");
        printf("Options:\n");
        printf("\t--help\t display this help and exit\n");
        return 1;
    }

    if (nargs > 1)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information.\n", args[0]);
        return 1;
    }

    system_halt();
    return 0;       /* unreachable */
}
