// apps/gsoftkbd.c — on-screen QWERTY keyboard, render only (ported from
// xinu-rpi5 device/video/softkbd.c for the Pi 3 / arm-rpi3 platform).
//
// Five-row layout with key widths picked so each row fits cleanly
// inside the window's content area.  No interactivity yet — just the
// visual keyboard on the desktop.

#ifdef _XINU_PLATFORM_ARM_RPI3_

#include <gsoftkbd.h>
#include <gvideo.h>

window_t softkbd_win;

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
    (void)frame;

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

#endif /* _XINU_PLATFORM_ARM_RPI3_ */
