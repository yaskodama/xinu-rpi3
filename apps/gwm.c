// apps/gwm.c — window manager: chrome + redraw loop (ported from
// xinu-rpi5 device/video/wm.c for the Pi 3 / arm-rpi3 platform).
//
// Also hosts gwm_main(), the Xinu thread entry that brings up the
// framebuffer, lays out a couple of static windows + the soft
// keyboard, and enters the never-returning redraw loop.

#ifdef _XINU_PLATFORM_ARM_RPI3_

#include <thread.h>
#include <gwm.h>
#include <gvideo.h>
#include <gsoftkbd.h>

#define DESKTOP_BG     0xFF003366U   /* dark navy "desktop"          */
#define DEFAULT_FPS    20            /* 1 frame every 50 ms          */

static window_t *wm_head;
static void    (*wm_tick)(void);

/* Set during the cursor-erase clipped repaint in wm_run(): when true,
 * window content callbacks must repaint EVERYTHING (ignore their dirty
 * tracking) so the clipped patch under the cursor is fully restored.
 * When false (the normal per-frame content pass), incremental
 * callbacks repaint only what changed to avoid framebuffer flicker. */
int g_force_redraw = 0;

/* Cursor overlay state — repainted on top of all windows every
 * frame so it never disappears under another redraw.  Cursor
 * coordinates are in *screen* space, not virtual desktop. */
static int cursor_x = 320;
static int cursor_y = 240;
static int cursor_visible = 1;

/* Viewport state — top-left corner of the visible camera inside
 * the WM_DESKTOP_W × WM_DESKTOP_H virtual desktop. */
static int vp_x = 0;
static int vp_y = 0;
/* Autopan defaults to OFF — without USB input there is no way to
 * trigger it, and the static layout fits the panel. */
static int autopan_on = 0;

static void clamp_viewport(int sw, int sh)
{
    int max_x = WM_DESKTOP_W - sw;
    int max_y = WM_DESKTOP_H - sh;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (vp_x < 0) vp_x = 0;
    if (vp_y < 0) vp_y = 0;
    if (vp_x > max_x) vp_x = max_x;
    if (vp_y > max_y) vp_y = max_y;
}

void wm_pan(int dx, int dy)
{
    vp_x += dx;
    vp_y += dy;
    /* Clamp on next frame using the real screen size. */
}

void wm_set_viewport(int x, int y) { vp_x = x; vp_y = y; }
int  wm_view_x(void) { return vp_x; }
int  wm_view_y(void) { return vp_y; }

void wm_set_autopan(int on) { autopan_on = on ? 1 : 0; }

void wm_set_tick(void (*fn)(void))
{
    wm_tick = fn;
}

void wm_cursor_set(int x, int y, int visible)
{
    cursor_x = x;
    cursor_y = y;
    cursor_visible = visible;
}

/* 12×12 arrow cursor sprite.  '#' = white, '.' = black border,
 * ' ' = transparent.  Anchor (hot-spot) is top-left. */
static const char cursor_sprite[12][12] = {
    {'#','.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','.',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','#','.',' ',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','#','#','.',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','#','#','#','.',' ',' ',' ',' ',' ',' '},
    {'#','#','#','#','#','#','.',' ',' ',' ',' ',' '},
    {'#','#','#','#','#','#','#','.',' ',' ',' ',' '},
    {'#','#','#','#','#','#','#','#','.',' ',' ',' '},
    {'#','#','#','#','#','.','.','.','.',' ',' ',' '},
    {'#','#','.','#','#','.',' ',' ',' ',' ',' ',' '},
    {'#','.',' ','.','#','#','.',' ',' ',' ',' ',' '},
    {'.',' ',' ',' ','.','#','#','.',' ',' ',' ',' '},
};

static void draw_cursor(void)
{
    if (!cursor_visible) return;
    /* Cursor is in screen coords — reset the viewport so the
     * sprite always renders 1:1 onto the physical display, then
     * restore it for the next frame's window draws. */
    int save_x = video_viewport_x();
    int save_y = video_viewport_y();
    video_set_viewport(0, 0);

    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();
    for (int dy = 0; dy < 12; dy++) {
        int py = cursor_y + dy;
        if (py < 0 || py >= sh) continue;
        for (int dx = 0; dx < 12; dx++) {
            int px = cursor_x + dx;
            if (px < 0 || px >= sw) continue;
            char c = cursor_sprite[dy][dx];
            if (c == '#')      fill_rect(px, py, 1, 1, 0xFFFFFFFFU);
            else if (c == '.') fill_rect(px, py, 1, 1, 0xFF000000U);
        }
    }

    video_set_viewport(save_x, save_y);
}

void wm_add(window_t *w)
{
    w->next = 0;
    if (wm_head == 0) {
        wm_head = w;
        return;
    }
    window_t *t = wm_head;
    while (t->next) t = t->next;
    t->next = w;
}

static void draw_chrome(window_t *w)
{
    /* outer border */
    draw_rect(w->x, w->y, w->width, w->height, w->chrome_color);

    /* title bar background (one pixel inside the border) */
    fill_rect(w->x + 1, w->y + 1, w->width - 2, WM_TITLEBAR_H, w->title_bg);

    /* title text — left-aligned with a 4 px gutter */
    draw_string_at(w->x + 4, w->y + 2, w->title, w->title_fg, w->title_bg);

    /* separator under the title */
    fill_rect(w->x + 1, w->y + WM_TITLEBAR_H + 1,
              w->width - 2, 1, w->chrome_color);

    /* content background */
    int cy = w->y + WM_TITLEBAR_H + 2;
    int ch = w->height - WM_TITLEBAR_H - 3;
    fill_rect(w->x + 1, cy, w->width - 2, ch, w->content_bg);
}

/* Repaint the desktop + every window.  Honours the active clip rectangle,
 * so wm_run() can call it with a tiny clip to refresh just the patch under
 * the cursor without a full-screen (flickery) redraw. */
/* On-screen WiFi indicator, bottom-right corner of the desktop.  White
 * fan = not connected; green = connected (DHCP lease held).  When
 * connected the joined AP's SSID is printed under the mark.  Drawn every
 * frame in wm_run() in the free band below the windows.  Display only —
 * AP selection is driven from the Mac /pi3 page. */
static void draw_wifi_indicator(void)
{
    extern int wifi_connected(void);
    extern const char *wifi_ssid(void);
    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();
    int conn = wifi_connected();
    unsigned int col = conn ? 0xFF36D35AU : 0xFFFFFFFFU;   /* green / white */
    int cx = sw - 24;          /* fan centre x */
    int by = sh - 32;          /* mark dot baseline (room for SSID below) */

    /* Dark backing box covering the mark + SSID label line. */
    fill_rect(sw - 152, by - 22, 150, 50, 0xFF182028U);
    /* Three arcs (approximated by centred bars) + a dot, fanning upward. */
    fill_rect(cx - 13, by - 18, 26, 2, col);   /* outer */
    fill_rect(cx - 9,  by - 13, 18, 2, col);   /* middle */
    fill_rect(cx - 5,  by - 8,  10, 2, col);   /* inner */
    fill_rect(cx - 2,  by - 3,   4, 4, col);   /* dot */
    /* SSID under the mark when connected, else a hint. */
    if (conn && wifi_ssid()[0]) {
        draw_string_at(sw - 148, by + 8, wifi_ssid(), 0xFFA0E0FFU, 0xFF182028U);
    } else {
        draw_string_at(sw - 110, by + 8, "not connected", 0xFF888888U, 0xFF182028U);
    }
}

/* ===================================================================
 * Graphics window: a rotating wire-frame 3-D wine glass.  The `wine`
 * xsh command calls gwm_start_wine() which spins it 30 full turns.
 * =================================================================== */
extern double sin(int);                 /* trig.c — int degrees -> double */
extern void   draw_line(int, int, int, int, unsigned int);

/* Integer sin/cos (×1024) via a 0..90° table, built once from sin().
 * Avoids 100s of slow soft-float Taylor calls per frame, which would
 * otherwise starve the rest of the system during the animation. */
static int  g_sintab[91];
static int  g_sininit = 0;
static int isin(int d)
{
    d %= 360; if (d < 0) d += 360;
    if (d <= 90)  return  g_sintab[d];
    if (d <= 180) return  g_sintab[180 - d];
    if (d <= 270) return -g_sintab[d - 180];
    return -g_sintab[360 - d];
}
static int icos(int d) { return isin(d + 90); }

#define WG_NP 8                          /* profile points (bottom..rim)   */
#define WG_M  10                         /* longitude divisions            */
/* radius / height ×100 (surface-of-revolution wine-glass profile). */
static const int wgr[WG_NP] = {42,42, 5, 5,11,34,36,30};
static const int wgh[WG_NP] = { 0, 4, 4,46,50,62,80,94};

static window_t graphics_win;
int        g_wine_active = 0;            /* read by xsh `wine` via extern  */
static int g_wine_ax = 0, g_wine_ay = 0, g_wine_az = 0;  /* X/Y/Z angles   */
static int g_wine_rot    = 0;            /* completed Y-axis turns          */

void gwm_start_wine(void)
{
    g_wine_active = 1;
    g_wine_ax = g_wine_ay = g_wine_az = 0;
    g_wine_rot = 0;
}

static void graphics_draw(window_t *self, unsigned int frame)
{
    static int drawn_idle = 0;
    /* Idle and already painted (and not the clipped cursor repaint): do
     * nothing.  Normally the glass spins continuously (auto-started at
     * boot), so this only kicks in if the animation is ever stopped. */
    if (!g_wine_active && drawn_idle && !g_force_redraw) return;
    /* Throttle the spin to ~10 fps (every 2nd frame) to keep framebuffer
     * load low while staying smooth — the cursor repaint still runs. */
    if (g_wine_active && (frame & 1) && !g_force_redraw) return;

    if (!g_sininit) {
        for (int k = 0; k <= 90; k++) g_sintab[k] = (int)(sin(k) * 1024.0 + 0.5);
        g_sininit = 1;
    }

    int x0 = self->x + 1, y0 = self->y + WM_TITLEBAR_H + 1;
    int w  = self->width - 2, h = self->height - WM_TITLEBAR_H - 3;
    int cx = x0 + w / 2, cy = y0 + h / 2;
    int sc = (h < w ? h : w) * 42 / 100;            /* projection scale */
    unsigned int col = g_wine_active ? 0xFF80E0FFU : 0xFF5090C0U;

    /* Combined rotation about all three axes (Rx . Ry . Rz). */
    int cAx = icos(g_wine_ax), sAx = isin(g_wine_ax);
    int cAy = icos(g_wine_ay), sAy = isin(g_wine_ay);
    int cAz = icos(g_wine_az), sAz = isin(g_wine_az);

    /* Clear only the glass bounding box (not the whole window) — the
     * full-window wipe on the uncached framebuffer was the bottleneck.
     * Box is square-ish since the glass now tumbles on every axis. */
    fill_rect(cx - 96, cy - 96, 192, 192, 0xFF050810U);

    /* Per-frame scaled radius/height (pixels). */
    int rsc[WG_NP], hsc[WG_NP], i, j;
    for (i = 0; i < WG_NP; i++) {
        rsc[i] = wgr[i] * sc / 100;
        hsc[i] = (wgh[i] - 50) * sc / 100;
    }
    static int px[WG_NP][WG_M], py[WG_NP][WG_M];
    for (i = 0; i < WG_NP; i++) {
        for (j = 0; j < WG_M; j++) {
            int lon = j * (360 / WG_M);
            int x = rsc[i] * icos(lon) / 1024;       /* glass-local point  */
            int z = rsc[i] * isin(lon) / 1024;
            int y = hsc[i];
            int n1, n2;
            n1 = (y * cAx - z * sAx) / 1024;          /* Rx */
            n2 = (y * sAx + z * cAx) / 1024;  y = n1; z = n2;
            n1 = (x * cAy + z * sAy) / 1024;          /* Ry */
            n2 = (-x * sAy + z * cAy) / 1024; x = n1; z = n2;
            n1 = (x * cAz - y * sAz) / 1024;          /* Rz */
            n2 = (x * sAz + y * cAz) / 1024;  x = n1; y = n2;
            px[i][j] = cx + x;
            py[i][j] = cy - y;
        }
    }
    for (j = 0; j < WG_M; j++)                       /* longitude curves */
        for (i = 0; i < WG_NP - 1; i++)
            draw_line(px[i][j], py[i][j], px[i+1][j], py[i+1][j], col);
    for (i = 0; i < WG_NP; i++) {                    /* latitude rings */
        if (wgr[i] == 0) continue;
        for (j = 0; j < WG_M; j++) {
            int j2 = (j + 1) % WG_M;
            draw_line(px[i][j], py[i][j], px[i][j2], py[i][j2], col);
        }
    }
    /* Title hint / progress (own strip — outside the glass bbox clear). */
    fill_rect(x0 + 2, y0 + 1, w - 4, 13, 0xFF050810U);
    if (g_wine_active) {
        /* "spin N/30" — stops after 30 full Y turns so the WiFi responder
         * thread gets its CPU back (a never-ending spin starved it). */
        char b[24]; int n = 0, r = g_wine_rot;
        const char *p = "spin "; while (*p) b[n++] = *p++;
        if (r >= 10) b[n++] = '0' + r / 10; b[n++] = '0' + r % 10;
        b[n++] = '/'; b[n++] = '3'; b[n++] = '0'; b[n] = 0;
        draw_string_at(x0 + 4, y0 + 2, b, 0xFFFFFF80U, 0xFF050810U);
        /* Advance each axis a little, at different rates -> tumbling. */
        g_wine_ax += 6;  if (g_wine_ax >= 360) g_wine_ax -= 360;
        g_wine_az += 10; if (g_wine_az >= 360) g_wine_az -= 360;
        g_wine_ay += 16; if (g_wine_ay >= 360) {
            g_wine_ay -= 360;
            if (++g_wine_rot >= 30) g_wine_active = 0;   /* done -> idle */
        }
    } else {
        draw_string_at(x0 + 4, y0 + 2, "run 'wine' in xsh", 0xFF607080U, 0xFF050810U);
    }
    drawn_idle = !g_wine_active;
}

static void paint_scene(unsigned int frame)
{
    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();

    clamp_viewport(sw, sh);

    /* Desktop background (screen space). */
    video_set_viewport(0, 0);
    fill_rect(0, 0, sw, sh, DESKTOP_BG);

    /* Windows, in virtual-desktop coords via the viewport offset. */
    video_set_viewport(vp_x, vp_y);
    for (window_t *w = wm_head; w; w = w->next) {
        draw_chrome(w);
        if (w->draw_content) w->draw_content(w, frame);
    }
}

/* ===================================================================
 * Window-console shell text ring + flicker-free rendering.
 *
 * The windowed xsh shell's stdout/stderr land here via the GWINCON0
 * device -> gwin_shell_record().  The ring is rendered by sh_draw()
 * which repaints ONLY dirty rows so it never wipes the whole content
 * area (a full repaint of the uncached framebuffer flickers badly).
 * ===================================================================
 */

#define SHW_ROWS 56
#define SHW_COLS 46

static char          sh_ring[SHW_ROWS][SHW_COLS + 1];
static unsigned char sh_row_dirty[SHW_ROWS];
static int           sh_cur_row;
static int           sh_cur_col;
static int           sh_filled;   /* have we wrapped past the bottom? */
static int           sh_esc;       /* CSI parser: 0 normal, 1 ESC, 2 ESC[ */
static int           sh_cr;        /* pending carriage return (\r seen)   */

static window_t sh_win;

static void sh_newline(void)
{
    sh_ring[sh_cur_row][sh_cur_col] = 0;
    sh_cur_row = (sh_cur_row + 1) % SHW_ROWS;
    sh_cur_col = 0;
    sh_ring[sh_cur_row][0] = 0;
    if (sh_cur_row == 0) sh_filled = 1;
    /* The whole visible tail shifts up a line, so everything must
     * be repainted on the next frame. */
    for (int r = 0; r < SHW_ROWS; r++) sh_row_dirty[r] = 1;
}

/* Append one character to the shell text ring.  Acts as a tiny VT100:
 *   - strips CSI escape sequences (ESC '[' ... final) emitted by the
 *     line editor's redraw (ESC[K clear-to-EOL, ESC[nD cursor-left) so
 *     they don't show up as literal "[K" / "[3D" garbage;
 *   - distinguishes a normal line ending "\r\n" (newline) from the line
 *     editor's "\r"+reprint (carriage return -> rewrite from column 0);
 *   - '\b'/0x7F backspace, other control chars dropped, printable
 *     appended (wrapping at SHW_COLS).
 * The per-char auto-null after each printable means a shorter rewrite
 * truncates any stale tail, so ESC[K need not be emulated explicitly. */
void gwin_shell_record(char c)
{
    /* --- CSI escape sequence stripping --- */
    if (sh_esc == 1) {                       /* saw ESC */
        sh_esc = (c == '[') ? 2 : 0;
        return;
    }
    if (sh_esc == 2) {                       /* inside ESC[ ... */
        if (c >= 0x40 && c <= 0x7E) sh_esc = 0;  /* final byte ends CSI */
        return;                              /* swallow the whole sequence */
    }
    if (c == 0x1B) { sh_esc = 1; return; }   /* ESC */

    /* --- carriage return: "\r\n" vs "\r"+rewrite --- */
    if (sh_cr) {
        sh_cr = 0;
        if (c == '\n') { sh_newline(); return; }
        sh_cur_col = 0;                      /* rewrite this line in place */
        sh_row_dirty[sh_cur_row] = 1;
        /* fall through to handle c at column 0 */
    }
    if (c == '\r') { sh_cr = 1; return; }
    if (c == '\n') { sh_newline(); return; }
    if (c == 0x08 || c == 0x7F) {            /* BS / DEL */
        if (sh_cur_col > 0) {
            sh_cur_col--;
            sh_ring[sh_cur_row][sh_cur_col] = 0;
            sh_row_dirty[sh_cur_row] = 1;
        }
        return;
    }
    if (c < 0x20) return;                    /* drop other control chars */

    if (sh_cur_col >= SHW_COLS) sh_newline();
    sh_ring[sh_cur_row][sh_cur_col++] = c;
    sh_ring[sh_cur_row][sh_cur_col] = 0;
    sh_row_dirty[sh_cur_row] = 1;
}

/* Diagnostic: copy the visible shell ring (oldest..newest) into buf as
 * newline-separated rows.  Returns the number of bytes written.  Used by
 * the /api/wifi/shellring HTTP endpoint to inspect what the shell has
 * actually emitted, independent of on-screen rendering. */
int gwin_shell_dump(char *buf, int max)
{
    int n = 0;
    int have = sh_filled ? SHW_ROWS : sh_cur_row + 1;
    int start = (sh_cur_row - have + 1 + SHW_ROWS) % SHW_ROWS;
    for (int i = 0; i < have && n < max - 2; i++) {
        int r = (start + i) % SHW_ROWS;
        const char *s = sh_ring[r];
        for (int k = 0; s[k] && n < max - 2; k++) buf[n++] = s[k];
        buf[n++] = '\n';
    }
    buf[n] = 0;
    return n;
}

/* Content callback: repaint only dirty rows (or all rows when
 * g_force_redraw is set, e.g. during the clipped cursor-erase). */
static void sh_draw(window_t *self, unsigned int frame)
{
    (void)frame;
    const int line_h = FONT_HEIGHT + 1;

    /* Cap visible rows to what physically fits, newest tail first. */
    int content_h = self->height - WM_TITLEBAR_H - 7;
    int max_rows = content_h / line_h;
    if (max_rows < 1) return;
    if (max_rows > SHW_ROWS) max_rows = SHW_ROWS;

    int have = sh_filled ? SHW_ROWS : sh_cur_row + 1;
    int rows = have < max_rows ? have : max_rows;

    /* `start` = oldest ring row to display (rows ending at sh_cur_row). */
    int start = (sh_cur_row - rows + 1 + SHW_ROWS) % SHW_ROWS;

    for (int i = 0; i < rows; i++) {
        int r = (start + i) % SHW_ROWS;
        if (g_force_redraw || sh_row_dirty[r]) {
            int ry = self->y + WM_TITLEBAR_H + 4 + i * line_h;
            /* Erase the row strip, then draw its text. */
            fill_rect(self->x + 4, ry, self->width - 8, line_h,
                      self->content_bg);
            draw_string_at(self->x + 4, ry, sh_ring[r],
                           0xFFCCE0FFU, self->content_bg);
            if (!g_force_redraw) sh_row_dirty[r] = 0;
        }
    }
}

/* Open (idempotently) the on-screen shell window and reset its ring.
 * Declared in gwm.h; called by the `win` shell command. */
int gwin_shell_window_open(void)
{
    static int opened = 0;
    if (opened) return OK;

    /* Do NOT clear the ring here: the shell bound to GWINCON0 starts
     * before gwm_main opens this window and may already have printed its
     * banner + first prompt into the ring.  The ring is static (zero-
     * initialised at boot), so just mark every row dirty so the first
     * frame paints whatever is already there. */
    for (int r = 0; r < SHW_ROWS; r++) sh_row_dirty[r] = 1;

    sh_win.x = 824;
    sh_win.y = 16;
    sh_win.width  = 440;
    sh_win.height = 724;
    {
        const char *t = "Shell";
        int i;
        for (i = 0; i < WM_TITLE_MAX && t[i]; i++) sh_win.title[i] = t[i];
        sh_win.title[i] = 0;
    }
    sh_win.chrome_color = 0xFFAACCEEU;
    sh_win.title_bg     = 0xFF0040A0U;
    sh_win.title_fg     = 0xFFFFFFFFU;
    sh_win.content_bg   = 0xFF000010U;
    sh_win.draw_content = sh_draw;

    wm_add(&sh_win);
    opened = 1;
    return OK;
}

void wm_run(void)
{
    unsigned int frame = 0;
    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();
    int dcx, dcy;          /* where the cursor was last drawn */

    /* Tail of the window list whose chrome has already been painted.
     * Windows added later (e.g. the shell window via the `win` command)
     * appear after this pointer and get their chrome + initial content
     * drawn once by the per-frame content pass below. */
    window_t *chrome_done = 0;

    (void)sw; (void)sh;

    /* Paint the whole scene once.  After this we only repaint the small
     * region under the cursor each frame — a full-screen redraw flickers
     * badly because every pixel write goes straight to the uncached
     * (MMU-off) framebuffer, so a 3 MB wipe + redraw is slow and visible.
     * Force every window's content to paint fully for this initial draw. */
    g_force_redraw = 1;
    paint_scene(0);
    g_force_redraw = 0;
    /* Remember the tail we just fully painted. */
    for (window_t *w = wm_head; w; w = w->next) chrome_done = w;
    dcx = cursor_x;
    dcy = cursor_y;
    draw_cursor();

    for (;;) {
        if (wm_tick) wm_tick();

        /* (a) Content pass — no clip.  For windows added since the last
         * frame, draw their chrome + force a full content paint once.
         * For already-painted windows, just call draw_content with
         * g_force_redraw=0 so incremental callbacks (sh_draw) repaint
         * only their dirty rows — small strips, so no flicker. */
        video_set_viewport(vp_x, vp_y);
        {
            int seen_done = (chrome_done == 0);
            window_t *newtail = chrome_done;
            for (window_t *w = wm_head; w; w = w->next) {
                if (!seen_done) {
                    /* Still walking the already-painted prefix. */
                    if (w == chrome_done) seen_done = 1;
                    if (w->draw_content) w->draw_content(w, frame);
                } else {
                    /* Newly-added window: paint chrome + full content. */
                    draw_chrome(w);
                    g_force_redraw = 1;
                    if (w->draw_content) w->draw_content(w, frame);
                    g_force_redraw = 0;
                }
                newtail = w;
            }
            chrome_done = newtail;
        }

        draw_wifi_indicator();   /* WiFi status mark, bottom-right corner */

        /* (b) Erase the cursor at its previously-drawn position by
         * repainting just that 12x12 patch of the scene (desktop + any
         * overlapping window content), then draw the cursor at its new
         * position.  g_force_redraw=1 makes window content callbacks
         * repaint fully inside the tiny clip so the patch is restored. */
        g_force_redraw = 1;
        video_set_clip(dcx, dcy, 12, 12);
        paint_scene(frame);
        video_clear_clip();
        g_force_redraw = 0;

        dcx = cursor_x;
        dcy = cursor_y;
        draw_cursor();

        delay_ms(1000 / DEFAULT_FPS);
        frame++;
    }
}

/* ===================================================================
 * Static window descriptors + Xinu thread entry.
 * ===================================================================
 */

static window_t info_win;

/* Simple info window — replaces the Pi5 shell window for M1 (no
 * Pi5-specific uart/shell dependencies).  Draws a few static lines. */
static void info_draw(window_t *self, unsigned int frame)
{
    (void)frame;
    draw_string_at(self->x + 8, self->y + WM_TITLEBAR_H + 6,
        "Xinu Pi3 Window System",
        0xFF00FF80U, self->content_bg);
    draw_string_at(self->x + 8, self->y + WM_TITLEBAR_H + 18,
        "BCM2837 Cortex-A53 -- arm-rpi3",
        0xFFCCCCCCU, self->content_bg);
    draw_string_at(self->x + 8, self->y + WM_TITLEBAR_H + 30,
        "HDMI framebuffer 1280x800x32",
        0xFF888888U, self->content_bg);
    draw_string_at(self->x + 8, self->y + WM_TITLEBAR_H + 42,
        "xsh in Shell window -- keys via /api/wifi/key",
        0xFF888888U, self->content_bg);
}

static void title_set(window_t *w, const char *t)
{
    int i;
    for (i = 0; i < WM_TITLE_MAX && t[i]; i++) w->title[i] = t[i];
    w->title[i] = 0;
}

/* ===================================================================
 *  Actor monitor window — a live table of the AIPL actors currently
 *  on this Xinu node: id, class, active state, and message counts
 *  (RECV = received, PROC = processed, PEND = backlog).  Reads the
 *  runtime via the abcl_object_* accessors every refresh, so actors
 *  spawned at runtime (e.g. over the RPC link) appear automatically.
 * =================================================================== */
extern int         abcl_n_objects(void);
extern int         abcl_object_class_id(int obj_id);
extern const char *abcl_class_name(int class_id);
extern int         abcl_object_enq(int obj_id);
extern int         abcl_object_deq(int obj_id);
extern int         abcl_object_drops(int obj_id);
extern int         abcl_object_started(int obj_id);
extern int         abcl_object_dead(int obj_id);

static window_t actor_win;

#define ACT_MAX_ROWS 30
#define ACT_LINE_W   64
static char act_row_cache[ACT_MAX_ROWS][ACT_LINE_W];

static void act_put_str(char *b, int *p, const char *s, int w)
{
    int i = 0;
    while (s && s[i] && i < w) { b[(*p)++] = s[i]; i++; }
    while (i < w) { b[(*p)++] = ' '; i++; }
}

static void act_put_int(char *b, int *p, long v, int w)
{
    char t[20];
    int  n = 0, neg = 0, j, pad;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) t[n++] = '-';
    for (pad = w - n; pad > 0; pad--) b[(*p)++] = ' ';
    for (j = n - 1; j >= 0; j--) b[(*p)++] = t[j];
}

static int act_str_eq(const char *a, const char *b)
{
    int i;
    for (i = 0; i < ACT_LINE_W; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static void act_str_cpy(char *d, const char *s)
{
    int i;
    for (i = 0; i < ACT_LINE_W - 1 && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
}

static void actor_draw(window_t *self, unsigned int frame)
{
    int cx = self->x + 6;
    int y0 = self->y + WM_TITLEBAR_H + 4;
    int n, i, p;
    char line[ACT_LINE_W];
    unsigned int bg = self->content_bg;

    /* Refresh a few times a second; the uncached framebuffer flickers
     * if every 20 fps frame repaints, so update on every 7th frame. */
    if ((frame % 7) != 0) return;

    draw_string_at(cx, y0, "ID CLASS        ST   RECV  PROC  PEND",
                   0xFF80C0FFU, bg);

    n = abcl_n_objects();
    for (i = 0; i < ACT_MAX_ROWS - 1; i++) {
        int ry = y0 + (i + 1) * 12;
        if (i < n) {
            int enq  = abcl_object_enq(i);
            int deq  = abcl_object_deq(i);
            int pend = enq - deq;
            const char *cls = abcl_class_name(abcl_object_class_id(i));
            const char *st;
            if (abcl_object_dead(i))     st = "dead";
            else if (!abcl_object_started(i)) st = "new";
            else if (pend > 0)           st = "act";
            else                         st = "idle";
            p = 0;
            act_put_int(line, &p, i, 2);                line[p++] = ' ';
            act_put_str(line, &p, cls ? cls : "?", 12); line[p++] = ' ';
            act_put_str(line, &p, st, 4);               line[p++] = ' ';
            act_put_int(line, &p, enq, 5);              line[p++] = ' ';
            act_put_int(line, &p, deq, 5);              line[p++] = ' ';
            act_put_int(line, &p, pend, 5);
            line[p] = '\0';
        } else {
            line[0] = '\0';
        }
        /* Repaint a row only when its text changed (cuts flicker). */
        if (!act_str_eq(line, act_row_cache[i])) {
            fill_rect(self->x + 1, ry, self->width - 2, 12, bg);
            if (line[0]) {
                unsigned int fg = (i < n && abcl_object_class_id(i) == 1)
                                  ? 0xFFFFE080U   /* Philosopher: amber */
                                  : 0xFFD0D0D0U;  /* others: grey */
                draw_string_at(cx, ry, line, fg, bg);
            }
            act_str_cpy(act_row_cache[i], line);
        }
    }
}

/* ===================================================================
 *  AIPL print console window — shows the text passed to the AIPL
 *  print() / v_print() runtime (apps/abcl_program.c calls
 *  aipl_console_puts() for every printed line).  A scrolling ring of
 *  the most recent lines; the Xinu philosophers narrate their state
 *  here (thinking / took fork / eating / put down fork).
 * =================================================================== */
#define CON_ROWS  23
#define CON_COLW  50
static char con_ring[CON_ROWS][CON_COLW];
static volatile unsigned int con_head = 0;          /* total lines ever written */
static unsigned int con_drawn = 0xFFFFFFFFu;

/* Append one line to the console ring.  Called from the AIPL runtime
 * (abcl_program.c v_print) on the actor threads. */
void aipl_console_puts(const char *s)
{
    unsigned int slot = con_head % CON_ROWS;
    int i = 0;
    while (s && s[i] && i < CON_COLW - 1) { con_ring[slot][i] = s[i]; i++; }
    while (i < CON_COLW - 1) con_ring[slot][i++] = ' ';   /* pad: clean overwrite */
    con_ring[slot][CON_COLW - 1] = '\0';
    con_head++;
}

static window_t console_win;

static void console_draw(window_t *self, unsigned int frame)
{
    int cx = self->x + 6;
    int y0 = self->y + WM_TITLEBAR_H + 4;
    unsigned int head, total, start;
    int r;

    if ((frame % 7) != 0) return;       /* ~3 Hz */
    head = con_head;
    if (head == con_drawn) return;       /* nothing new since last paint */
    con_drawn = head;

    total = head;
    start = (total > CON_ROWS) ? (total - CON_ROWS) : 0;
    for (r = 0; r < CON_ROWS; r++) {
        int ry = y0 + r * 12;
        if (start + (unsigned int)r < total) {
            unsigned int slot = (start + (unsigned int)r) % CON_ROWS;
            /* the line is space-padded to full width, so this single draw
             * cleanly overwrites the previous row content (low flicker). */
            draw_string_at(cx, ry, con_ring[slot],
                           0xFF40FF80U, self->content_bg);
        } else {
            fill_rect(self->x + 1, ry, self->width - 2, 12, self->content_bg);
        }
    }
}

thread gwm_main(void)
{
    extern int kprintf(const char *, ...);
    int rc;

    kprintf("[gwm] enter\r\n");
    rc = video_init();
    kprintf("[gwm] video_init rc=%d\r\n", rc);
    if (rc != 0) {
        /* No framebuffer — nothing to render.  Park the thread. */
        kprintf("[gwm] no framebuffer, parking\r\n");
        for (;;) sleep(1000);
    }
    kprintf("[gwm] fb ok, sw=%d sh=%d, building windows\r\n",
            (int)video_screen_width(), (int)video_screen_height());

    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();

    /* Info window: top-left. */
    info_win.x = 16;
    info_win.y = 16;
    info_win.width  = 420;
    info_win.height = 120;
    title_set(&info_win, "Xinu Pi3");
    info_win.chrome_color = 0xFFAACCEEU;
    info_win.title_bg     = 0xFF0040A0U;
    info_win.title_fg     = 0xFFFFFFFFU;
    info_win.content_bg   = 0xFF000010U;
    info_win.draw_content = info_draw;
    wm_add(&info_win);

    /* Soft keyboard window: left column, bottom (below the AIPL console,
     * which ends at y=452) so the two never overlap. */
    softkbd_win.x = 16;
    softkbd_win.y = 450;
    softkbd_win.width  = 420;
    softkbd_win.height = 290;
    title_set(&softkbd_win, "Soft keyboard");
    softkbd_win.chrome_color = 0xFFFFB060U;
    softkbd_win.title_bg     = 0xFF704020U;
    softkbd_win.title_fg     = 0xFFFFFFFFU;
    softkbd_win.content_bg   = 0xFF0A0A14U;
    softkbd_win.draw_content = softkbd_draw;
    wm_add(&softkbd_win);

    /* AIPL print console: left column, below the info window and left of
     * the actor monitor (no overlap with info / actor / soft keyboard). */
    console_win.x = 16;
    console_win.y = 148;
    console_win.width  = 420;
    console_win.height = 296;
    title_set(&console_win, "AIPL console (print)");
    console_win.chrome_color = 0xFF80FF80U;
    console_win.title_bg     = 0xFF205020U;
    console_win.title_fg     = 0xFFFFFFFFU;
    console_win.content_bg   = 0xFF001000U;
    console_win.draw_content = console_draw;
    wm_add(&console_win);

    /* Actor monitor: middle column, top half. */
    actor_win.x = 448;
    actor_win.y = 16;
    actor_win.width  = 360;
    actor_win.height = 348;
    title_set(&actor_win, "AIPL actors (live)");
    actor_win.chrome_color = 0xFF60E0A0U;
    actor_win.title_bg     = 0xFF105030U;
    actor_win.title_fg     = 0xFFFFFFFFU;
    actor_win.content_bg   = 0xFF001008U;
    actor_win.draw_content = actor_draw;
    wm_add(&actor_win);

    /* Graphics window: middle column, below the actor monitor.  Shows the
     * rotating wire-frame 3-D wine glass driven by the `wine` xsh command. */
    graphics_win.x = 448;
    graphics_win.y = 372;
    graphics_win.width  = 360;
    graphics_win.height = 368;
    title_set(&graphics_win, "graphics");
    graphics_win.chrome_color = 0xFFB070E0U;
    graphics_win.title_bg     = 0xFF402060U;
    graphics_win.title_fg     = 0xFFFFFFFFU;
    graphics_win.content_bg   = 0xFF050810U;
    graphics_win.draw_content = graphics_draw;
    wm_add(&graphics_win);

    /* Interactive xsh window: right column.  Its text ring is fed by
     * the shell bound to GWINCON0 (see system/main.c); keystrokes are
     * injected over HTTP via gwincon_feed().  Opened here so it is part
     * of the initial full scene paint. */
    gwin_shell_window_open();
    /* Diagnostic test line written directly into the ring (bypasses the
     * shell/GWINCON0 path) to verify the ring -> sh_draw render path. */
    {
        const char *t = "[gwm] Shell window ready -- type in the Mac /pi3 page\n";
        for (int i = 0; t[i]; i++) gwin_shell_record(t[i]);
    }

    /* Start the cursor at the centre of the screen. */
    wm_cursor_set(sw / 2, sh / 2, 1);

    /* The wine glass is drawn statically at boot and only spins when the
     * `wine` xsh command is run — keeping the WiFi responder thread fed
     * (a continuous animation starved it).  g_wine_active stays 0 here. */

    wm_run();   /* never returns */
    return OK;  /* unreachable — keeps the compiler happy */
}

#endif /* _XINU_PLATFORM_ARM_RPI3_ */
