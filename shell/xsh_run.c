/**
 * @file xsh_run.c
 *
 * Run an a.out file produced by `cc`.
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <aout.h>

shellcmd xsh_run(int nargs, char *args[])
{
    int rc;
    if (nargs < 2)
    {
        fprintf(stderr, "Usage: run PATH\n");
        return 1;
    }
    rc = aoutRun(args[1]);
    if (rc == SYSERR)
    {
        fprintf(stderr, "run: %s: cannot execute\n", args[1]);
        return 1;
    }
    return rc;
}
