/**
 * @file wmcon.c
 *
 * WM-Console pseudo-device.  See include/wmcon.h.
 *
 * Implements:
 *   - a cell grid (WMCON_ROWS × WMCON_COLS) updated by a VT100-lite
 *     parser so the shell's readline + edit ANSI escapes display
 *     correctly inside the WM
 *   - a small input ring fed by the WM PS/2 keyboard handler
 *   - the standard Xinu device interface (Init/Open/Close/Read/
 *     Write/Getc/Putc/Control) so an ordinary `shell` thread can
 *     drive it via stdin/stdout/stderr.
 */

#include <stddef.h>
#include <kernel.h>
#include <device.h>
#include <semaphore.h>
#include <string.h>
#include <wmcon.h>

#define WMCON_IN_LEN    256

static char     wmcon_cells[WMCON_ROWS][WMCON_COLS];
static int      wmcon_cur_r, wmcon_cur_c;

/* VT100-lite parser state. */
static int      vt_st;             /* 0 = normal, 1 = saw ESC, 2 = saw ESC[ */
static int      vt_n1, vt_has_n1;  /* first CSI param */
static int      vt_n2, vt_has_n2;  /* second CSI param (after ;) */
static int      vt_in_n2;          /* parsing second param */

static char     wmcon_in[WMCON_IN_LEN];
static int      wmcon_in_head, wmcon_in_tail;
static semaphore wmcon_in_items;
static semaphore wmcon_in_mu;
static semaphore wmcon_state_mu;

/* ---------- cell-grid mutation ---------- */

static void scroll_up(void)
{
    int r, c;
    for (r = 0; r < WMCON_ROWS - 1; r++)
        for (c = 0; c < WMCON_COLS; c++)
            wmcon_cells[r][c] = wmcon_cells[r + 1][c];
    for (c = 0; c < WMCON_COLS; c++)
        wmcon_cells[WMCON_ROWS - 1][c] = ' ';
}

static void newline(void)
{
    wmcon_cur_c = 0;
    wmcon_cur_r++;
    if (wmcon_cur_r >= WMCON_ROWS) {
        scroll_up();
        wmcon_cur_r = WMCON_ROWS - 1;
    }
}

static void put_glyph(int c)
{
    if (wmcon_cur_c >= WMCON_COLS) newline();
    wmcon_cells[wmcon_cur_r][wmcon_cur_c++] = (char)c;
}

static void clear_screen(void)
{
    int r, c;
    for (r = 0; r < WMCON_ROWS; r++)
        for (c = 0; c < WMCON_COLS; c++)
            wmcon_cells[r][c] = ' ';
}

static void clear_to_eol(void)
{
    int c;
    for (c = wmcon_cur_c; c < WMCON_COLS; c++)
        wmcon_cells[wmcon_cur_r][c] = ' ';
}

static void vt_reset(void)
{
    vt_st = 0;
    vt_n1 = 0; vt_has_n1 = 0;
    vt_n2 = 0; vt_has_n2 = 0;
    vt_in_n2 = 0;
}

/* Apply a single incoming byte to the cell grid. */
static void apply_byte(int c)
{
    if (vt_st == 1) {
        if (c == '[') { vt_reset(); vt_st = 2; }
        else          vt_reset();
        return;
    }
    if (vt_st == 2) {
        if (c >= '0' && c <= '9') {
            if (vt_in_n2) {
                vt_n2 = vt_n2 * 10 + (c - '0'); vt_has_n2 = 1;
            } else {
                vt_n1 = vt_n1 * 10 + (c - '0'); vt_has_n1 = 1;
            }
            return;
        }
        if (c == ';') { vt_in_n2 = 1; return; }
        {
            int n  = vt_has_n1 ? vt_n1 : 1;
            switch (c) {
                case 'A': /* cursor up */
                    wmcon_cur_r = (wmcon_cur_r > n) ? wmcon_cur_r - n : 0;
                    break;
                case 'B': /* cursor down */
                    wmcon_cur_r += n;
                    if (wmcon_cur_r >= WMCON_ROWS) wmcon_cur_r = WMCON_ROWS - 1;
                    break;
                case 'C': /* cursor right */
                    wmcon_cur_c += n;
                    if (wmcon_cur_c >= WMCON_COLS) wmcon_cur_c = WMCON_COLS - 1;
                    break;
                case 'D': /* cursor left */
                    wmcon_cur_c = (wmcon_cur_c > n) ? wmcon_cur_c - n : 0;
                    break;
                case 'H': case 'f': {
                    int row = (vt_has_n1 ? vt_n1 : 1) - 1;
                    int col = (vt_has_n2 ? vt_n2 : 1) - 1;
                    if (row < 0) row = 0;
                    if (row >= WMCON_ROWS) row = WMCON_ROWS - 1;
                    if (col < 0) col = 0;
                    if (col >= WMCON_COLS) col = WMCON_COLS - 1;
                    wmcon_cur_r = row; wmcon_cur_c = col;
                    break;
                }
                case 'J':
                    if (vt_has_n1 && vt_n1 == 2) clear_screen();
                    /* other modes: ignore */
                    break;
                case 'K':
                    clear_to_eol();
                    break;
                case 'm':
                    /* color attributes — ignore */
                    break;
                default: break;
            }
        }
        vt_reset();
        return;
    }

    /* Normal mode */
    if (c == 0x1b) { vt_st = 1; return; }
    if (c == '\r') { wmcon_cur_c = 0; return; }
    if (c == '\n') { newline(); return; }
    if (c == '\b') { if (wmcon_cur_c > 0) wmcon_cur_c--; return; }
    if (c == 0x07) return;          /* BEL */
    if (c == '\t') {
        int n = 4 - (wmcon_cur_c & 3);
        while (n-- > 0) put_glyph(' ');
        return;
    }
    if (c >= 0x20 && c < 0x7f) put_glyph(c);
}

/* ---------- Xinu device interface ---------- */

devcall wmconInit(device *devptr)
{
    (void)devptr;
    clear_screen();
    wmcon_cur_r = 0; wmcon_cur_c = 0;
    vt_reset();
    wmcon_in_head = wmcon_in_tail = 0;
    wmcon_in_items = semcreate(0);
    wmcon_in_mu    = semcreate(1);
    wmcon_state_mu = semcreate(1);
    return OK;
}

devcall wmconOpen(device *devptr, int dev2)
{
    (void)devptr; (void)dev2;
    return OK;
}

devcall wmconClose(device *devptr)
{
    (void)devptr;
    return OK;
}

devcall wmconGetc(device *devptr)
{
    char c;
    (void)devptr;
    wait(wmcon_in_items);
    wait(wmcon_in_mu);
    c = wmcon_in[wmcon_in_tail];
    wmcon_in_tail = (wmcon_in_tail + 1) % WMCON_IN_LEN;
    signal(wmcon_in_mu);
    return (int)(unsigned char)c;
}

devcall wmconPutc(device *devptr, char c)
{
    (void)devptr;
    wait(wmcon_state_mu);
    apply_byte((unsigned char)c);
    signal(wmcon_state_mu);
    return (int)(unsigned char)c;
}

devcall wmconRead(device *devptr, void *buf, uint n)
{
    char *p = (char *)buf;
    uint i;
    (void)devptr;
    for (i = 0; i < n; i++) {
        int c;
        wait(wmcon_in_items);
        wait(wmcon_in_mu);
        c = (unsigned char)wmcon_in[wmcon_in_tail];
        wmcon_in_tail = (wmcon_in_tail + 1) % WMCON_IN_LEN;
        signal(wmcon_in_mu);
        p[i] = (char)c;
        if (c == '\n') { i++; break; }
    }
    return (devcall)i;
}

devcall wmconWrite(device *devptr, const void *buf, uint n)
{
    const char *p = (const char *)buf;
    uint i;
    (void)devptr;
    wait(wmcon_state_mu);
    for (i = 0; i < n; i++) apply_byte((unsigned char)p[i]);
    signal(wmcon_state_mu);
    return (devcall)n;
}

devcall wmconControl(device *devptr, int func, long arg1, long arg2)
{
    /* The wmcon is inherently raw + non-echoing, so the shell's
     * TTY_CTRL_SET/CLR_IFLAG toggles are a no-op here. */
    (void)devptr; (void)func; (void)arg1; (void)arg2;
    return OK;
}

/* ---------- bridge from apps/wm.c ---------- */

void wm_console_feed_key(int c)
{
    int next;
    wait(wmcon_in_mu);
    next = (wmcon_in_head + 1) % WMCON_IN_LEN;
    if (next != wmcon_in_tail) {
        wmcon_in[wmcon_in_head] = (char)c;
        wmcon_in_head = next;
        signal(wmcon_in_items);
    }
    signal(wmcon_in_mu);
}

void wm_console_get_state(int *rows, int *cols, const char **cells,
                          int *cur_r, int *cur_c)
{
    *rows  = WMCON_ROWS;
    *cols  = WMCON_COLS;
    *cells = (const char *)wmcon_cells;
    *cur_r = wmcon_cur_r;
    *cur_c = wmcon_cur_c;
}
