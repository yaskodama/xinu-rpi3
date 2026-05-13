/**
 * @file xsh_cc.c
 *
 * Tiny C compiler shell command (built-in).
 *
 *   cc SRC          -> writes ./a.out
 *   cc SRC -o OUT
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <aout.h>

shellcmd xsh_cc(int nargs, char *args[])
{
    const char *src = NULL;
    const char *out = "a.out";
    int i;

    for (i = 1; i < nargs; i++)
    {
        if (0 == strcmp(args[i], "-o") && i + 1 < nargs)
        {
            out = args[++i];
        }
        else if (args[i][0] == '-')
        {
            fprintf(stderr, "cc: unknown option %s\n", args[i]);
            return 1;
        }
        else
        {
            src = args[i];
        }
    }
    if (src == NULL)
    {
        fprintf(stderr, "Usage: cc SRC [-o OUT]\n");
        return 1;
    }
    if (OK != ccCompile(src, out))
    {
        fprintf(stderr, "cc: compilation failed\n");
        return 1;
    }
    return 0;
}
