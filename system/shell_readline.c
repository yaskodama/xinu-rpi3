/**
 * @file shell_readline.c
 *
 * Line editor + history.  See include/shell_readline.h for the API.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <kernel.h>
#include <device.h>
#include <tty.h>
#include <shell_readline.h>

/* Xinu's libxc has memcpy but no memmove; provide an overlap-safe
 * shim for the in-place buffer shuffles below. */
static void rl_memmove(void *dst, const void *src, int n)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    int i;
    if (d == s || n <= 0) return;
    if (d < s) {
        for (i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (i = n - 1; i >= 0; i--) d[i] = s[i];
    }
}
#define memmove rl_memmove

/* ---------- helpers ---------- */

/* Write a raw byte string straight to the device. */
static void wstr(int fd, const char *s)
{
    int n = strlen(s);
    if (n > 0) write(fd, (void *)s, n);
}

static void wch(int fd, char c)
{
    write(fd, &c, 1);
}

/* Redraw the line: \r, prompt, current buffer, clear-to-EOL, then
 * back up the cursor to the desired column.  Using ANSI escapes:
 *   ESC [ K   — clear from cursor to end of line
 *   ESC [ n D — move cursor n chars left  */
static void redraw(int fd, const char *prompt, const char *buf,
                   int len, int pos)
{
    char esc[16];
    int back;

    wch(fd, '\r');
    wstr(fd, prompt);
    if (len > 0) write(fd, (void *)buf, len);
    wstr(fd, "\x1b[K");
    back = len - pos;
    if (back > 0) {
        /* small integer to decimal */
        int  n = back, i = 0;
        char tmp[8];
        if (n == 0) tmp[i++] = '0';
        else { while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; } }
        esc[0] = 0x1b; esc[1] = '[';
        int p = 2;
        while (i > 0) esc[p++] = tmp[--i];
        esc[p++] = 'D';
        esc[p]   = 0;
        wstr(fd, esc);
    }
}

/* ---------- history ---------- */

void shell_history_init(struct shell_history *h)
{
    int i;
    h->count = 0;
    for (i = 0; i < SHELL_HIST_MAX; i++) h->buf[i][0] = 0;
}

void shell_history_add(struct shell_history *h, const char *line)
{
    int idx;
    if (line == NULL || line[0] == 0 || line[0] == '\n') return;
    /* Skip if identical to the most recent entry. */
    if (h->count > 0) {
        int prev = (h->count - 1) % SHELL_HIST_MAX;
        if (0 == strcmp(h->buf[prev], line)) return;
    }
    idx = h->count % SHELL_HIST_MAX;
    strncpy(h->buf[idx], line, SHELL_HIST_LINELEN - 1);
    h->buf[idx][SHELL_HIST_LINELEN - 1] = 0;
    h->count++;
}

/* Number of currently retained history lines. */
int shell_history_size(struct shell_history *h)
{
    return (h->count < SHELL_HIST_MAX) ? h->count : SHELL_HIST_MAX;
}

/* Map an "index-from-newest" (0 = most recent) to a slot. */
const char *shell_history_at(struct shell_history *h, int from_newest)
{
    int slot;
    if (from_newest < 0 || from_newest >= shell_history_size(h)) return NULL;
    slot = (h->count - 1 - from_newest) % SHELL_HIST_MAX;
    if (slot < 0) slot += SHELL_HIST_MAX;
    return h->buf[slot];
}

/* Backwards-compat aliases for the local readline below. */
#define hist_size shell_history_size
#define hist_at   shell_history_at

/* ---------- readline ---------- */

int shell_readline(int fd, char *buf, int max, struct shell_history *h,
                   const char *prompt)
{
    int len      = 0;     /* characters in buf (excl. terminator) */
    int pos      = 0;     /* cursor position [0..len] */
    int hview    = -1;    /* -1 = editing fresh line; else from-newest */
    int esc_st   = 0;     /* 0 normal, 1 saw ESC, 2 saw ESC[ */
    char saved[SHELL_BUFLEN];   /* current line saved when entering history */
    int  has_saved = 0;

    if (max < 4) return SYSERR;

    /* Raw mode: no kernel echo, no cooked line buffering. */
    control(fd, TTY_CTRL_CLR_IFLAG, TTY_ECHO, NULL);
    control(fd, TTY_CTRL_SET_IFLAG, TTY_IRAW, NULL);

    /* Show prompt. */
    wch(fd, '\r');
    wstr(fd, prompt);

    buf[0] = 0;

    for (;;)
    {
        int c = getc(fd);
        if (c == EOF) {
            /* restore cooked + return EOF on bare EOF */
            control(fd, TTY_CTRL_CLR_IFLAG, TTY_IRAW, NULL);
            control(fd, TTY_CTRL_SET_IFLAG, TTY_ECHO, NULL);
            return EOF;
        }

        /* CSI parser: ESC '[' <final> */
        if (esc_st == 2) {
            esc_st = 0;
            switch (c) {
                case 'A': goto act_up;
                case 'B': goto act_down;
                case 'C': goto act_right;
                case 'D': goto act_left;
                case 'H': goto act_home;     /* some terminals */
                case 'F': goto act_end;
                default:  continue;          /* unknown CSI — drop */
            }
        }
        if (esc_st == 1) {
            if (c == '[') { esc_st = 2; continue; }
            esc_st = 0;
            continue;
        }
        if (c == 0x1b) { esc_st = 1; continue; }

        switch (c) {
            case '\r':
            case '\n':
                wch(fd, '\n');
                if (len + 1 < max) buf[len++] = '\n';
                buf[len] = 0;
                /* restore cooked tty for command execution */
                control(fd, TTY_CTRL_CLR_IFLAG, TTY_IRAW, NULL);
                control(fd, TTY_CTRL_SET_IFLAG, TTY_ECHO, NULL);
                return len;

            case 0x10: goto act_up;        /* Ctrl-P */
            case 0x0e: goto act_down;      /* Ctrl-N */
            case 0x02: goto act_left;      /* Ctrl-B */
            case 0x06: goto act_right;     /* Ctrl-F */
            case 0x01: goto act_home;      /* Ctrl-A */
            case 0x05: goto act_end;       /* Ctrl-E */

            case 0x7f:                      /* DEL */
            case 0x08:                      /* Backspace */
                if (pos > 0) {
                    memmove(buf + pos - 1, buf + pos, len - pos);
                    pos--; len--;
                    buf[len] = 0;
                    redraw(fd, prompt, buf, len, pos);
                }
                continue;

            case 0x04:                      /* Ctrl-D */
                if (len == 0) {
                    /* empty line: treat as EOF (clean shell exit) */
                    control(fd, TTY_CTRL_CLR_IFLAG, TTY_IRAW, NULL);
                    control(fd, TTY_CTRL_SET_IFLAG, TTY_ECHO, NULL);
                    return EOF;
                }
                if (pos < len) {
                    memmove(buf + pos, buf + pos + 1, len - pos - 1);
                    len--;
                    buf[len] = 0;
                    redraw(fd, prompt, buf, len, pos);
                }
                continue;

            case 0x0b:                      /* Ctrl-K — kill to end */
                if (pos < len) {
                    len = pos;
                    buf[len] = 0;
                    redraw(fd, prompt, buf, len, pos);
                }
                continue;

            case 0x15:                      /* Ctrl-U — kill to start */
                if (pos > 0) {
                    memmove(buf, buf + pos, len - pos);
                    len -= pos;
                    pos = 0;
                    buf[len] = 0;
                    redraw(fd, prompt, buf, len, pos);
                }
                continue;

            case 0x0c:                      /* Ctrl-L — clear screen */
                wstr(fd, "\x1b[2J\x1b[H");
                redraw(fd, prompt, buf, len, pos);
                continue;

            default:
                if (c >= 0x20 && c < 0x7f && len + 2 < max) {
                    memmove(buf + pos + 1, buf + pos, len - pos);
                    buf[pos] = (char)c;
                    pos++; len++;
                    buf[len] = 0;
                    if (pos == len) {
                        /* fast path: just emit the new char */
                        wch(fd, (char)c);
                    } else {
                        redraw(fd, prompt, buf, len, pos);
                    }
                }
                continue;
        }

    act_left:
        if (pos > 0) { pos--; wstr(fd, "\x1b[D"); }
        continue;
    act_right:
        if (pos < len) { pos++; wstr(fd, "\x1b[C"); }
        continue;
    act_home:
        while (pos > 0) { pos--; wstr(fd, "\x1b[D"); }
        continue;
    act_end:
        while (pos < len) { pos++; wstr(fd, "\x1b[C"); }
        continue;

    act_up:
        if (h != NULL && hview + 1 < hist_size(h)) {
            const char *src;
            if (hview == -1 && !has_saved) {
                memcpy(saved, buf, len);
                saved[len] = 0;
                has_saved = 1;
            }
            hview++;
            src = hist_at(h, hview);
            if (src != NULL) {
                int n = strlen(src);
                /* strip trailing newline copies */
                while (n > 0 && (src[n-1] == '\n' || src[n-1] == '\r')) n--;
                if (n >= max - 1) n = max - 2;
                memcpy(buf, src, n);
                buf[n] = 0;
                len = n; pos = n;
                redraw(fd, prompt, buf, len, pos);
            }
        }
        continue;
    act_down:
        if (h != NULL && hview > 0) {
            const char *src;
            hview--;
            src = hist_at(h, hview);
            if (src != NULL) {
                int n = strlen(src);
                while (n > 0 && (src[n-1] == '\n' || src[n-1] == '\r')) n--;
                if (n >= max - 1) n = max - 2;
                memcpy(buf, src, n);
                buf[n] = 0;
                len = n; pos = n;
                redraw(fd, prompt, buf, len, pos);
            }
        } else if (h != NULL && hview == 0) {
            /* Step below newest entry — restore the line we were editing. */
            hview = -1;
            if (has_saved) {
                int n = strlen(saved);
                if (n >= max - 1) n = max - 2;
                memcpy(buf, saved, n);
                buf[n] = 0;
                len = n; pos = n;
            } else {
                buf[0] = 0; len = 0; pos = 0;
            }
            redraw(fd, prompt, buf, len, pos);
        }
        continue;
    }
}
