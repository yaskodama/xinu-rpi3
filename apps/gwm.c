// apps/gwm.c — window manager: chrome + redraw loop (ported from
// xinu-rpi5 device/video/wm.c for the Pi 3 / arm-rpi3 platform).
//
// Also hosts gwm_main(), the Xinu thread entry that brings up the
// framebuffer, lays out a couple of static windows + the soft
// keyboard, and enters the never-returning redraw loop.

#ifdef _XINU_PLATFORM_ARM_RPI3_

#include <thread.h>
#include <semaphore.h>
#include <stdio.h>      /* sprintf() for wm_dump_layout() */
#include <gwm.h>
#include <gvideo.h>
#include <gsoftkbd.h>

#define DESKTOP_BG     0xFF003366U   /* dark navy "desktop"          */
#define DEFAULT_FPS    20            /* content repaint: 1 frame / 50 ms */
/* The USB mouse updates cursor_x/y asynchronously (USB IRQ) far faster than
 * the 20 fps content pass.  Within each content frame we poll the cursor every
 * WM_CURSOR_STEP_MS and move just the 12x12 sprite, so the pointer tracks the
 * mouse at ~1000/WM_CURSOR_STEP_MS Hz instead of jumping once per content frame. */
#define WM_CURSOR_STEP_MS 2          /* poll the cursor ~500 Hz (USB caps it) */

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

/* The active (focused) window — the last one clicked/raised.  Drawn with a
 * thick highlighted border so it's obvious which window is selected. */
static window_t *active_win;

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

/* ---- mouse-cursor backing store -----------------------------------
 * The pointer must track the USB mouse smoothly (~hundreds of Hz), but the
 * scene repaint (windows + the soft-float 3-D wine) only runs at 20 fps and
 * is expensive on this uncached, D-cache-off framebuffer.  So instead of
 * repainting the scene under the cursor on every move, we stash the 12x12
 * patch beneath it and put it back when it moves — a pure pixel blit, no
 * window/wine redraw.  cursor_show()/cursor_hide() are the only things that
 * touch the cursor; wm_run() calls hide before a content pass and show after,
 * and hide+show on each move in the fast sub-loop. */
extern void video_save_rect(int, int, int, int, unsigned int *);
extern void video_restore_rect(int, int, int, int, const unsigned int *);
static unsigned int cursor_bak[12 * 12];
static int          cursor_bak_valid = 0;
static int          bak_x, bak_y;       /* where the sprite is currently drawn */

static void draw_cursor_xy(int cx, int cy)
{
    if (!cursor_visible) return;
    /* Cursor is in screen coords — reset the viewport + clip so the sprite
     * always renders 1:1 onto the physical display. */
    int save_x = video_viewport_x();
    int save_y = video_viewport_y();
    video_set_viewport(0, 0);
    video_clear_clip();

    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();
    for (int dy = 0; dy < 12; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= sh) continue;
        for (int dx = 0; dx < 12; dx++) {
            int px = cx + dx;
            if (px < 0 || px >= sw) continue;
            char c = cursor_sprite[dy][dx];
            if (c == '#')      fill_rect(px, py, 1, 1, 0xFFFFFFFFU);
            else if (c == '.') fill_rect(px, py, 1, 1, 0xFF000000U);
        }
    }

    video_set_viewport(save_x, save_y);
}

/* Put back the pixels the sprite is covering (fast blit; no scene repaint). */
static void cursor_hide(void)
{
    if (cursor_bak_valid) {
        video_restore_rect(bak_x, bak_y, 12, 12, cursor_bak);
        cursor_bak_valid = 0;
    }
}

/* Stash the patch under the current cursor position, then draw the sprite.
 * Snapshots cursor_x/y once so a USB-IRQ update mid-call can't desync the
 * saved patch from where the sprite lands. */
static void cursor_show(void)
{
    if (!cursor_visible) return;
    int x = cursor_x, y = cursor_y;
    video_save_rect(x, y, 12, 12, cursor_bak);
    bak_x = x; bak_y = y; cursor_bak_valid = 1;
    draw_cursor_xy(x, y);
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

    /* resize grip — small dots in the bottom-right corner (drag here to
     * resize).  Cosmetic hint; the grab zone is WM_RESIZE_GRAB regardless. */
    {
        int rx = w->x + w->width, ry = w->y + w->height;
        fill_rect(rx - 5,  ry - 5,  3, 3, w->chrome_color);
        fill_rect(rx - 10, ry - 5,  2, 2, w->chrome_color);
        fill_rect(rx - 5,  ry - 10, 2, 2, w->chrome_color);
    }

    /* Active-window highlight: a thick bright frame drawn on top so the
     * selected window stands out from the others. */
    if (w == active_win) {
        unsigned int hc = 0xFFFFD23CU;          /* amber */
        draw_rect(w->x,     w->y,     w->width,     w->height,     hc);
        draw_rect(w->x + 1, w->y + 1, w->width - 2, w->height - 2, hc);
        draw_rect(w->x + 2, w->y + 2, w->width - 4, w->height - 4, hc);
    }
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
    extern void wifi_dhcp_diag(unsigned char *ip, unsigned char *gw, int *have);
    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();
    int conn = wifi_connected();
    unsigned int col = conn ? 0xFF36D35AU : 0xFFFFFFFFU;   /* green / white */
    int cx = sw - 24;          /* fan centre x */
    int by = sh - 40;          /* mark dot baseline (room for SSID + IP below) */

    /* Dark backing box covering the mark + SSID + IP label lines. */
    fill_rect(sw - 152, by - 22, 150, 60, 0xFF182028U);
    /* Three arcs (approximated by centred bars) + a dot, fanning upward. */
    fill_rect(cx - 13, by - 18, 26, 2, col);   /* outer */
    fill_rect(cx - 9,  by - 13, 18, 2, col);   /* middle */
    fill_rect(cx - 5,  by - 8,  10, 2, col);   /* inner */
    fill_rect(cx - 2,  by - 3,   4, 4, col);   /* dot */
    /* SSID + IP under the mark when connected, else a hint. */
    if (conn && wifi_ssid()[0]) {
        unsigned char ip[4], gw[4]; int have = 0;
        draw_string_at(sw - 148, by + 8, wifi_ssid(), 0xFFA0E0FFU, 0xFF182028U);
        wifi_dhcp_diag(ip, gw, &have);
        if (have) {
            char ipstr[20];
            sprintf(ipstr, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            draw_string_at(sw - 148, by + 20, ipstr, 0xFF80FF80U, 0xFF182028U);
        }
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
    /* Redraw the (expensive, soft-float) wine glass only every 4th frame.
     * Its recompute is the main thing that freezes the mouse cursor during a
     * content pass, so a lower wine rate = fewer cursor hitches. */
    if (g_wine_active && (frame & 3) && !g_force_redraw) return;

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
static char          sh_csi[8];    /* CSI parameter bytes (e.g. "2" of 2J)*/
static int           sh_csi_n;
static int           sh_full_clear; /* clear: wipe the whole content area    */

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

/* Clear the shell window: blank every ring row and home the cursor.
 * Driven by the `clear` command, which prints ESC[2J (erase display). */
static void gwin_shell_clear(void)
{
    for (int r = 0; r < SHW_ROWS; r++) { sh_ring[r][0] = 0; sh_row_dirty[r] = 1; }
    sh_cur_row = 0;
    sh_cur_col = 0;
    sh_filled  = 0;
    sh_full_clear = 1;   /* next sh_draw wipes the entire content area, not
                          * just the (now single) live row — otherwise the
                          * previously-displayed lines stay on screen. */
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
        if (c == '[') { sh_esc = 2; sh_csi_n = 0; } else sh_esc = 0;
        return;
    }
    if (sh_esc == 2) {                       /* inside ESC[ ... */
        if ((c >= '0' && c <= '9') || c == ';') {     /* accumulate params */
            if (sh_csi_n < (int)sizeof sh_csi - 1) sh_csi[sh_csi_n++] = c;
            return;
        }
        if (c >= 0x40 && c <= 0x7E) {         /* final byte ends the CSI */
            sh_csi[sh_csi_n] = 0;
            if (c == 'J') gwin_shell_clear(); /* ESC[2J / ESC[J = clear screen */
            sh_esc = 0; sh_csi_n = 0;
        }
        return;                               /* swallow the whole sequence */
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
    const int line_h = FONT_HEIGHT + 1;

    /* `clear` requested: wipe the whole content area once (the row loop only
     * repaints the live rows, which after a clear is just one). */
    if (sh_full_clear) {
        fill_rect(self->x + 1, self->y + WM_TITLEBAR_H + 2,
                  self->width - 2, self->height - WM_TITLEBAR_H - 3,
                  self->content_bg);
        sh_full_clear = 0;
    }

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

    /* Blinking block caret at the input position (the newest visible row,
     * column sh_cur_col).  Drawn every frame so it blinks; the input cell is
     * always blank, so the "off" phase just repaints the content background. */
    {
        int cr_y = self->y + WM_TITLEBAR_H + 4 + (rows - 1) * line_h;
        int cr_x = self->x + 4 + sh_cur_col * FONT_WIDTH;
        if (cr_x + FONT_WIDTH <= self->x + self->width - 4) {
            unsigned int col = ((frame >> 2) & 1) ? 0xFFFFD23CU : self->content_bg;
            fill_rect(cr_x, cr_y, FONT_WIDTH, FONT_HEIGHT, col);
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
    sh_win.y = 18;
    sh_win.width  = 456;
    sh_win.height = 476;
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

/* ---- title-bar drag (move) + corner drag (resize) -----------------
 * Hold the LEFT mouse button (usbmouse_buttons bit 0, USB boot protocol):
 *   - on a window's title bar  -> move the window
 *   - on its bottom-right grip -> resize the window. */
extern volatile int usbmouse_buttons;
#define WM_RESIZE_GRAB 16                  /* bottom-right grab square (px)   */
#define WM_MIN_W       96
#define WM_MIN_H       (WM_TITLEBAR_H + 24)
static window_t *drag_win;                 /* window being dragged, or 0      */
static int       drag_mode;                /* 1 = move, 2 = resize            */
static int       drag_off_x, drag_off_y;   /* cursor offset captured on grab  */
static int       g_need_full;              /* a window moved/resized -> repaint*/

/* Topmost window whose title bar covers screen point (sx,sy), or 0.
 * Windows live in desktop coords; add the viewport to map screen->desktop. */
static window_t *window_at_titlebar(int sx, int sy)
{
    int dx = sx + vp_x, dy = sy + vp_y;
    window_t *hit = 0;
    for (window_t *w = wm_head; w; w = w->next)
        if (dx >= w->x && dx < w->x + w->width &&
            dy >= w->y && dy < w->y + WM_TITLEBAR_H + 2)
            hit = w;                        /* last match = drawn last = on top */
    return hit;
}

/* Topmost window whose bottom-right resize grip covers (sx,sy), or 0. */
static window_t *window_at_resize(int sx, int sy)
{
    int dx = sx + vp_x, dy = sy + vp_y;
    window_t *hit = 0;
    for (window_t *w = wm_head; w; w = w->next)
        if (dx >= w->x + w->width  - WM_RESIZE_GRAB && dx < w->x + w->width &&
            dy >= w->y + w->height - WM_RESIZE_GRAB && dy < w->y + w->height)
            hit = w;
    return hit;
}

/* Topmost window containing screen point (sx,sy) anywhere, or 0. */
static window_t *window_at_point(int sx, int sy)
{
    int dx = sx + vp_x, dy = sy + vp_y;
    window_t *hit = 0;
    for (window_t *w = wm_head; w; w = w->next)
        if (dx >= w->x && dx < w->x + w->width &&
            dy >= w->y && dy < w->y + w->height)
            hit = w;
    return hit;
}

/* Move w to the tail of the list so it repaints on top of the others. */
static void wm_raise(window_t *w)
{
    if (!w || !wm_head || !w->next) return;          /* single / already top */
    if (wm_head == w) wm_head = w->next;
    else { window_t *p = wm_head; while (p->next && p->next != w) p = p->next;
           if (p->next == w) p->next = w->next; }
    w->next = 0;
    window_t *t = wm_head; while (t->next) t = t->next; t->next = w;
}

/* BASIC-window toolbar (defined in the BASIC section below). */
static int  basic_toolbar_hit(int sx, int sy);   /* button index at screen pt, or -1 */
static void basic_run_button(int idx);           /* run that button's command */

/* Process the left button each cursor poll: start / continue / end a drag. */
static void wm_drag_tick(void)
{
    static int prev_left = 0;
    int left = usbmouse_buttons & 1;

    /* On a fresh press, a click on a BASIC toolbar button runs its command
     * (edge-triggered so holding doesn't fire it every frame). */
    if (left && !prev_left) {
        int b = basic_toolbar_hit(cursor_x, cursor_y);
        if (b >= 0) { basic_run_button(b); prev_left = left; return; }
    }
    prev_left = left;

    if (left && !drag_win) {                         /* press: focus + maybe grab */
        window_t *w = window_at_resize(cursor_x, cursor_y);    /* corner first */
        if (w) {
            drag_win = w; drag_mode = 2;
            drag_off_x = (cursor_x + vp_x) - (w->x + w->width);
            drag_off_y = (cursor_y + vp_y) - (w->y + w->height);
        } else if ((w = window_at_titlebar(cursor_x, cursor_y)) != 0) {
            drag_win = w; drag_mode = 1;
            drag_off_x = (cursor_x + vp_x) - w->x;
            drag_off_y = (cursor_y + vp_y) - w->y;
        } else {
            w = window_at_point(cursor_x, cursor_y);  /* body click = focus only */
        }
        if (w) { active_win = w; wm_raise(w); g_need_full = 1; }
    } else if (left && drag_win && drag_mode == 1) {  /* move: follow the cursor */
        int nx = (cursor_x + vp_x) - drag_off_x;
        int ny = (cursor_y + vp_y) - drag_off_y;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > WM_DESKTOP_W - drag_win->width)  nx = WM_DESKTOP_W - drag_win->width;
        if (ny > WM_DESKTOP_H - drag_win->height) ny = WM_DESKTOP_H - drag_win->height;
        if (nx != drag_win->x || ny != drag_win->y) {
            drag_win->x = nx; drag_win->y = ny;
            g_need_full = 1;
        }
    } else if (left && drag_win && drag_mode == 2) {  /* resize: corner follows */
        int nw = (cursor_x + vp_x) - drag_off_x - drag_win->x;
        int nh = (cursor_y + vp_y) - drag_off_y - drag_win->y;
        if (nw < WM_MIN_W) nw = WM_MIN_W;
        if (nh < WM_MIN_H) nh = WM_MIN_H;
        if (drag_win->x + nw > WM_DESKTOP_W) nw = WM_DESKTOP_W - drag_win->x;
        if (drag_win->y + nh > WM_DESKTOP_H) nh = WM_DESKTOP_H - drag_win->y;
        if (nw != drag_win->width || nh != drag_win->height) {
            drag_win->width = nw; drag_win->height = nh;
            g_need_full = 1;
        }
    } else if (!left && drag_win) {                  /* release */
        drag_win = 0; drag_mode = 0;
    }
}

void wm_run(void)
{
    unsigned int frame = 0;
    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();

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
    cursor_show();

    for (;;) {
        if (wm_tick) wm_tick();
        cursor_hide();        /* lift the sprite so the content pass draws clean */
        wm_drag_tick();       /* process the left button: start/continue a drag */

        video_set_viewport(vp_x, vp_y);
        if (g_need_full) {
            /* A window was moved or raised — repaint the whole scene so the
             * old position is cleared and the new z-order is honoured. */
            g_force_redraw = 1;
            paint_scene(frame);
            g_force_redraw = 0;
            window_t *t = 0;
            for (window_t *w = wm_head; w; w = w->next) t = w;
            chrome_done = t;
            g_need_full = 0;
        } else {
            /* (a) Content pass — incremental.  For windows added since the
             * last frame, draw chrome + a full content paint once; already-
             * painted windows just repaint their dirty rows (no flicker). */
            int seen_done = (chrome_done == 0);
            window_t *newtail = chrome_done;
            for (window_t *w = wm_head; w; w = w->next) {
                if (!seen_done) {
                    if (w == chrome_done) seen_done = 1;
                    if (w->draw_content) w->draw_content(w, frame);
                } else {
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

        /* Re-stash the patch under the cursor (the content pass may have
         * redrawn there) and draw the sprite on top. */
        cursor_show();
        frame++;

        /* Fast cursor sub-loop.  USB mouse reports move cursor_x/y
         * asynchronously; the heavy content pass above only runs at 20 fps.
         * Spend the rest of the frame period polling the cursor and, whenever
         * it moved, just restore the 12x12 patch under the old position and
         * blit the sprite at the new one (cursor_hide/show) — a pure pixel
         * copy, NO scene repaint and NO 3-D wine recompute, so the pointer
         * tracks the mouse smoothly even on the uncached framebuffer. */
        for (int t = 0; t < 1000 / DEFAULT_FPS; t += WM_CURSOR_STEP_MS) {
            delay_ms(WM_CURSOR_STEP_MS);
            wm_drag_tick();                 /* left-button drag at full poll rate */
            if (g_need_full) break;         /* window moved -> repaint now (loop top) */
            if (cursor_x == bak_x && cursor_y == bak_y) continue;
            cursor_hide();                  /* cursor moved: fast sprite blit */
            cursor_show();
        }
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

/* ============================================================ *
 *  BASIC interpreter window                                    *
 *  A line-numbered BASIC (apps/basic.c) in its own gwm window.  *
 *  Interpreter output -> bas_emit (the window's text ring);     *
 *  keystrokes -> basic_feed -> a line queue the BASIC thread     *
 *  (basic_main) and INPUT consume.  gwm_feed_key() routes keys   *
 *  to whichever window is active (shell vs BASIC).              *
 * ============================================================ */
extern void basic_set_emit(void (*)(const char *));
extern void basic_set_input(int (*)(char *, int));
extern void basic_init(void);
extern void basic_exec_line(const char *);

#define BAS_ROWS 56
#define BAS_COLS 60
#define BAS_TB_H 16                 /* toolbar height (px)                    */
static char          bas_ring[BAS_ROWS][BAS_COLS + 1];
static unsigned char bas_dirty[BAS_ROWS];
static int           bas_row, bas_col, bas_filled, bas_full;
static int           bas_gfx;       /* 1 = pixel graphics (CLS/PLOT/LINE), 0 = text scroll */
static window_t      basic_win;

/* Top of the scrolling text / graphics area, below the title bar + toolbar. */
#define BAS_TXT_TOP(self) ((self)->y + WM_TITLEBAR_H + 2 + BAS_TB_H)
/* The graphics content rectangle (desktop coords) of the BASIC window. */
static void bas_gfx_rect(int *gx0, int *gy0, int *gw, int *gh)
{
    *gx0 = basic_win.x + 4;
    *gy0 = basic_win.y + WM_TITLEBAR_H + 2 + BAS_TB_H;
    *gw  = basic_win.width - 8;
    *gh  = basic_win.height - (*gy0 - basic_win.y) - 3;
}
static int bas_abs(int v) { return v < 0 ? -v : v; }
static unsigned int bas_palette(int c)
{
    static const unsigned int pal[16] = {
        0xFF000000U, 0xFF3060FFU, 0xFF30D040U, 0xFF30D0D0U,
        0xFFE03030U, 0xFFE040E0U, 0xFFE0E040U, 0xFFF0F0F0U,
        0xFF808080U, 0xFF80A0FFU, 0xFF80FF80U, 0xFF80FFFFU,
        0xFFFF8080U, 0xFFFF80FFU, 0xFFFFFF80U, 0xFFFFFFFFU };
    return pal[c & 15];
}

/* Full-screen editor cursor (classic micro-BASIC style): arrow keys roam it
 * over the on-screen program text; Enter submits the logical line under it.
 * It tracks the output position (bas_row/bas_col) while the interpreter is
 * printing, then the user can move it up to re-edit a previous line. */
static int           bas_ed_row, bas_ed_col;
static int           bas_esc;      /* CSI parser: 0 idle, 1 saw ESC, 2 saw ESC[ */

static void bas_newline(void)
{
    bas_ring[bas_row][bas_col] = 0;
    bas_row = (bas_row + 1) % BAS_ROWS; bas_col = 0; bas_ring[bas_row][0] = 0;
    if (bas_row == 0) bas_filled = 1;
    for (int r = 0; r < BAS_ROWS; r++) bas_dirty[r] = 1;
}
static void bas_putc(char c)
{
    if (bas_gfx) {               /* leaving graphics mode — wipe + reset to text */
        bas_gfx = 0; bas_full = 1;
        bas_row = 0; bas_col = 0; bas_filled = 0; bas_ring[0][0] = 0;
    }
    if (c == '\r') return;
    if (c == '\n') { bas_newline(); return; }
    if (c == '\t') { do { if (bas_col < BAS_COLS) bas_ring[bas_row][bas_col++] = ' '; }
                     while ((bas_col % 8) && bas_col < BAS_COLS);
                     bas_ring[bas_row][bas_col] = 0; bas_dirty[bas_row] = 1; return; }
    if (c == 8 || c == 0x7f) { if (bas_col > 0) { bas_col--; bas_ring[bas_row][bas_col] = 0; bas_dirty[bas_row] = 1; } return; }
    if (c < 0x20) return;
    if (bas_col >= BAS_COLS) bas_newline();
    bas_ring[bas_row][bas_col++] = c; bas_ring[bas_row][bas_col] = 0; bas_dirty[bas_row] = 1;
}
static void bas_emit(const char *s)
{
    while (*s) bas_putc(*s++);
    /* Interpreter output lands at the bottom; keep the edit cursor there so
     * the next keystroke continues from the live line (the user can still
     * arrow up afterwards to re-edit history). */
    bas_ed_row = bas_row; bas_ed_col = bas_col;
}

/* ---- CLS / PLOT / LINE / PAUSE: pixel graphics drawn straight into the
 * BASIC window's content area.  basic_draw() leaves the content untouched
 * while in graphics mode (bas_gfx), so what we paint here persists frame to
 * frame; a program animates by CLS + draw + PAUSE in a loop (rotate.bas). -- */
/* CLS mode: 1 = text screen, 2 = graphics screen, 3 = both. */
static void bas_cls(int mode)
{
    if (mode == 2 || mode == 3) {            /* clear the graphics layer */
        int gx0, gy0, gw, gh;
        bas_gfx_rect(&gx0, &gy0, &gw, &gh);
        if (gw > 0 && gh > 0) fill_rect(gx0, gy0, gw, gh, 0xFF001405U);
    }
    if (mode == 1 || mode == 3) {            /* clear the text screen */
        int r;
        for (r = 0; r < BAS_ROWS; r++) { bas_ring[r][0] = 0; bas_dirty[r] = 1; }
        bas_row = 0; bas_col = 0; bas_filled = 0;
        bas_ed_row = 0; bas_ed_col = 0; bas_full = 1;
    }
    /* show the graphics layer for a graphics clear, else the text layer */
    bas_gfx = (mode == 2) ? 1 : 0;
}
/* PLOT x,y[,char]: a character at pixel cell (x,y) on the graphics screen. */
static void bas_plot(int x, int y, int ch)
{
    int gx0, gy0, gw, gh; char s[2];
    bas_gfx_rect(&gx0, &gy0, &gw, &gh);
    bas_gfx = 1;
    if (ch < 0x20 || ch > 0x7e) ch = '*';
    if (x < 0 || x * FONT_WIDTH >= gw || y < 0 || y * FONT_HEIGHT >= gh) return;
    s[0] = (char)ch; s[1] = 0;
    draw_string_at(gx0 + x * FONT_WIDTH, gy0 + y * FONT_HEIGHT, s, 0xFFB6FFB6U, 0xFF001405U);
}
/* LINE(x1,y1)-(x2,y2),color: a line segment (Bresenham) on the graphics
 * screen, coordinates in content-area pixels, colour 0..15. */
static void bas_line(int x1, int y1, int x2, int y2, int color)
{
    int gx0, gy0, gw, gh;
    unsigned int col = bas_palette(color);
    int dx = bas_abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -bas_abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    bas_gfx_rect(&gx0, &gy0, &gw, &gh);
    bas_gfx = 1;
    for (;;) {
        if (x1 >= 0 && x1 < gw && y1 >= 0 && y1 < gh)
            fill_rect(gx0 + x1, gy0 + y1, 1, 1, col);
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}
static void bas_pause(int ms)
{
    if (ms < 0) ms = 0;
    if (ms > 5000) ms = 5000;
    sleep(ms);   /* yields so the wm render thread paints each frame */
}

/* ---- BASIC window toolbar: clickable buttons that run a command --------- */
static int bas_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static const struct { const char *label; const char *cmd; } bas_btns[] = {
    { "FILES",      "files" },
    { "LIST",       "list" },
    { "RUN hello",  "run \"hello.bas\"" },
    { "RUN rotate", "run \"rotate.bas\"" },
};
#define BAS_NBTN ((int)(sizeof(bas_btns) / sizeof(bas_btns[0])))

/* Button i's rectangle in desktop coords (what basic_draw paints + what the
 * click test compares against). */
static void bas_btn_rect(int i, int *bx, int *by, int *bw, int *bh)
{
    int x = basic_win.x + 4, k;
    for (k = 0; k < i; k++) x += bas_slen(bas_btns[k].label) * FONT_WIDTH + 8 + 4;
    *bx = x;
    *by = basic_win.y + WM_TITLEBAR_H + 3;
    *bw = bas_slen(bas_btns[i].label) * FONT_WIDTH + 8;
    *bh = BAS_TB_H - 4;
}
static void basic_draw_toolbar(window_t *self)
{
    int i, bx, by, bw, bh;
    fill_rect(self->x + 1, self->y + WM_TITLEBAR_H + 2, self->width - 2, BAS_TB_H, 0xFF0A2A12U);
    for (i = 0; i < BAS_NBTN; i++) {
        bas_btn_rect(i, &bx, &by, &bw, &bh);
        if (bx + bw > self->x + self->width - 2) break;     /* clip to window */
        fill_rect(bx, by, bw, bh, 0xFF1E6E38U);
        fill_rect(bx, by, bw, 1, 0xFF2EA050U);              /* top highlight  */
        draw_string_at(bx + 4, by + 1, bas_btns[i].label, 0xFFEFFFE0U, 0xFF1E6E38U);
    }
}

static void basic_draw(window_t *self, unsigned int frame)
{
    const int line_h = FONT_HEIGHT + 1;
    if (bas_full) { fill_rect(self->x + 1, self->y + WM_TITLEBAR_H + 2,
                              self->width - 2, self->height - WM_TITLEBAR_H - 3, self->content_bg); bas_full = 0; }
    basic_draw_toolbar(self);
    /* Graphics mode: CLS/PLOT/LINE paint the content directly; leave it be. */
    if (bas_gfx) return;
    int content_h = self->height - (BAS_TXT_TOP(self) - self->y) - 3;
    int max_rows = content_h / line_h; if (max_rows < 1) return; if (max_rows > BAS_ROWS) max_rows = BAS_ROWS;
    int rows, start;
    {                                    /* scrolling text console */
        int have = bas_filled ? BAS_ROWS : bas_row + 1;
        rows = have < max_rows ? have : max_rows;
        start = (bas_row - rows + 1 + BAS_ROWS) % BAS_ROWS;
    }
    for (int i = 0; i < rows; i++) {
        int r = (start + i) % BAS_ROWS;
        if (g_force_redraw || bas_dirty[r]) {
            int ry = BAS_TXT_TOP(self) + i * line_h;
            fill_rect(self->x + 4, ry, self->width - 8, line_h, self->content_bg);
            draw_string_at(self->x + 4, ry, bas_ring[r], 0xFFB6FFB6U, self->content_bg);
            if (!g_force_redraw) bas_dirty[r] = 0;
        }
    }
    {                                    /* blinking underline at the edit cursor */
        int i = (bas_ed_row - start + BAS_ROWS) % BAS_ROWS;
        if (i >= 0 && i < rows) {
            int cr_y = BAS_TXT_TOP(self) + i * line_h;
            int cr_x = self->x + 4 + bas_ed_col * FONT_WIDTH;
            if (cr_x + FONT_WIDTH <= self->x + self->width - 4) {
                unsigned int col = ((frame >> 2) & 1) ? 0xFF66FF66U : self->content_bg;
                fill_rect(cr_x, cr_y + FONT_HEIGHT - 2, FONT_WIDTH, 2, col);
            }
        }
    }
}

/* Click test (screen coords): index of the toolbar button under the point, or
 * -1.  Requires the BASIC window to be the topmost window there. */
static int basic_toolbar_hit(int sx, int sy)
{
    int dx = sx + vp_x, dy = sy + vp_y, i, bx, by, bw, bh;
    if (window_at_point(sx, sy) != &basic_win) return -1;
    for (i = 0; i < BAS_NBTN; i++) {
        bas_btn_rect(i, &bx, &by, &bw, &bh);
        if (dx >= bx && dx < bx + bw && dy >= by && dy < by + bh) return i;
    }
    return -1;
}

/* line queue: the editor (Enter) enqueues a logical line, basic_main + INPUT
 * dequeue it.  Decoupling the keyboard thread from the interpreter thread. */
#define BAS_QN 8
static char      bas_q[BAS_QN][256];
static int       bas_qh, bas_qt;
static semaphore bas_sem;

/* ---- screen-editor primitives (operate on the bas_ring grid) ---------- */

static int bas_linelen(int r)
{
    int n = 0; while (n < BAS_COLS && bas_ring[r][n]) n++; return n;
}
/* Highest ring row the cursor may visit: the live output row (or the whole
 * ring once it has wrapped). */
static int bas_ed_maxrow(void) { return bas_filled ? BAS_ROWS - 1 : bas_row; }

static void bas_ed_clampcol(void)
{
    int len = bas_linelen(bas_ed_row);
    if (bas_ed_col > len) bas_ed_col = len;
    if (bas_ed_col < 0)   bas_ed_col = 0;
}

static void bas_ed_putchar(char ch)        /* overwrite at the cursor, extend EOL */
{
    int len = bas_linelen(bas_ed_row);
    if (bas_ed_col >= BAS_COLS) return;
    if (bas_ed_col > len) { for (int k = len; k < bas_ed_col; k++) bas_ring[bas_ed_row][k] = ' '; len = bas_ed_col; }
    bas_ring[bas_ed_row][bas_ed_col] = ch;
    if (bas_ed_col == len) bas_ring[bas_ed_row][bas_ed_col + 1] = 0;   /* appended a char */
    if (bas_ed_col < BAS_COLS - 1) bas_ed_col++;
    /* keep the output cursor in step while editing the live bottom line so a
     * later newline doesn't truncate the freshly typed text */
    if (bas_ed_row == bas_row && bas_ed_col > bas_col) bas_col = bas_ed_col;
    bas_dirty[bas_ed_row] = 1;
}

static void bas_ed_backspace(void)         /* destructive: delete left, shift up */
{
    if (bas_ed_col <= 0) return;
    bas_ed_col--;
    int len = bas_linelen(bas_ed_row);
    for (int k = bas_ed_col; k < len; k++) bas_ring[bas_ed_row][k] = bas_ring[bas_ed_row][k + 1];
    if (bas_ed_row == bas_row && bas_col > 0) bas_col--;
    bas_dirty[bas_ed_row] = 1;
}

/* Submit the logical line under the cursor to the interpreter queue, then
 * return the cursor to the live bottom line. */
static void bas_ed_enter(void)
{
    char line[256]; int n = 0;
    for (; n < BAS_COLS && bas_ring[bas_ed_row][n]; n++) line[n] = bas_ring[bas_ed_row][n];
    while (n > 0 && line[n - 1] == ' ') n--;      /* trim trailing pad */
    line[n] = 0;

    if (bas_ed_row == bas_row) {                  /* editing the live line: scroll down */
        bas_col = bas_linelen(bas_row);
        bas_putc('\n');
    }
    bas_ed_row = bas_row; bas_ed_col = bas_col;    /* cursor home to the bottom */

    { int k = 0; for (; line[k] && k < 255; k++) bas_q[bas_qt][k] = line[k]; bas_q[bas_qt][k] = 0; }
    bas_qt = (bas_qt + 1) % BAS_QN;
    if (bas_sem != (semaphore)SYSERR) signaln(bas_sem, 1);
}

/* Route a keystroke into the editor.  Parses the ANSI arrow CSI sequences the
 * USB keyboard emits (ESC [ A/B/C/D). */
void basic_feed(int c)
{
    if (bas_esc == 1) { bas_esc = (c == '[') ? 2 : 0; return; }
    if (bas_esc == 2) {
        bas_esc = 0;
        switch (c) {
            case 'A': if (bas_ed_row > 0)               { bas_dirty[bas_ed_row] = 1; bas_ed_row--; bas_ed_clampcol(); bas_dirty[bas_ed_row] = 1; } return; /* up    */
            case 'B': if (bas_ed_row < bas_ed_maxrow()) { bas_dirty[bas_ed_row] = 1; bas_ed_row++; bas_ed_clampcol(); bas_dirty[bas_ed_row] = 1; } return; /* down  */
            case 'C': { int len = bas_linelen(bas_ed_row); if (bas_ed_col < len && bas_ed_col < BAS_COLS - 1) { bas_ed_col++; bas_dirty[bas_ed_row] = 1; } } return; /* right */
            case 'D': if (bas_ed_col > 0) { bas_ed_col--; bas_dirty[bas_ed_row] = 1; } return; /* left */
            default:  return;
        }
    }
    if (c == 0x1b) { bas_esc = 1; return; }
    if (c == '\r' || c == '\n') { bas_ed_enter(); return; }
    if (c == 8 || c == 0x7f)    { bas_ed_backspace(); return; }
    if (c >= 0x20 && c < 0x7f)  { bas_ed_putchar((char)c); return; }
}

static int basic_getline(char *buf, int max)
{
    wait(bas_sem);
    int i = 0; for (; bas_q[bas_qh][i] && i < max - 1; i++) buf[i] = bas_q[bas_qh][i]; buf[i] = 0;
    bas_qh = (bas_qh + 1) % BAS_QN; return i;
}

/* Toolbar button -> run its command: echo it (so it looks typed) and push it
 * onto the interpreter queue, exactly like pressing ENTER on that line. */
static void basic_run_button(int idx)
{
    const char *cmd;
    int k;
    if (idx < 0 || idx >= BAS_NBTN) return;
    cmd = bas_btns[idx].cmd;
    bas_emit(cmd); bas_emit("\n");
    for (k = 0; cmd[k] && k < 255; k++) bas_q[bas_qt][k] = cmd[k];
    bas_q[bas_qt][k] = 0;
    bas_qt = (bas_qt + 1) % BAS_QN;
    if (bas_sem != (semaphore)SYSERR) signaln(bas_sem, 1);
}

thread basic_main(void)
{
    char line[256];
    bas_sem = semcreate(0);
    basic_set_emit(bas_emit);
    basic_set_input(basic_getline);
    {
        extern void basic_set_cls(void (*)(int));
        extern void basic_set_plot(void (*)(int, int, int));
        extern void basic_set_pause(void (*)(int));
        extern void basic_set_line(void (*)(int, int, int, int, int));
        basic_set_cls(bas_cls);
        basic_set_plot(bas_plot);
        basic_set_pause(bas_pause);
        basic_set_line(bas_line);
    }
    basic_init();
    bas_emit("Xinu BASIC ready.  Full-screen editor:\n"
             "  type a line + ENTER to enter it; arrow keys roam the screen,\n"
             "  edit any line and press ENTER to re-register it.\n"
             "  RUN / LIST / NEW / FILES / RUN \"hello.bas\".\n");
    for (;;) {
        basic_getline(line, sizeof line);
        basic_exec_line(line);
    }
    return OK;
}

/* Route a keystroke to the active window's input (shell or BASIC). */
void gwm_feed_key(int c)
{
    extern void gwincon_feed(int);
    if (active_win == &basic_win) basic_feed(c);
    else                          gwincon_feed(c);
}

/* Restore every window to its default (boot) position and size, undoing any
 * dragging/resizing — i.e. reset the desktop to its initial screen.  Triggered
 * via the /api/wm/reset HTTP route (apps/webactor.c). */
void wm_reset_layout(void)
{
    info_win.x = 16;      info_win.y = 16;      info_win.width = 420;  info_win.height = 120;
    console_win.x = 593;  console_win.y = 424;  console_win.width = 227; console_win.height = 355;
    softkbd_win.x = 832;  softkbd_win.y = 503;  softkbd_win.width = 439; softkbd_win.height = 223;
    actor_win.x = 19;     actor_win.y = 143;    actor_win.width = 412;  actor_win.height = 270;
    graphics_win.x = 444; graphics_win.y = 15;  graphics_win.width = 374; graphics_win.height = 402;
    basic_win.x = 23;     basic_win.y = 421;    basic_win.width = 560;  basic_win.height = 360;
    sh_win.x = 824;       sh_win.y = 18;        sh_win.width = 456;     sh_win.height = 476;
    active_win = 0;
    g_need_full = 1;          /* force a full repaint at the next frame */
}

/* wm_dump_layout — snapshot the LIVE window geometry as ready-to-paste C
 * source (one assignment line per window, matching wm_reset_layout's format).
 * Lets us capture an on-screen arrangement (after mouse drag/resize) and bake
 * it back into the C initial screen.  Triggered via /api/wm/dump. */
int wm_dump_layout(char *buf, int max)
{
    static const struct { const char *name; window_t *w; } tab[] = {
        { "info_win",     &info_win     },
        { "console_win",  &console_win  },
        { "softkbd_win",  &softkbd_win  },
        { "actor_win",    &actor_win    },
        { "graphics_win", &graphics_win },
        { "basic_win",    &basic_win    },
        { "sh_win",       &sh_win       },
    };
    int n = 0, i;
    for (i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++) {
        window_t *w = tab[i].w;
        if (n >= max - 120) break;
        n += sprintf(buf + n,
                     "    %s.x = %d; %s.y = %d; %s.width = %d; %s.height = %d;\n",
                     tab[i].name, (int)w->x,
                     tab[i].name, (int)w->y,
                     tab[i].name, (int)w->width,
                     tab[i].name, (int)w->height);
    }
    buf[n] = 0;
    return n;
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
    softkbd_win.x = 832;
    softkbd_win.y = 503;
    softkbd_win.width  = 439;
    softkbd_win.height = 223;
    title_set(&softkbd_win, "Soft keyboard");
    softkbd_win.chrome_color = 0xFFFFB060U;
    softkbd_win.title_bg     = 0xFF704020U;
    softkbd_win.title_fg     = 0xFFFFFFFFU;
    softkbd_win.content_bg   = 0xFF0A0A14U;
    softkbd_win.draw_content = softkbd_draw;
    wm_add(&softkbd_win);

    /* AIPL print console: left column, below the info window and left of
     * the actor monitor (no overlap with info / actor / soft keyboard). */
    console_win.x = 593;
    console_win.y = 424;
    console_win.width  = 227;
    console_win.height = 355;
    title_set(&console_win, "AIPL console (print)");
    console_win.chrome_color = 0xFF80FF80U;
    console_win.title_bg     = 0xFF205020U;
    console_win.title_fg     = 0xFFFFFFFFU;
    console_win.content_bg   = 0xFF001000U;
    console_win.draw_content = console_draw;
    wm_add(&console_win);

    /* Actor monitor: middle column, top half. */
    actor_win.x = 19;
    actor_win.y = 143;
    actor_win.width  = 412;
    actor_win.height = 270;
    title_set(&actor_win, "AIPL actors (live)");
    actor_win.chrome_color = 0xFF60E0A0U;
    actor_win.title_bg     = 0xFF105030U;
    actor_win.title_fg     = 0xFFFFFFFFU;
    actor_win.content_bg   = 0xFF001008U;
    actor_win.draw_content = actor_draw;
    wm_add(&actor_win);

    /* Graphics window: middle column, below the actor monitor.  Shows the
     * rotating wire-frame 3-D wine glass driven by the `wine` xsh command. */
    graphics_win.x = 444;
    graphics_win.y = 15;
    graphics_win.width  = 374;
    graphics_win.height = 402;
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
    /* BASIC interpreter window.  Initial position captured from the live
     * on-screen layout (/api/wm/dump) so the desktop boots as last arranged. */
    basic_win.x = 23;     basic_win.y = 421;    basic_win.width = 560;  basic_win.height = 360;
    title_set(&basic_win, "BASIC");
    basic_win.chrome_color = 0xFF66FF99U;
    basic_win.title_bg     = 0xFF105028U;
    basic_win.title_fg     = 0xFFFFFFFFU;
    basic_win.content_bg   = 0xFF001405U;
    basic_win.draw_content = basic_draw;
    wm_add(&basic_win);

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
