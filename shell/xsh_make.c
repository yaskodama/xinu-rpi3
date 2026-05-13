/**
 * @file xsh_make.c
 *
 * Tiny make.  Reads ./Makefile from the current directory; auto-generates
 * one from *.c and *.abcl on first use.
 *
 *   make             build the default target list (TARGETS = ...)
 *   make TARGET ...  build the named targets
 *
 * Makefile format:
 *
 *   # comments start with '#'
 *   TARGETS = hello sum PingPong RotLines       (= default target list)
 *   hello:    hello.c
 *   sum:      sum.c
 *   PingPong: PingPong.abcl
 *   RotLines: RotLines.abcl
 *
 * Actions are inferred from the source suffix:
 *   .c     -> cc SRC -o TARGET
 *   .abcl  -> abclc SRC -o TARGET   (translate + cc, in one shot)
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <xfs.h>
#include <aout.h>
#include <abclc.h>

#define MK_NAMELEN     64
#define MK_MAX_RULES   32
#define MK_MAX_DEFLT   32
#define MK_MAX_LINES   4096

struct mk_rule {
    char target[MK_NAMELEN];
    char source[MK_NAMELEN];
};

static struct mk_rule g_rules[MK_MAX_RULES];
static int  g_nrules;
static char g_default[MK_MAX_DEFLT][MK_NAMELEN];
static int  g_ndefault;

/* ---------- helpers ---------- */

static int has_suffix(const char *s, const char *suf)
{
    int sl  = strlen(s);
    int sfl = strlen(suf);
    if (sl < sfl) return 0;
    return 0 == memcmp(s + sl - sfl, suf, sfl);
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int copy_token(const char **pp, char *dst, int dstmax)
{
    const char *p = skip_ws(*pp);
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != ':' &&
           *p != '\n' && *p != '\r' && *p != '=' && n < dstmax - 1)
        dst[n++] = *p++;
    dst[n] = 0;
    *pp = p;
    return n;
}

static const struct mk_rule *find_rule(const char *target)
{
    int i;
    for (i = 0; i < g_nrules; i++)
        if (0 == strcmp(g_rules[i].target, target))
            return &g_rules[i];
    return NULL;
}

/* ---------- generate Makefile from cwd contents ---------- */

static int write_str(int fd, const char *s)
{
    int n = strlen(s);
    return (xfsWrite(fd, s, n) == n) ? OK : SYSERR;
}

static int generate_makefile(const char *makefile_path,
                             const char *cwd)
{
    int fd;
    struct xdirent ent;
    uint32_t idx = 0, next;
    char nm[XFS_MAX_NAME + 1];
    int   n_c = 0, n_abcl = 0;

    /* Two passes: TARGETS line first, then rules. */

    fd = xfsOpen(makefile_path,
                 XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
    if (fd < 0) return SYSERR;

    write_str(fd,
        "# XINU Makefile  (auto-generated; edit freely)\n"
        "#\n"
        "# Format:\n"
        "#   TARGETS = list of default targets used when `make` has no args\n"
        "#   target: source       (one rule per line; trailing blank lines OK)\n"
        "#\n"
        "# Actions inferred from the source suffix:\n"
        "#   .c    -> cc    SRC -o TARGET\n"
        "#   .abcl -> abclc SRC -o TARGET   (translate + cc in one shot)\n"
        "\n"
        "TARGETS =");

    /* Pass 1: emit each TARGET name into the TARGETS variable. */
    while (OK == xfsReaddir(cwd, idx, &ent, &next))
    {
        idx = next;
        if (ent.type == XFS_T_DIR) continue;
        memcpy(nm, ent.name, ent.name_len); nm[ent.name_len] = 0;
        if (has_suffix(nm, ".c"))
        {
            nm[ent.name_len - 2] = 0;
            write_str(fd, " "); write_str(fd, nm);
            n_c++;
        }
        else if (has_suffix(nm, ".abcl"))
        {
            nm[ent.name_len - 5] = 0;
            write_str(fd, " "); write_str(fd, nm);
            n_abcl++;
        }
    }
    write_str(fd, "\n\n");

    /* Pass 2: emit one `target: source` rule per file. */
    idx = 0;
    while (OK == xfsReaddir(cwd, idx, &ent, &next))
    {
        idx = next;
        if (ent.type == XFS_T_DIR) continue;
        memcpy(nm, ent.name, ent.name_len); nm[ent.name_len] = 0;
        if (has_suffix(nm, ".c"))
        {
            char base[XFS_MAX_NAME + 1];
            memcpy(base, nm, ent.name_len - 2);
            base[ent.name_len - 2] = 0;
            write_str(fd, base); write_str(fd, ": ");
            write_str(fd, nm);   write_str(fd, "\n");
        }
        else if (has_suffix(nm, ".abcl"))
        {
            char base[XFS_MAX_NAME + 1];
            memcpy(base, nm, ent.name_len - 5);
            base[ent.name_len - 5] = 0;
            write_str(fd, base); write_str(fd, ": ");
            write_str(fd, nm);   write_str(fd, "\n");
        }
    }
    xfsClose(fd);

    printf("make: generated Makefile (%d .c, %d .abcl)\n", n_c, n_abcl);
    return (n_c + n_abcl > 0) ? OK : SYSERR;
}

/* ---------- parse Makefile ---------- */

static int parse_makefile(const char *path)
{
    int fd, n, i;
    char *buf;
    struct xinode in;
    const char *p, *line, *line_end;

    g_nrules = 0;
    g_ndefault = 0;

    if (OK != xfsStat(path, &in, NULL)) return SYSERR;
    buf = (char *)memget(in.size + 1);
    if ((void *)SYSERR == buf) return SYSERR;
    fd = xfsOpen(path, XFS_O_RDONLY);
    if (fd < 0) { memfree(buf, in.size + 1); return SYSERR; }
    n = xfsRead(fd, buf, in.size);
    xfsClose(fd);
    if (n < 0) n = 0;
    buf[n] = 0;

    p = buf;
    while (*p)
    {
        line = p;
        while (*p && *p != '\n') p++;
        line_end = p;
        if (*p) p++;

        /* Skip blank/comment */
        {
            const char *q = skip_ws(line);
            if (q >= line_end || *q == '#') continue;

            /* Variable: KEY = ... */
            {
                const char *eq = q;
                while (eq < line_end && *eq != '=' && *eq != ':') eq++;
                if (eq < line_end && *eq == '=')
                {
                    char key[MK_NAMELEN];
                    int kn = 0;
                    const char *k = q;
                    while (k < eq && *k != ' ' && *k != '\t' && kn < MK_NAMELEN-1)
                        key[kn++] = *k++;
                    key[kn] = 0;
                    if (0 == strcmp(key, "TARGETS"))
                    {
                        const char *t = eq + 1;
                        while (t < line_end)
                        {
                            char tok[MK_NAMELEN];
                            const char *tp = t;
                            int tn = copy_token(&tp, tok, MK_NAMELEN);
                            if (tn == 0) break;
                            if (g_ndefault < MK_MAX_DEFLT)
                                strlcpy(g_default[g_ndefault++], tok, MK_NAMELEN);
                            t = tp;
                        }
                    }
                    continue;
                }
                /* Rule: TARGET: SOURCE */
                if (eq < line_end && *eq == ':')
                {
                    char tgt[MK_NAMELEN], src[MK_NAMELEN];
                    int  tn = 0, sn = 0;
                    const char *k = q;
                    while (k < eq && *k != ' ' && *k != '\t' && tn < MK_NAMELEN-1)
                        tgt[tn++] = *k++;
                    tgt[tn] = 0;
                    {
                        const char *s = skip_ws(eq + 1);
                        while (s < line_end && *s != ' ' && *s != '\t' &&
                               *s != '\r' && sn < MK_NAMELEN-1)
                            src[sn++] = *s++;
                        src[sn] = 0;
                    }
                    if (tn > 0 && sn > 0 && g_nrules < MK_MAX_RULES)
                    {
                        strlcpy(g_rules[g_nrules].target, tgt, MK_NAMELEN);
                        strlcpy(g_rules[g_nrules].source, src, MK_NAMELEN);
                        g_nrules++;
                    }
                }
            }
        }
    }

    memfree(buf, in.size + 1);
    /* If TARGETS wasn't set, default to all rules in declaration order. */
    if (g_ndefault == 0)
    {
        for (i = 0; i < g_nrules && g_ndefault < MK_MAX_DEFLT; i++)
            strlcpy(g_default[g_ndefault++], g_rules[i].target, MK_NAMELEN);
    }
    return OK;
}

/* ---------- build a single target ---------- */

static int build_target(const char *target)
{
    const struct mk_rule *r = find_rule(target);
    char src_inferred[MK_NAMELEN];
    const char *src;
    char cpath[XFS_PATH_MAX];

    if (r) src = r->source;
    else
    {
        /* No explicit rule: try TARGET.c then TARGET.abcl */
        struct xinode tmp;
        sprintf(src_inferred, "%s.c", target);
        if (OK == xfsStat(src_inferred, &tmp, NULL))
            src = src_inferred;
        else
        {
            sprintf(src_inferred, "%s.abcl", target);
            if (OK == xfsStat(src_inferred, &tmp, NULL))
                src = src_inferred;
            else
            {
                fprintf(stderr, "make: no rule and no source for %s\n",
                        target);
                return SYSERR;
            }
        }
    }

    if (has_suffix(src, ".c"))
    {
        printf("    cc %s -o %s\n", src, target);
        if (OK != ccCompile(src, target))
        { fprintf(stderr, "    -> failed\n"); return SYSERR; }
        return OK;
    }
    if (has_suffix(src, ".abcl"))
    {
        printf("    abclc %s -> %s.c -> %s\n", src, target, target);
        sprintf(cpath, "%s.c", target);
        if (OK != abclcTranslate(src, cpath))
        { fprintf(stderr, "    -> abclc failed\n"); return SYSERR; }
        if (OK != ccCompile(cpath, target))
        { fprintf(stderr, "    -> cc failed\n"); return SYSERR; }
        return OK;
    }
    fprintf(stderr, "make: don't know how to build %s from %s\n",
            target, src);
    return SYSERR;
}

/* ---------- top-level command ---------- */

shellcmd xsh_make(int nargs, char *args[])
{
    char cwd[XFS_PATH_MAX];
    char makefile_path[XFS_PATH_MAX];
    struct xinode tmp;
    int rc = 0;
    int i;
    int built = 0;

    if (OK != xfsGetcwd(cwd, sizeof(cwd)))
    {
        fprintf(stderr, "make: cannot determine cwd\n");
        return 1;
    }
    {
        const char *sep = (strlen(cwd) > 0 && cwd[strlen(cwd)-1] != '/')
                          ? "/" : "";
        sprintf(makefile_path, "%s%sMakefile", cwd, sep);
    }

    /* Auto-generate Makefile if missing. */
    if (OK != xfsStat(makefile_path, &tmp, NULL))
    {
        if (OK != generate_makefile(makefile_path, cwd))
        {
            fprintf(stderr, "make: nothing to build in %s\n", cwd);
            return 1;
        }
    }

    if (OK != parse_makefile(makefile_path))
    {
        fprintf(stderr, "make: parse error\n");
        return 1;
    }

    if (nargs >= 2)
    {
        for (i = 1; i < nargs; i++)
        {
            if (OK != build_target(args[i])) rc = 1;
            else built++;
        }
    }
    else
    {
        if (g_ndefault == 0)
        {
            fprintf(stderr, "make: no targets\n");
            return 1;
        }
        for (i = 0; i < g_ndefault; i++)
        {
            if (OK != build_target(g_default[i])) rc = 1;
            else built++;
        }
    }
    if (rc == 0) printf("make: built %d target(s)\n", built);
    return rc;
}
