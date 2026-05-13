/**
 * @file xsh_abclc.c
 *
 * Shell command: abclc SRC.abcl [-o OUT]
 *
 * Translates the .abcl source to a .c file, then chains through ccCompile()
 * to produce a runnable a.out.  Default output basename = source basename
 * (stripped of .abcl), placed alongside the source.
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <abclc.h>
#include <aout.h>

static int rfind(const char *s, char c)
{
    int i, last = -1;
    for (i = 0; s[i]; i++) if (s[i] == c) last = i;
    return last;
}

static void derive_default(const char *src, char *cpath, char *binpath,
                           int sz)
{
    int dot = rfind(src, '.');
    int len;
    if (dot < 0) dot = (int)strlen(src);
    len = dot;
    if (len > sz - 4) len = sz - 4;
    memcpy(cpath, src, len);
    memcpy(cpath + len, ".c", 3);
    memcpy(binpath, src, len);
    binpath[len] = 0;
}

shellcmd xsh_abclc(int nargs, char *args[])
{
    const char *src = NULL;
    const char *out = NULL;
    char cpath[256];
    char binpath[256];
    int i;

    for (i = 1; i < nargs; i++)
    {
        if (0 == strcmp(args[i], "-o") && i + 1 < nargs)
        {
            out = args[++i];
        }
        else if (args[i][0] == '-')
        {
            fprintf(stderr, "abclc: unknown option %s\n", args[i]);
            return 1;
        }
        else
        {
            src = args[i];
        }
    }
    if (src == NULL)
    {
        fprintf(stderr, "Usage: abclc SRC.abcl [-o OUT]\n");
        return 1;
    }

    if (out == NULL)
    {
        derive_default(src, cpath, binpath, sizeof(cpath));
    }
    else
    {
        int len;
        len = strlen(out);
        if (len + 3 >= (int)sizeof(cpath))
        { fprintf(stderr, "abclc: output path too long\n"); return 1; }
        memcpy(cpath, out, len); memcpy(cpath + len, ".c", 3);
        strlcpy(binpath, out, sizeof(binpath));
    }

    if (OK != abclcTranslate(src, cpath))
    {
        fprintf(stderr, "abclc: translation failed\n");
        return 1;
    }
    printf("abclc: translated %s -> %s\n", src, cpath);

    if (OK != ccCompile(cpath, binpath))
    {
        fprintf(stderr, "abclc: cc failed\n");
        return 1;
    }
    printf("abclc: compiled %s -> %s\n", cpath, binpath);
    return 0;
}
