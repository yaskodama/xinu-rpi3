/**
 * @file shell_readline.h
 *
 * Line editor with history for the Xinu shell.
 *
 * Replaces the previous cooked-mode read() inside shell.c.  Provides:
 *   - left / right cursor movement within the current line
 *     (Ctrl-B / Ctrl-F + ESC[D / ESC[C)
 *   - jump to start / end of line (Ctrl-A / Ctrl-E)
 *   - backspace + Ctrl-D delete-under-cursor
 *   - Ctrl-K / Ctrl-U kill-to-end / kill-to-start
 *   - history navigation: Ctrl-P / Ctrl-N + ESC[A / ESC[B
 *   - per-shell circular history of up to SHELL_HIST_MAX entries
 *
 * Output uses ANSI escapes (\r, ESC[K, ESC[<n>D) — the QEMU console
 * and any modern terminal interprets these.
 */
#ifndef _SHELL_READLINE_H_
#define _SHELL_READLINE_H_

#include <shell.h>

#define SHELL_HIST_MAX     32                  /* circular slots */
#define SHELL_HIST_LINELEN (SHELL_BUFLEN)      /* per-slot bytes */

struct shell_history {
    char buf[SHELL_HIST_MAX][SHELL_HIST_LINELEN];
    int  count;     /* total entries ever added */
};

void shell_history_init(struct shell_history *h);
void shell_history_add (struct shell_history *h, const char *line);

/* Helpers used by both the console shell readline and the WM
 * mini-shell editor. */
int         shell_history_size(struct shell_history *h);
const char *shell_history_at  (struct shell_history *h, int from_newest);

/* Read one line from `fd`.  Returns the byte count written to `buf`
 * (including the trailing '\n') on success, EOF on EOF/error.  The
 * caller sees the same shape as the previous cooked read(). */
int  shell_readline    (int fd, char *buf, int max,
                        struct shell_history *h,
                        const char *prompt);

#endif /* _SHELL_READLINE_H_ */
