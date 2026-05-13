/**
 * @file xsh_edit.c
 *
 * Tiny emacs-like editor.  Built-in shell command (runs in shell thread).
 *
 *   edit FILE              opens FILE; creates if missing
 *
 * Bindings (one-key, no meta/escape sequences except arrows):
 *   C-f / C-b / C-n / C-p   forward/back char, next/prev line
 *   C-a / C-e               beginning / end of line
 *   C-d                     delete char under cursor
 *   Backspace (0x7F or 0x08) delete char before cursor
 *   Enter                   insert newline
 *   Tab                     insert two spaces
 *   C-k                     kill to end of line (deletes; no kill ring)
 *   C-l                     redraw (in case of garble)
 *   C-x C-s                 save buffer
 *   C-x C-c                 save + quit
 *   C-g                     quit without saving (asks if modified)
 *   ESC [ A/B/C/D           up/down/right/left arrow
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <thread.h>
#include <tty.h>
#include <xfs.h>

#define ED_MAX_LINES   400
#define ED_MAX_LINE    256
#define ED_VIEW_ROWS   24
#define ED_VIEW_COLS   80

static char ed_buf[ED_MAX_LINES][ED_MAX_LINE];
static int  ed_len[ED_MAX_LINES];
static int  ed_nlines;
static int  ed_row, ed_col;
static int  ed_top;
static char ed_filename[128];
static int  ed_modified;
static char ed_status[80];

static void ed_clear_screen(void)    { printf("\033[2J\033[H"); }
static void ed_goto(int r, int c)    { printf("\033[%d;%dH", r + 1, c + 1); }
static void ed_clear_line(void)      { printf("\033[K"); }
static void ed_invert_on(void)       { printf("\033[7m"); }
static void ed_invert_off(void)      { printf("\033[0m"); }

static void ed_set_status(const char *s)
{
    int i, n = strlen(s);
    if (n > (int)sizeof(ed_status) - 1) n = sizeof(ed_status) - 1;
    for (i = 0; i < n; i++) ed_status[i] = s[i];
    ed_status[n] = 0;
}

/* ---------- file I/O ---------- */

static int ed_load(const char *path)
{
    int  fd;
    char b[1024];
    int  n, i, j;
    int  cur_line = 0, cur_col = 0;

    ed_nlines = 0;
    ed_modified = 0;

    fd = xfsOpen(path, XFS_O_RDONLY);
    if (fd < 0)
    {
        ed_buf[0][0] = 0; ed_len[0] = 0; ed_nlines = 1;
        ed_set_status("(new file)");
        return OK;
    }
    ed_buf[0][0] = 0; ed_len[0] = 0; ed_nlines = 1;
    while ((n = xfsRead(fd, b, sizeof(b))) > 0)
    {
        for (i = 0; i < n; i++)
        {
            char c = b[i];
            if (c == '\n')
            {
                ed_len[cur_line] = cur_col;
                cur_line++;
                if (cur_line >= ED_MAX_LINES) { cur_line--; break; }
                cur_col = 0;
                ed_buf[cur_line][0] = 0;
                ed_len[cur_line] = 0;
            }
            else
            {
                if (cur_col < ED_MAX_LINE - 1)
                    ed_buf[cur_line][cur_col++] = c;
            }
        }
    }
    ed_len[cur_line] = cur_col;
    ed_nlines = cur_line + 1;
    xfsClose(fd);

    /* Trim trailing empty line that always shows up after a final '\n' */
    if (ed_nlines > 1 && ed_len[ed_nlines - 1] == 0) ed_nlines--;

    sprintf(ed_status, "loaded %d lines", ed_nlines);
    for (j = 0; j < ED_MAX_LINES && j >= ed_nlines; j++) ed_len[j] = 0;
    return OK;
}

static int ed_save(const char *path)
{
    int fd, i;
    fd = xfsOpen(path, XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
    if (fd < 0) { ed_set_status("save: cannot open"); return SYSERR; }
    for (i = 0; i < ed_nlines; i++)
    {
        if (ed_len[i] > 0) xfsWrite(fd, ed_buf[i], ed_len[i]);
        xfsWrite(fd, "\n", 1);
    }
    xfsClose(fd);
    ed_modified = 0;
    sprintf(ed_status, "wrote %d lines to %s", ed_nlines, path);
    return OK;
}

/* ---------- redraw ---------- */

static void ed_redraw(void)
{
    int r, j;

    ed_clear_screen();
    /* Title bar */
    ed_invert_on();
    ed_goto(0, 0);
    {
        char title[80];
        sprintf(title, " edit %s%s ",
                ed_filename, ed_modified ? " *" : "");
        printf("%s", title);
        for (j = strlen(title); j < ED_VIEW_COLS; j++) putchar(' ');
    }
    ed_invert_off();

    /* Body */
    for (r = 0; r < ED_VIEW_ROWS; r++)
    {
        int line_idx = ed_top + r;
        ed_goto(r + 1, 0);
        if (line_idx < ed_nlines)
        {
            int n = ed_len[line_idx];
            if (n > ED_VIEW_COLS) n = ED_VIEW_COLS;
            for (j = 0; j < n; j++)
            {
                char c = ed_buf[line_idx][j];
                if (c == '\t') putchar(' ');
                else if (c >= 32 && c < 127) putchar(c);
                else putchar('?');
            }
        }
        else
        {
            putchar('~');
        }
    }

    /* Status bar */
    ed_invert_on();
    ed_goto(ED_VIEW_ROWS + 1, 0);
    {
        char info[80];
        sprintf(info, " L%d:C%d  %s ",
                ed_row + 1, ed_col + 1, ed_status[0] ? ed_status : "");
        printf("%s", info);
        for (j = strlen(info); j < ED_VIEW_COLS; j++) putchar(' ');
    }
    ed_invert_off();

    /* Place cursor */
    ed_goto((ed_row - ed_top) + 1, ed_col);
}

/* ---------- editing primitives ---------- */

static void ed_scroll(void)
{
    if (ed_row < ed_top) ed_top = ed_row;
    else if (ed_row >= ed_top + ED_VIEW_ROWS) ed_top = ed_row - ED_VIEW_ROWS + 1;
    if (ed_top < 0) ed_top = 0;
}

static void ed_clamp_col(void)
{
    if (ed_col > ed_len[ed_row]) ed_col = ed_len[ed_row];
    if (ed_col < 0) ed_col = 0;
}

static void ed_insert_char(char c)
{
    int i, n;
    if (ed_row >= ED_MAX_LINES) return;
    n = ed_len[ed_row];
    if (n + 1 >= ED_MAX_LINE) return;
    for (i = n; i > ed_col; i--)
        ed_buf[ed_row][i] = ed_buf[ed_row][i - 1];
    ed_buf[ed_row][ed_col] = c;
    ed_len[ed_row] = n + 1;
    ed_col++;
    ed_modified = 1;
}

static void ed_delete_back(void)
{
    int i;
    if (ed_col > 0)
    {
        for (i = ed_col - 1; i < ed_len[ed_row] - 1; i++)
            ed_buf[ed_row][i] = ed_buf[ed_row][i + 1];
        ed_len[ed_row]--;
        ed_col--;
        ed_modified = 1;
        return;
    }
    /* At col 0 of a line — merge with previous line */
    if (ed_row == 0) return;
    {
        int prev = ed_row - 1;
        int sp   = ed_len[prev];
        int nlen = sp + ed_len[ed_row];
        if (nlen >= ED_MAX_LINE) return;
        for (i = 0; i < ed_len[ed_row]; i++)
            ed_buf[prev][sp + i] = ed_buf[ed_row][i];
        ed_len[prev] = nlen;
        for (i = ed_row; i < ed_nlines - 1; i++)
        {
            int k;
            int srclen = ed_len[i + 1];
            for (k = 0; k < srclen; k++) ed_buf[i][k] = ed_buf[i + 1][k];
            ed_len[i] = srclen;
        }
        ed_nlines--;
        ed_row = prev;
        ed_col = sp;
        ed_modified = 1;
    }
}

static void ed_delete_fwd(void)
{
    int i;
    if (ed_col < ed_len[ed_row])
    {
        for (i = ed_col; i < ed_len[ed_row] - 1; i++)
            ed_buf[ed_row][i] = ed_buf[ed_row][i + 1];
        ed_len[ed_row]--;
        ed_modified = 1;
        return;
    }
    if (ed_row + 1 >= ed_nlines) return;
    {
        int nlen = ed_len[ed_row] + ed_len[ed_row + 1];
        if (nlen >= ED_MAX_LINE) return;
        for (i = 0; i < ed_len[ed_row + 1]; i++)
            ed_buf[ed_row][ed_len[ed_row] + i] = ed_buf[ed_row + 1][i];
        ed_len[ed_row] = nlen;
        for (i = ed_row + 1; i < ed_nlines - 1; i++)
        {
            int k, srclen = ed_len[i + 1];
            for (k = 0; k < srclen; k++) ed_buf[i][k] = ed_buf[i + 1][k];
            ed_len[i] = srclen;
        }
        ed_nlines--;
        ed_modified = 1;
    }
}

static void ed_kill_eol(void)
{
    if (ed_col == ed_len[ed_row])
    { ed_delete_fwd(); return; }
    ed_len[ed_row] = ed_col;
    ed_modified = 1;
}

static void ed_split_line(void)
{
    int i, src;
    if (ed_nlines >= ED_MAX_LINES) return;
    /* Shift lines down. */
    for (i = ed_nlines; i > ed_row + 1; i--)
    {
        int k, srclen = ed_len[i - 1];
        for (k = 0; k < srclen; k++) ed_buf[i][k] = ed_buf[i - 1][k];
        ed_len[i] = srclen;
    }
    /* New line gets the tail of current line. */
    src = ed_len[ed_row] - ed_col;
    for (i = 0; i < src; i++)
        ed_buf[ed_row + 1][i] = ed_buf[ed_row][ed_col + i];
    ed_len[ed_row + 1] = src;
    ed_len[ed_row]     = ed_col;
    ed_nlines++;
    ed_row++;
    ed_col = 0;
    ed_modified = 1;
}

/* ---------- main loop ---------- */

shellcmd xsh_edit(int nargs, char *args[])
{
    int c, prev = 0;
    int orig_iflags;
    int running = 1;

    if (nargs < 2)
    {
        fprintf(stderr, "Usage: edit FILE\n");
        return 1;
    }
    strlcpy(ed_filename, args[1], sizeof(ed_filename));
    if (OK != ed_load(ed_filename))
    {
        fprintf(stderr, "edit: cannot load %s\n", ed_filename);
        return 1;
    }
    ed_row = 0;
    ed_col = 0;
    ed_top = 0;

    /* Switch TTY to raw mode (no echo, no canonical processing). */
    orig_iflags = TTY_ECHO;          /* shell sets ECHO; we restore on exit */
    control(stdin, TTY_CTRL_SET_IFLAG, TTY_IRAW, NULL);
    control(stdin, TTY_CTRL_CLR_IFLAG, TTY_ECHO, NULL);

    ed_redraw();
    while (running)
    {
        /* Read one byte. */
        char ch;
        int r = read(stdin, &ch, 1);
        if (r != 1) break;
        c = (unsigned char)ch;

        if (prev == 0x18)            /* C-x prefix */
        {
            prev = 0;
            if (c == 0x13) { ed_save(ed_filename); }              /* C-s */
            else if (c == 0x03) { ed_save(ed_filename); running = 0; } /* C-c */
            ed_redraw();
            continue;
        }
        if (prev == 0x1B && c == '[')        /* ESC [ */
        {
            prev = '[';
            continue;
        }
        if (prev == '[')
        {
            prev = 0;
            switch (c)
            {
            case 'A': if (ed_row > 0) ed_row--;             break; /* up    */
            case 'B': if (ed_row + 1 < ed_nlines) ed_row++; break; /* down  */
            case 'C': ed_col++;                             break; /* right */
            case 'D': if (ed_col > 0) ed_col--;             break; /* left  */
            default:  break;
            }
            ed_clamp_col(); ed_scroll(); ed_redraw();
            continue;
        }

        switch (c)
        {
        case 0x18: prev = 0x18; continue;                /* C-x */
        case 0x1B: prev = 0x1B; continue;                /* ESC */
        case 0x06: ed_col++; ed_clamp_col(); break;      /* C-f */
        case 0x02: if (ed_col > 0) ed_col--;            break; /* C-b */
        case 0x0E: if (ed_row + 1 < ed_nlines) ed_row++; ed_clamp_col(); break; /* C-n */
        case 0x10: if (ed_row > 0) ed_row--; ed_clamp_col();              break; /* C-p */
        case 0x01: ed_col = 0;                          break; /* C-a */
        case 0x05: ed_col = ed_len[ed_row];             break; /* C-e */
        case 0x04: ed_delete_fwd();                     break; /* C-d */
        case 0x0B: ed_kill_eol();                       break; /* C-k */
        case 0x0C: /* C-l: just redraw */               break;
        case 0x07: ed_set_status("quit (no save)"); running = 0; break; /* C-g */
        case 0x7F:
        case 0x08: ed_delete_back();                    break;
        case '\r':
        case '\n': ed_split_line();                     break;
        case '\t': ed_insert_char(' '); ed_insert_char(' '); break;
        default:
            if (c >= 32 && c < 127) ed_insert_char((char)c);
            break;
        }
        ed_scroll();
        ed_redraw();
    }

    /* Restore TTY: cooked mode + echo */
    control(stdin, TTY_CTRL_CLR_IFLAG, TTY_IRAW, NULL);
    control(stdin, TTY_CTRL_SET_IFLAG, TTY_ECHO, NULL);
    ed_clear_screen();
    printf("edit: %s\n", ed_modified ? "buffer modified (not saved)" : "ok");
    (void)orig_iflags;
    return 0;
}
