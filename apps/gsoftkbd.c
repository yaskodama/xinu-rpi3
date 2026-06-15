// apps/gsoftkbd.c — on-screen QWERTY keyboard, render only (ported from
// xinu-rpi5 device/video/softkbd.c for the Pi 3 / arm-rpi3 platform).
//
// Five-row layout with key widths picked so each row fits cleanly
// inside the window's content area.  No interactivity yet — just the
// visual keyboard on the desktop.

#ifdef _XINU_PLATFORM_ARM_RPI3_

#include <gsoftkbd.h>
#include <gvideo.h>
#include <string.h>

window_t softkbd_win;

/* Modifier state.  Shift is one-shot (clears after the next character key);
 * Caps is a lock.  Both are toggled by clicking their on-screen keys. */
static int kbd_shift = 0;
static int kbd_caps  = 0;

/* One row = sequence of (label, width_in_units).  Width 0 means
 * use the row's default 1-unit cell.  A NULL label terminates. */
typedef struct {
    const char *label;
    int         w;   /* in base units; 0 == 1u */
} key_t;

static const key_t row0[] = {
    {"`",  0}, {"1", 0}, {"2", 0}, {"3", 0}, {"4", 0},
    {"5",  0}, {"6", 0}, {"7", 0}, {"8", 0}, {"9", 0},
    {"0",  0}, {"-", 0}, {"=", 0}, {"Bksp", 2}, {0,0}
};
static const key_t row1[] = {
    {"Tab", 2}, {"q", 0}, {"w", 0}, {"e", 0}, {"r", 0}, {"t", 0},
    {"y",   0}, {"u", 0}, {"i", 0}, {"o", 0}, {"p", 0},
    {"[",   0}, {"]", 0}, {0,0}
};
static const key_t row2[] = {
    {"Caps", 2}, {"a", 0}, {"s", 0}, {"d", 0}, {"f", 0}, {"g", 0},
    {"h",    0}, {"j", 0}, {"k", 0}, {"l", 0}, {";", 0}, {"'", 0},
    {"Ret",  2}, {0,0}
};
static const key_t row3[] = {
    {"Shift", 2}, {"z", 0}, {"x", 0}, {"c", 0}, {"v", 0}, {"b", 0},
    {"n",     0}, {"m", 0}, {",", 0}, {".", 0}, {"/", 0},
    {"Shift", 2}, {0,0}
};
static const key_t row4[] = {
    {"Ctrl", 2}, {"Alt", 2}, {"Space", 9}, {"Alt", 2}, {"Ctrl", 2}, {0,0}
};

static const key_t *rows[5] = { row0, row1, row2, row3, row4 };

static int row_unit_count(const key_t *row)
{
    int n = 0;
    for (const key_t *k = row; k->label; k++) n += k->w ? k->w : 1;
    return n;
}

static void draw_key(int x, int y, int w, int h, const char *lbl,
                     unsigned int face, unsigned int border, unsigned int fg)
{
    /* face + border rectangle */
    fill_rect(x, y, w, h, face);
    draw_rect(x, y, w, h, border);

    /* centred glyph label.  draw_string_at is 8 px / char and
     * caller is responsible for staying inside the cell. */
    int len = 0;
    while (lbl[len]) len++;
    int gw = len * FONT_WIDTH;
    int gx = x + (w - gw) / 2;
    int gy = y + (h - FONT_HEIGHT) / 2;
    if (gx < x + 2) gx = x + 2;
    draw_string_at(gx, gy, lbl, fg, face);
}

void softkbd_draw(window_t *self, unsigned int frame)
{
    extern int g_force_redraw;     /* apps/gwm.c: set only on a full repaint */
    (void)frame;
    /* Only paint on a full repaint.  Drawing all the keys every frame
     * (the incremental content pass calls draw_content for every window)
     * made the keyboard bleed on top of whatever window overlaps it. */
    if (!g_force_redraw) return;

    /* Inner content rectangle. */
    int cx0 = self->x + 4;
    int cy0 = self->y + WM_TITLEBAR_H + 4;
    int cw  = self->width  - 8;
    int ch  = self->height - WM_TITLEBAR_H - 7;

    int row_count = 5;
    if (ch < row_count * 12) return;          /* too short to render */

    int row_h = ch / row_count;
    int row_pad = 2;
    int cell_h = row_h - row_pad;

    /* Pick the widest row in units to set the base cell width. */
    int max_units = 0;
    for (int r = 0; r < row_count; r++) {
        int n = row_unit_count(rows[r]);
        if (n > max_units) max_units = n;
    }
    if (max_units == 0) return;
    int unit_w = cw / max_units;
    if (unit_w < 8) unit_w = 8;

    /* Colours: dark face, lighter border, white text. */
    unsigned int face   = 0xFF202830U;
    unsigned int border = 0xFF608090U;
    unsigned int fg     = 0xFFE8F0F8U;

    for (int r = 0; r < row_count; r++) {
        int n_units = row_unit_count(rows[r]);
        int row_w = n_units * unit_w;
        int x = cx0 + (cw - row_w) / 2;
        int y = cy0 + r * row_h;
        for (const key_t *k = rows[r]; k->label; k++) {
            int kw = (k->w ? k->w : 1) * unit_w;
            int pad = 2;
            draw_key(x + pad, y, kw - 2 * pad, cell_h, k->label,
                     face, border, fg);
            x += kw;
        }
    }
}

/* US-QWERTY shifted symbols for the number/punctuation keys. */
static char shift_sym(char c)
{
    switch (c) {
        case '1': return '!'; case '2': return '@'; case '3': return '#';
        case '4': return '$'; case '5': return '%'; case '6': return '^';
        case '7': return '&'; case '8': return '*'; case '9': return '(';
        case '0': return ')'; case '-': return '_'; case '=': return '+';
        case '[': return '{'; case ']': return '}'; case ';': return ':';
        case '\'':return '"'; case ',': return '<'; case '.': return '>';
        case '/': return '?'; case '`': return '~';
        default:  return c;
    }
}

/* Resolve a key label to the character to feed the focused window, honouring
 * the current Shift/Caps state.  Returns 0 for a pure modifier key. */
static int key_char(const char *lbl)
{
    if (lbl[1] == 0) {                          /* single-glyph key */
        char c = lbl[0];
        if (c >= 'a' && c <= 'z')
            return (kbd_caps ^ kbd_shift) ? (c - 'a' + 'A') : c;
        return kbd_shift ? shift_sym(c) : c;
    }
    if (0 == strcmp(lbl, "Space")) return ' ';
    if (0 == strcmp(lbl, "Bksp"))  return 8;    /* backspace */
    if (0 == strcmp(lbl, "Ret"))   return '\n'; /* newline -> run line */
    if (0 == strcmp(lbl, "Tab"))   return '\t';
    return 0;                                   /* Caps/Shift/Ctrl/Alt */
}

/* Hit-test a click at desktop coordinates (dx,dy).  If it lands on a key,
 * set *out_char to the character to feed (0 for a modifier that was just
 * toggled) and return 1; otherwise return 0.  The layout math here mirrors
 * softkbd_draw() exactly so the clickable cells match what is painted. */
int softkbd_hit(int dx, int dy, int *out_char)
{
    window_t *self = &softkbd_win;
    int cx0 = self->x + 4;
    int cy0 = self->y + WM_TITLEBAR_H + 4;
    int cw  = self->width  - 8;
    int ch  = self->height - WM_TITLEBAR_H - 7;
    int row_count = 5;

    *out_char = 0;
    if (ch < row_count * 12) return 0;

    int row_h  = ch / row_count;
    int cell_h = row_h - 2;

    int max_units = 0;
    for (int r = 0; r < row_count; r++) {
        int n = row_unit_count(rows[r]);
        if (n > max_units) max_units = n;
    }
    if (max_units == 0) return 0;
    int unit_w = cw / max_units;
    if (unit_w < 8) unit_w = 8;

    for (int r = 0; r < row_count; r++) {
        int n_units = row_unit_count(rows[r]);
        int row_w = n_units * unit_w;
        int x = cx0 + (cw - row_w) / 2;
        int y = cy0 + r * row_h;
        if (dy < y || dy >= y + cell_h) continue;     /* not in this row band */
        for (const key_t *k = rows[r]; k->label; k++) {
            int kw  = (k->w ? k->w : 1) * unit_w;
            int pad = 2;
            if (dx >= x + pad && dx < x + kw - pad) {  /* hit this key cell */
                if (0 == strcmp(k->label, "Shift")) { kbd_shift = !kbd_shift; return 1; }
                if (0 == strcmp(k->label, "Caps"))  { kbd_caps  = !kbd_caps;  return 1; }
                if (0 == strcmp(k->label, "Ctrl") ||
                    0 == strcmp(k->label, "Alt"))   { return 1; }   /* no-op */
                *out_char = key_char(k->label);
                kbd_shift = 0;                         /* one-shot shift clears */
                return 1;
            }
            x += kw;
        }
        return 0;                                      /* in the band, between keys */
    }
    return 0;
}

#endif /* _XINU_PLATFORM_ARM_RPI3_ */
