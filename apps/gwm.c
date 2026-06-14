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
#include <string.h>     /* strncmp() for the AIPL window command dispatch */
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
#define WM_CLOSE_W        9          /* title-bar close (X) box, px (fits 8px glyph) */

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

/* Lazy-lift state for the content pass.  While g_paint_armed is set, the
 * gvideo pre-draw hook (gv_cursor_predraw) lifts the cursor sprite the FIRST
 * time a draw rectangle overlaps it — so a frame that never touches the
 * pointer leaves it untouched (no per-frame blink), while the continuously
 * spinning 3-D window, the WiFi badge, etc. (all elsewhere) no longer
 * disturb it.  g_cursor_lifted records whether we actually hid it. */
static volatile int g_paint_armed  = 0;
static volatile int g_cursor_lifted = 0;
static tid_typ      g_wm_tid = -1;      /* the wm_run thread; only it may lift */

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

/* gvideo pre-draw hook (installed in wm_run).  Called in SCREEN coords just
 * before a fill_rect / draw_line / glyph touches the framebuffer.  During the
 * content pass we lift the cursor sprite the first time a draw actually
 * overlaps its 12x12 box; draws that miss the pointer leave it alone, so the
 * cursor only ever "blinks" when something is genuinely redrawn underneath it
 * (correct), not on every animation frame somewhere else on screen. */
static void gv_cursor_predraw(int sx, int sy, int w, int h)
{
    /* Only the wm thread's content pass may touch the cursor backing store.
     * Other threads (e.g. the BASIC interpreter drawing CLS/LINE graphics)
     * also go through fill_rect/draw_line — they must NOT race cursor_hide()
     * here, which would corrupt the sprite patch and the animation. */
    if (thrcurrent != g_wm_tid) return;
    if (!g_paint_armed || g_cursor_lifted) return;
    if (!cursor_visible || !cursor_bak_valid) return;
    /* Overlap test against the sprite's current position (bak_x,bak_y). */
    if (sx >= bak_x + 12 || sx + w <= bak_x ||
        sy >= bak_y + 12 || sy + h <= bak_y) return;
    cursor_hide();                 /* restore the patch — no fill_rect, no recursion */
    g_cursor_lifted = 1;
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

/* Unlink a window from the manager (no-op if it isn't currently shown). */
void wm_remove(window_t *w)
{
    window_t *p;
    if (!w || !wm_head) return;
    if (wm_head == w) { wm_head = w->next; w->next = 0; return; }
    for (p = wm_head; p->next; p = p->next)
        if (p->next == w) { p->next = w->next; w->next = 0; return; }
}

static void draw_chrome(window_t *w)
{
    /* outer border */
    draw_rect(w->x, w->y, w->width, w->height, w->chrome_color);

    /* title bar background (one pixel inside the border) */
    fill_rect(w->x + 1, w->y + 1, w->width - 2, WM_TITLEBAR_H, w->title_bg);

    /* close box (X) at the left end of the title bar — click to dismiss */
    {
        int bx = w->x + 2, by = w->y + 2;
        fill_rect(bx, by, WM_CLOSE_W, WM_CLOSE_W, 0xFFC0382EU);   /* red box */
        draw_rect(bx, by, WM_CLOSE_W, WM_CLOSE_W, 0xFFFFFFFFU);   /* white edge */
        draw_string_at(bx + 1, by + 1, "x", 0xFFFFFFFFU, 0xFFC0382EU);
    }

    /* title text — to the right of the close box */
    draw_string_at(w->x + WM_CLOSE_W + 6, w->y + 2, w->title,
                   w->title_fg, w->title_bg);

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

    /* Only repaint when the displayed state actually changes (connection,
     * SSID or IP) — otherwise we churn the framebuffer every frame and would
     * lift the cursor whenever it is parked over the badge. */
    {
        unsigned char ip0[4] = {0,0,0,0}, gw0[4]; int have0 = 0;
        wifi_dhcp_diag(ip0, gw0, &have0);
        const char *ss = wifi_ssid();
        unsigned sig = (unsigned)conn * 2654435761u;
        for (const char *p = ss; *p; p++) sig = sig * 31u + (unsigned char)*p;
        sig = sig * 31u + (unsigned)have0;
        if (have0) for (int i = 0; i < 4; i++) sig = sig * 31u + ip0[i];
        static unsigned last_sig = 0; static int seeded = 0;
        if (seeded && sig == last_sig && !g_force_redraw) return;
        last_sig = sig; seeded = 1;
    }

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
#define NSHELL   2          /* GWINCON0 + GWINCON1 -> two independent shells */
#define SH_TB_H  16         /* shell toolbar height (px) — one button row     */
#define SH_BTN_H 12         /* one button's height                            */
#define SH_ROW_H 16         /* row pitch                                      */
/* Top of the shell text area, below the title bar + toolbar. */
#define SH_TXT_TOP(self) ((self)->y + WM_TITLEBAR_H + 2 + SH_TB_H)

/* All per-shell console state, one struct per windowed shell. */
struct shcon {
    char          ring[SHW_ROWS][SHW_COLS + 1];
    unsigned char row_dirty[SHW_ROWS];
    int           cur_row, cur_col, filled;
    int           esc, cr, csi_n, full_clear;
    char          csi[8];
    window_t      win;
    int           opened;
};
static struct shcon shc[NSHELL];

/* Forward decls: defined further down, used by gwin_shell_window_open_n(). */
static void wm_raise(window_t *w);
static int  g_need_full;
static window_t *window_at_point(int sx, int sy);   /* topmost window @point */
/* BASIC windows live in bui[] (full defs in the BASIC section); these
 * accessors let wm_close_window() dismiss one like a shell window. */
static int  bas_index_of(window_t *w);   /* BASIC window -> instance, else -1 */
static void bas_mark_closed(int idx);    /* mark BASIC instance idx off-screen */
static int  aipl_is_win(window_t *w);    /* 1 if w is the AIPL window */
static void aipl_mark_closed(void);      /* mark the AIPL window off-screen */

/* AIPL window state — declared early (used by wm_drag_tick / the frame loop). */
#define AIPL_ROWS 56
#define AIPL_COLS 72
#define AIPL_QN   8
struct aipl_ui {
  char ring[AIPL_ROWS][AIPL_COLS + 1];
  unsigned char dirty[AIPL_ROWS];
  int  row, col, filled, full;
  int  ed_row, ed_col;
  int  esc;                   /* CSI parser: 0 idle, 1 saw ESC, 2 saw ESC[   */
  char q[AIPL_QN][256];
  int  qh, qt;
  semaphore sem;
  window_t win;
  int  open;
  volatile int running;       /* a graphics program is live (Rotate4Lines)  */
  int  gfx;                   /* 1 = graphics mode (fill window with lines)  */
};
static struct aipl_ui aui;
static int  aipl_toolbar_hit(int sx, int sy);
static void aipl_run_button(int idx);
static void aipl_emit(const char *s);
static int  aipl_capture;       /* 1 = mirror AIPL print() into the dev window */

/* Which shell index a window pointer belongs to (0 if not a shell window). */
static int sh_index_of(window_t *self)
{
    int i;
    for (i = 0; i < NSHELL; i++) if (self == &shc[i].win) return i;
    return 0;
}

/* Like sh_index_of() but returns -1 for a non-shell window. */
static int sh_index_of_strict(window_t *self)
{
    int i;
    for (i = 0; i < NSHELL; i++) if (self == &shc[i].win) return i;
    return -1;
}

static void sh_newline(struct shcon *s)
{
    s->ring[s->cur_row][s->cur_col] = 0;
    s->cur_row = (s->cur_row + 1) % SHW_ROWS;
    s->cur_col = 0;
    s->ring[s->cur_row][0] = 0;
    if (s->cur_row == 0) s->filled = 1;
    for (int r = 0; r < SHW_ROWS; r++) s->row_dirty[r] = 1;
}

static void sh_clear(struct shcon *s)
{
    for (int r = 0; r < SHW_ROWS; r++) { s->ring[r][0] = 0; s->row_dirty[r] = 1; }
    s->cur_row = 0; s->cur_col = 0; s->filled = 0; s->full_clear = 1;
}

/* Append one character to shell @m's text ring (tiny VT100; see history). */
void gwin_shell_record_m(int m, char c)
{
    struct shcon *s;
    if (m < 0 || m >= NSHELL) return;
    s = &shc[m];

    if (s->esc == 1) { if (c == '[') { s->esc = 2; s->csi_n = 0; } else s->esc = 0; return; }
    if (s->esc == 2) {
        if ((c >= '0' && c <= '9') || c == ';') {
            if (s->csi_n < (int)sizeof s->csi - 1) s->csi[s->csi_n++] = c;
            return;
        }
        if (c >= 0x40 && c <= 0x7E) {
            s->csi[s->csi_n] = 0;
            if (c == 'J') sh_clear(s);
            s->esc = 0; s->csi_n = 0;
        }
        return;
    }
    if (c == 0x1B) { s->esc = 1; return; }
    if (s->cr) {
        s->cr = 0;
        if (c == '\n') { sh_newline(s); return; }
        s->cur_col = 0; s->row_dirty[s->cur_row] = 1;
    }
    if (c == '\r') { s->cr = 1; return; }
    if (c == '\n') { sh_newline(s); return; }
    if (c == 0x08 || c == 0x7F) {
        if (s->cur_col > 0) { s->cur_col--; s->ring[s->cur_row][s->cur_col] = 0; s->row_dirty[s->cur_row] = 1; }
        return;
    }
    if (c < 0x20) return;
    if (s->cur_col >= SHW_COLS) sh_newline(s);
    s->ring[s->cur_row][s->cur_col++] = c;
    s->ring[s->cur_row][s->cur_col] = 0;
    s->row_dirty[s->cur_row] = 1;
}
/* Back-compat: the GWINCON0 putc path (kept for any minor-0-only callers). */
void gwin_shell_record(char c) { gwin_shell_record_m(0, c); }

/* Diagnostic: dump shell 0's visible ring (the /api/wifi/shellring route). */
int gwin_shell_dump(char *buf, int max)
{
    struct shcon *s = &shc[0];
    int n = 0;
    int have = s->filled ? SHW_ROWS : s->cur_row + 1;
    int start = (s->cur_row - have + 1 + SHW_ROWS) % SHW_ROWS;
    for (int i = 0; i < have && n < max - 2; i++) {
        int r = (start + i) % SHW_ROWS;
        const char *t = s->ring[r];
        for (int k = 0; t[k] && n < max - 2; k++) buf[n++] = t[k];
        buf[n++] = '\n';
    }
    buf[n] = 0;
    return n;
}

/* ---- Shell window toolbar: clickable buttons that run a command -------- */
static int sh_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static const struct { const char *label; const char *cmd; } sh_btns[] = {
    { "ps",          "ps" },
    { "wifi on",     "wifi on" },
    { "wifi status", "wifi status" },
    { "wifi off",    "wifi off" },
    { "wine",        "wine" },
    { "clear",       "clear" },
    { "help",        "help" },
};
#define SH_NBTN ((int)(sizeof(sh_btns) / sizeof(sh_btns[0])))

/* Button i's rectangle in desktop coords, laid out left-to-right and wrapped
 * to a new row when it would overflow the window width. */
static void sh_btn_rect(window_t *self, int i, int *bx, int *by, int *bw, int *bh)
{
    int left = self->x + 4, right = self->x + self->width - 2;
    int x = left, row = 0, k;
    for (k = 0; ; k++) {
        int w = sh_slen(sh_btns[k].label) * FONT_WIDTH + 8;
        if (x + w > right && x > left) { row++; x = left; }   /* wrap */
        if (k == i) {
            *bx = x; *by = self->y + WM_TITLEBAR_H + 3 + row * SH_ROW_H;
            *bw = w; *bh = SH_BTN_H; return;
        }
        x += w + 4;
    }
}
static void sh_draw_toolbar(window_t *self)
{
    int i, bx, by, bw, bh;
    fill_rect(self->x + 1, self->y + WM_TITLEBAR_H + 2, self->width - 2, SH_TB_H, 0xFF0A1A2AU);
    for (i = 0; i < SH_NBTN; i++) {
        sh_btn_rect(self, i, &bx, &by, &bw, &bh);
        fill_rect(bx, by, bw, bh, 0xFF1E4E8EU);
        fill_rect(bx, by, bw, 1, 0xFF2E78C0U);              /* top highlight  */
        draw_string_at(bx + 4, by + 1, sh_btns[i].label, 0xFFE0ECFFU, 0xFF1E4E8EU);
    }
}

static void sh_draw(window_t *self, unsigned int frame)
{
    const int line_h = FONT_HEIGHT + 1;
    struct shcon *s = &shc[sh_index_of(self)];

    if (s->full_clear) {
        fill_rect(self->x + 1, self->y + WM_TITLEBAR_H + 2,
                  self->width - 2, self->height - WM_TITLEBAR_H - 3, self->content_bg);
        s->full_clear = 0;
    }
    if (g_force_redraw) sh_draw_toolbar(self);   /* gate: avoid bleeding on top */
    int content_h = self->height - (SH_TXT_TOP(self) - self->y) - 3;
    int max_rows = content_h / line_h;
    if (max_rows < 1) return;
    if (max_rows > SHW_ROWS) max_rows = SHW_ROWS;
    int have = s->filled ? SHW_ROWS : s->cur_row + 1;
    int rows = have < max_rows ? have : max_rows;
    int start = (s->cur_row - rows + 1 + SHW_ROWS) % SHW_ROWS;
    for (int i = 0; i < rows; i++) {
        int r = (start + i) % SHW_ROWS;
        if (g_force_redraw || s->row_dirty[r]) {
            int ry = SH_TXT_TOP(self) + i * line_h;
            fill_rect(self->x + 4, ry, self->width - 8, line_h, self->content_bg);
            draw_string_at(self->x + 4, ry, s->ring[r], 0xFFCCE0FFU, self->content_bg);
            if (!g_force_redraw) s->row_dirty[r] = 0;
        }
    }
    {
        int cr_y = SH_TXT_TOP(self) + (rows - 1) * line_h;
        int cr_x = self->x + 4 + s->cur_col * FONT_WIDTH;
        if (cr_x + FONT_WIDTH <= self->x + self->width - 4) {
            unsigned int col = ((frame >> 4) & 1) ? 0xFFFFD23CU : self->content_bg;
            fill_rect(cr_x, cr_y, FONT_WIDTH, FONT_HEIGHT, col);
        }
    }
}

/* Click test (screen coords): index of the shell toolbar button under the
 * point, or -1; *minor returns which shell window was hit. */
static int sh_toolbar_hit(int sx, int sy, int *minor)
{
    int dx = sx + vp_x, dy = sy + vp_y, i, bx, by, bw, bh, mi;
    window_t *top = window_at_point(sx, sy);
    mi = sh_index_of_strict(top);
    if (mi < 0) return -1;
    if (minor) *minor = mi;
    for (i = 0; i < SH_NBTN; i++) {
        sh_btn_rect(top, i, &bx, &by, &bw, &bh);
        if (dx >= bx && dx < bx + bw && dy >= by && dy < by + bh) return i;
    }
    return -1;
}
/* Run a toolbar command on shell @minor by feeding it into that shell's stdin
 * exactly as if typed (character by character + Enter). */
static void sh_run_button(int minor, int idx)
{
    extern void gwincon_feed(int, int);
    const char *cmd;
    int k;
    if (minor < 0 || minor >= NSHELL || idx < 0 || idx >= SH_NBTN) return;
    cmd = sh_btns[idx].cmd;
    for (k = 0; cmd[k]; k++) gwincon_feed(minor, (int)cmd[k]);
    gwincon_feed(minor, '\n');
}

/* Open (idempotently) shell window @idx and add it to the WM. */
int gwin_shell_window_open_n(int idx)
{
    struct shcon *s;
    if (idx < 0 || idx >= NSHELL) return SYSERR;
    s = &shc[idx];
    if (s->opened) { active_win = &s->win; wm_raise(&s->win); g_need_full = 1; return OK; }

    for (int r = 0; r < SHW_ROWS; r++) s->row_dirty[r] = 1;
    s->win.x = (idx == 0) ? 824 : 690;
    s->win.y = (idx == 0) ? 18  : 120;
    s->win.width  = 456;
    s->win.height = 476;
    {
        const char *t = (idx == 0) ? "Shell" : "Shell 2";
        int i;
        for (i = 0; i < WM_TITLE_MAX && t[i]; i++) s->win.title[i] = t[i];
        s->win.title[i] = 0;
    }
    s->win.chrome_color = 0xFFAACCEEU;
    s->win.title_bg     = 0xFF0040A0U;
    s->win.title_fg     = 0xFFFFFFFFU;
    s->win.content_bg   = 0xFF000010U;
    s->win.draw_content = sh_draw;
    wm_add(&s->win);
    s->opened = 1;
    active_win = &s->win; g_need_full = 1;
    return OK;
}
/* Back-compat: the `win` shell command + gwm_main open shell 0. */
int gwin_shell_window_open(void) { return gwin_shell_window_open_n(0); }

/* Close shell window @idx: unlink from the WM, reset its console ring,
 * mark it closed.  Called when its shell() returns (the user typed
 * `exit`); the supervisor below then waits for a re-open. */
static void gwin_shell_close(int idx)
{
    struct shcon *s;
    if (idx < 0 || idx >= NSHELL) return;
    s = &shc[idx];
    wm_remove(&s->win);
    if (active_win == &s->win) active_win = 0;
    s->opened = 0;
    /* wipe the console so the next session starts on a clean window */
    for (int r = 0; r < SHW_ROWS; r++) { s->ring[r][0] = 0; s->row_dirty[r] = 1; }
    s->cur_row = s->cur_col = s->filled = 0;
    s->esc = s->cr = s->csi_n = 0;
    g_need_full = 1;
}

/* Supervisor thread for a windowed shell (one per GWINCON minor).  Runs
 * shell() until the user exits, closes the window, then blocks until the
 * window is re-opened (right-click menu -> Shell) and runs a fresh shell.
 * This makes `exit` actually dismiss the window instead of leaving a dead
 * one on screen. */
thread gwin_shell_supervisor(int minor, int d0, int d1, int d2)
{
    extern thread shell(int, int, int);
    for (;;) {
        shell(d0, d1, d2);              /* returns when the user types `exit` */
        gwin_shell_close(minor);
        /* Park until the menu re-opens this window (sets opened = 1). */
        while (!shc[minor].opened) sleep(120);
    }
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

/* Topmost window whose title-bar close (X) box covers (sx,sy), or 0. */
static window_t *window_at_close(int sx, int sy)
{
    int dx = sx + vp_x, dy = sy + vp_y;
    window_t *hit = 0;
    for (window_t *w = wm_head; w; w = w->next)
        if (dx >= w->x + 2 && dx < w->x + 2 + WM_CLOSE_W &&
            dy >= w->y + 2 && dy < w->y + 2 + WM_CLOSE_W)
            hit = w;                        /* last match = on top */
    return hit;
}

/* Dismiss a window from its close box.  Shell windows are marked closed so
 * the right-click menu can re-open them (their supervisor shell keeps
 * running); other windows are simply unlinked from the WM. */
static void wm_close_window(window_t *w)
{
    int idx, bi;
    if (!w) return;
    idx = sh_index_of_strict(w);
    if (idx >= 0) shc[idx].opened = 0;
    bi = bas_index_of(w);
    if (bi >= 0) bas_mark_closed(bi);        /* allow menu to re-open it */
    if (aipl_is_win(w)) aipl_mark_closed();
    wm_remove(w);
    if (active_win == w) active_win = 0;
    g_need_full = 1;
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
static int  basic_toolbar_hit(int sx, int sy, int *inst); /* button idx @pt, or -1 */
static void basic_run_button(int inst, int idx);          /* run that button's command */

/* ---- right-click pull-down menu --------------------------------------- */
#define MENU_W  76
#define MENU_IH 15
static const char *menu_labels[] = { "Shell", "BASIC", "AIPL" };
#define NMENU ((int)(sizeof(menu_labels) / sizeof(menu_labels[0])))
static int menu_open, menu_x, menu_y;            /* desktop coords           */
static void menu_exec(int sel);                  /* defined after the windows */

/* Index of the menu item at desktop point (dx,dy), or -1. */
static int menu_hit(int dx, int dy)
{
    int rel;
    if (!menu_open) return -1;
    if (dx < menu_x || dx >= menu_x + MENU_W) return -1;
    rel = dy - (menu_y + 2);
    if (rel < 0 || rel >= NMENU * MENU_IH) return -1;
    return rel / MENU_IH;
}
static void draw_menu(void)
{
    int i, h, hot;
    if (!menu_open) return;
    h = NMENU * MENU_IH + 4;
    fill_rect(menu_x, menu_y, MENU_W, h, 0xFF202830U);
    fill_rect(menu_x, menu_y, MENU_W, 1, 0xFF4878C0U);          /* top edge */
    fill_rect(menu_x, menu_y, 1, h, 0xFF4878C0U);
    hot = menu_hit(cursor_x + vp_x, cursor_y + vp_y);           /* hovered item */
    for (i = 0; i < NMENU; i++) {
        unsigned int bg = (i == hot) ? 0xFF3A6CB8U : 0xFF202830U;
        unsigned int fg = (i == hot) ? 0xFFFFFFFFU : 0xFFE6ECF4U;
        if (i == hot)
            fill_rect(menu_x + 1, menu_y + 2 + i * MENU_IH, MENU_W - 2, MENU_IH, bg);
        draw_string_at(menu_x + 6, menu_y + 4 + i * MENU_IH, menu_labels[i], fg, bg);
    }
}

/* Process the left button each cursor poll: start / continue / end a drag. */
static void wm_drag_tick(void)
{
    static int prev_left = 0, prev_right = 0;
    int left  = usbmouse_buttons & 1;
    int right = usbmouse_buttons & 2;

    /* Right-click opens the pull-down menu at the cursor. */
    if (right && !prev_right) {
        menu_open = 1; menu_x = cursor_x + vp_x; menu_y = cursor_y + vp_y;
        g_need_full = 1; prev_right = right; prev_left = left; return;
    }
    prev_right = right;

    /* While the menu is open, a left click selects an item (or dismisses it). */
    if (left && !prev_left && menu_open) {
        int sel = menu_hit(cursor_x + vp_x, cursor_y + vp_y);
        menu_open = 0; g_need_full = 1;
        if (sel >= 0) menu_exec(sel);
        prev_left = left; return;
    }

    /* On a fresh press, a click on a title-bar close (X) box dismisses
     * that window.  Checked before drag/toolbar so the box always wins. */
    if (left && !prev_left) {
        window_t *cw = window_at_close(cursor_x, cursor_y);
        if (cw) { wm_close_window(cw); prev_left = left; return; }
    }

    /* On a fresh press, a click on a BASIC toolbar button runs its command
     * (edge-triggered so holding doesn't fire it every frame). */
    if (left && !prev_left) {
        int bi = 0, b = basic_toolbar_hit(cursor_x, cursor_y, &bi);
        if (b >= 0) { basic_run_button(bi, b); prev_left = left; return; }
    }
    /* Likewise, a click on a Shell toolbar button feeds its command to that
     * shell's stdin (ps / wifi on / wifi status / wifi off). */
    if (left && !prev_left) {
        int mi = 0, b = sh_toolbar_hit(cursor_x, cursor_y, &mi);
        if (b >= 0) { sh_run_button(mi, b); prev_left = left; return; }
    }
    /* AIPL window: toolbar button, else a click in its graphics area routes to
     * the AIPL program's Start/Stop buttons (abcl_xinu_gui_handle_click). */
    if (left && !prev_left) {
        int b = aipl_toolbar_hit(cursor_x, cursor_y);
        if (b >= 0) { aipl_run_button(b); prev_left = left; return; }
        if (aui.open && window_at_point(cursor_x, cursor_y) == &aui.win) {
            extern int abcl_xinu_gui_handle_click(int, int);
            if (abcl_xinu_gui_handle_click(cursor_x + vp_x, cursor_y + vp_y)) {
                active_win = &aui.win; wm_raise(&aui.win);
                prev_left = left; return;
            }
        }
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
    /* Install the overlap-aware lazy-lift hook: from now on the cursor is
     * only hidden when a draw genuinely lands on it, not once per frame.
     * Record our thread id so only this thread's draws lift the sprite. */
    g_wm_tid = thrcurrent;
    extern void (*gv_predraw_rect)(int, int, int, int);
    gv_predraw_rect = gv_cursor_predraw;

    for (;;) {
        if (wm_tick) wm_tick();
        /* Arm the lazy lift: the first draw that overlaps the pointer this
         * frame hides it (gv_cursor_predraw); a frame that misses it leaves
         * the sprite untouched, so the pointer no longer blinks while the
         * 3-D window spins / the WiFi badge refreshes elsewhere. */
        g_cursor_lifted = 0;
        g_paint_armed   = 1;
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

        /* Drive the AIPL actor program's ticks once per frame so the Spinner
         * actors advance their angle (they update g_lines, which aipl_draw
         * paints).  Only while the AIPL window is open + a program is live. */
        if (aui.open && aui.running) { extern void abcl_xinu_gui_tick_all(void); abcl_xinu_gui_tick_all(); }

        draw_wifi_indicator();   /* WiFi status mark, bottom-right corner */
        draw_menu();             /* right-click pull-down menu, if open    */

        /* Disarm the lazy lift.  Only re-stash + redraw the sprite if the
         * content pass actually hid it (i.e. something drew underneath it);
         * otherwise the pointer is still on screen and untouched — no blink. */
        g_paint_armed = 0;
        if (g_cursor_lifted) cursor_show();
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
    /* Static text — only paint on a full repaint, like softkbd_draw.  Avoids
     * rewriting the same glyphs every frame (framebuffer churn + would lift
     * the cursor if parked over this window). */
    if (!g_force_redraw) return;
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
    /* When the AIPL dev window ran a text program (PingPong), mirror the
     * actor print() output into that window so it shows where it was run. */
    if (aipl_capture && aui.open && !aui.gfx && s) { aipl_emit(s); aipl_emit("\n"); }
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
extern int  basic_curi(void);              /* current interpreter instance     */
extern void basic_bind_thread(int inst);   /* register this thread's instance  */
extern void basic_break_n(int inst);       /* Ctrl-C a specific window         */

#define BAS_ROWS 300        /* ~10 screens of scrollback (arrow up/down roams it) */
#define BAS_COLS 60
#define BAS_TB_H  32                /* toolbar height (px) — two button rows  */
#define BAS_BTN_H 12                /* one button's height                    */
#define BAS_ROW_H 16                /* row pitch (button + gap)               */
#define BAS_QN 8                    /* interpreter input line queue depth     */
#ifndef NBASIC
#define NBASIC 2                    /* independent BASIC windows (== basic.c)  */
#endif

/* All per-window BASIC UI state: text ring + editor cursor + graphics flag +
 * the line queue feeding this window's interpreter thread.  One struct per
 * on-screen BASIC window; the matching interpreter state lives in apps/basic.c
 * bs[].  Instance is resolved by basic_curi() (interpreter-thread hooks) or by
 * the window pointer (draw) or the active window (keystrokes). */
/* Flicker-free animation: each gfx primitive drawn since the last CLS 2 is
 * recorded here so CLS 2 can erase JUST those (in the bg colour) instead of
 * blanking the whole area each frame (the full-area dark fill was the flicker). */
#define BAS_GOPS 384
struct bas_gop { unsigned char type, color; short a, b, c, d; }; /* 1=line 2=plot 3=circle */
struct basic_ui {
    char          ring[BAS_ROWS][BAS_COLS + 1];
    unsigned char dirty[BAS_ROWS];
    int           row, col, filled, full;
    int           gfx;              /* 1 = pixel graphics (CLS/PLOT/LINE)     */
    int           ed_row, ed_col;   /* full-screen editor cursor              */
    int           prev_start;       /* last view top row (detect scroll)      */
    int           esc;              /* CSI parser: 0 idle, 1 ESC, 2 ESC[      */
    char          q[BAS_QN][256];   /* logical-line queue -> interpreter      */
    int           qh, qt;
    semaphore     sem;
    window_t      win;
    int           open;             /* 1 while this BASIC window is on screen */
    struct bas_gop gops[BAS_GOPS];  /* gfx primitives since the last CLS 2    */
    int           ngops, gops_over; /* count + "too many to track" flag       */
    struct bas_gop pgops[BAS_GOPS]; /* the PREVIOUS frame's primitives (on screen) */
    int           png, perase;      /* prev count + how many paired-erased so far  */
};
static struct basic_ui bui[NBASIC];
static void          basic_draw(window_t *self, unsigned int frame);

/* BASIC window <-> instance helpers (used by wm_close_window via accessors). */
static int bas_index_of(window_t *w)
{
    int i;
    for (i = 0; i < NBASIC; i++) if (w == &bui[i].win) return i;
    return -1;
}
static void bas_mark_closed(int idx) { if (idx >= 0 && idx < NBASIC) bui[idx].open = 0; }
/* The instance whose window is currently active (for keystroke routing); -1. */
static int bas_active_index(void)
{
    int i;
    for (i = 0; i < NBASIC; i++) if (active_win == &bui[i].win && bui[i].open) return i;
    return -1;
}

/* Open (idempotently) BASIC window @idx and add it to the WM.  Mirrors
 * gwin_shell_window_open_n(): if already shown, just raise + focus it;
 * otherwise (re)build its chrome and add it.  The basic_main_n interpreter
 * thread keeps running regardless, so its program/state survive a close. */
int basic_window_open_n(int idx)
{
    struct basic_ui *u;
    if (idx < 0 || idx >= NBASIC) return SYSERR;
    u = &bui[idx];
    if (u->open) { active_win = &u->win; wm_raise(&u->win); g_need_full = 1; return OK; }
    if (u->win.width == 0) {          /* first open: default layout (offset 2nd) */
        u->win.x = (idx == 0) ? 23  : 90;
        u->win.y = (idx == 0) ? 421 : 300;
        u->win.width = 560; u->win.height = 360;
    }
    title_set(&u->win, (idx == 0) ? "BASIC" : "BASIC 2");
    u->win.chrome_color = 0xFF66FF99U;
    u->win.title_bg     = 0xFF105028U;
    u->win.title_fg     = 0xFFFFFFFFU;
    u->win.content_bg   = 0xFF001405U;
    u->win.draw_content = basic_draw;
    wm_add(&u->win);
    u->open = 1;
    active_win = &u->win; wm_raise(&u->win); g_need_full = 1;
    return OK;
}
/* Back-compat: gwm_main + the `bas` command open BASIC 0. */
void basic_window_open(void) { basic_window_open_n(0); }

/* Top of the scrolling text / graphics area, below the title bar + toolbar. */
#define BAS_TXT_TOP(self) ((self)->y + WM_TITLEBAR_H + 2 + BAS_TB_H)
/* The graphics content rectangle (desktop coords) of BASIC window @u. */
static void bas_gfx_rect(struct basic_ui *u, int *gx0, int *gy0, int *gw, int *gh)
{
    *gx0 = u->win.x + 4;
    *gy0 = u->win.y + WM_TITLEBAR_H + 2 + BAS_TB_H;
    *gw  = u->win.width - 8;
    *gh  = u->win.height - (*gy0 - u->win.y) - 3;
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
 * It tracks the output position (u->row/u->col) while the interpreter is
 * printing, then the user can move it up to re-edit a previous line.
 * (Cursor state ed_row/ed_col/esc now lives in struct basic_ui.) */
static void bas_newline(struct basic_ui *u)
{
    u->ring[u->row][u->col] = 0;
    u->row = (u->row + 1) % BAS_ROWS; u->col = 0; u->ring[u->row][0] = 0;
    if (u->row == 0) u->filled = 1;
    for (int r = 0; r < BAS_ROWS; r++) u->dirty[r] = 1;
}
static void bas_putc(struct basic_ui *u, char c)
{
    if (u->gfx) {                /* leaving graphics mode — wipe + reset to text */
        u->gfx = 0; u->full = 1;
        u->row = 0; u->col = 0; u->filled = 0; u->ring[0][0] = 0;
    }
    if (c == '\r') return;
    if (c == '\n') { bas_newline(u); return; }
    if (c == '\t') { do { if (u->col < BAS_COLS) u->ring[u->row][u->col++] = ' '; }
                     while ((u->col % 8) && u->col < BAS_COLS);
                     u->ring[u->row][u->col] = 0; u->dirty[u->row] = 1; return; }
    if (c == 8 || c == 0x7f) { if (u->col > 0) { u->col--; u->ring[u->row][u->col] = 0; u->dirty[u->row] = 1; } return; }
    if (c < 0x20) return;
    if (u->col >= BAS_COLS) bas_newline(u);
    u->ring[u->row][u->col++] = c; u->ring[u->row][u->col] = 0; u->dirty[u->row] = 1;
}
/* Emit a string to a specific BASIC window's ring. */
static void bas_emit_u(struct basic_ui *u, const char *s)
{
    while (*s) bas_putc(u, *s++);
    /* Interpreter output lands at the bottom; keep the edit cursor there so
     * the next keystroke continues from the live line (the user can still
     * arrow up afterwards to re-edit history). */
    u->ed_row = u->row; u->ed_col = u->col;
}
/* Interpreter output hook — runs on the BASIC thread, so basic_curi() picks
 * the right window. */
static void bas_emit(const char *s) { bas_emit_u(&bui[basic_curi()], s); }

/* ---- CLS / PLOT / LINE / PAUSE: pixel graphics drawn straight into the
 * BASIC window's content area.  basic_draw() leaves the content untouched
 * while in graphics mode (bas_gfx), so what we paint here persists frame to
 * frame; a program animates by CLS + draw + PAUSE in a loop (rotate.bas). -- */
#define BAS_GFX_BG 0xFF001405U          /* the graphics-layer background colour */

/* Record a gfx primitive so CLS 2 can erase just it (flicker-free animation). */
static void bas_gop_add(struct basic_ui *u, int type, int a, int b, int c, int d, int color)
{
    if (u->ngops >= BAS_GOPS) { u->gops_over = 1; return; }
    struct bas_gop *g = &u->gops[u->ngops++];
    g->type = (unsigned char)type; g->color = (unsigned char)color;
    g->a = (short)a; g->b = (short)b; g->c = (short)c; g->d = (short)d;
}
/* ---- factored draw helpers (take an explicit colour, so the same code both
 * draws a primitive and erases it by passing the background colour). -------- */
static void bas_draw_seg(struct basic_ui *u, int x1, int y1, int x2, int y2, unsigned int col)
{
    int gx0, gy0, gw, gh;
    int dx = bas_abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -bas_abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    bas_gfx_rect(u, &gx0, &gy0, &gw, &gh);
    for (;;) {
        if (x1 >= 0 && x1 < gw && y1 >= 0 && y1 < gh)
            fill_rect(gx0 + x1, gy0 + y1, 1, 1, col);
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}
static void bas_draw_circle_col(struct basic_ui *u, int cx, int cy, int r, unsigned int col)
{
    int gx0, gy0, gw, gh, x = r, y = 0, err = 1 - r;
    bas_gfx_rect(u, &gx0, &gy0, &gw, &gh);
    if (r < 0) return;
#define BAS_PX(px, py) do { int ax = (px), ay = (py); \
        if (ax >= 0 && ax < gw && ay >= 0 && ay < gh) fill_rect(gx0 + ax, gy0 + ay, 1, 1, col); } while (0)
    while (x >= y) {
        BAS_PX(cx + x, cy + y); BAS_PX(cx - x, cy + y);
        BAS_PX(cx + x, cy - y); BAS_PX(cx - x, cy - y);
        BAS_PX(cx + y, cy + x); BAS_PX(cx - y, cy + x);
        BAS_PX(cx + y, cy - x); BAS_PX(cx - y, cy - x);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
#undef BAS_PX
}
static void bas_draw_plot_cell(struct basic_ui *u, int x, int y, int ch, unsigned int fg)
{
    int gx0, gy0, gw, gh; char s[2];
    bas_gfx_rect(u, &gx0, &gy0, &gw, &gh);
    if (x < 0 || x * FONT_WIDTH >= gw || y < 0 || y * FONT_HEIGHT >= gh) return;
    if (ch == 0)                         /* erase: blank the cell */
        fill_rect(gx0 + x * FONT_WIDTH, gy0 + y * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT, BAS_GFX_BG);
    else { s[0] = (char)ch; s[1] = 0;
        draw_string_at(gx0 + x * FONT_WIDTH, gy0 + y * FONT_HEIGHT, s, fg, BAS_GFX_BG); }
}
/* Erase one recorded primitive (redraw it in the background colour). */
static void bas_gop_erase(struct basic_ui *u, struct bas_gop *g)
{
    if (g->type == 1)      bas_draw_seg(u, g->a, g->b, g->c, g->d, BAS_GFX_BG);
    else if (g->type == 2) bas_draw_plot_cell(u, g->a, g->b, 0, 0);
    else if (g->type == 3) bas_draw_circle_col(u, g->a, g->b, g->c, BAS_GFX_BG);
}
/* Paired incremental draw step.  Compares the primitive about to be drawn with
 * the matching one from the previous frame:
 *   - identical  -> leave the on-screen pixels alone, skip the redraw (so
 *                   static scenery like a control tower never flickers);
 *   - different  -> erase the old one now, then the caller draws the new one
 *                   (the old line vanishes only as its replacement appears).
 * Returns 1 if the caller should draw, 0 if the primitive is already correct. */
static int bas_pair_step(struct basic_ui *u, int type, int a, int b, int c, int d, int color)
{
    if (u->perase < u->png) {
        struct bas_gop *g = &u->pgops[u->perase++];
        if (g->type == (unsigned char)type && g->color == (unsigned char)color &&
            g->a == (short)a && g->b == (short)b && g->c == (short)c && g->d == (short)d)
            return 0;                            /* unchanged: nothing to do */
        bas_gop_erase(u, g);                     /* changed: erase the old one */
    }
    return 1;
}
static void bas_gfx_clear_full(struct basic_ui *u)
{
    int gx0, gy0, gw, gh;
    bas_gfx_rect(u, &gx0, &gy0, &gw, &gh);
    if (gw > 0 && gh > 0) fill_rect(gx0, gy0, gw, gh, BAS_GFX_BG);
    u->ngops = 0; u->gops_over = 0; u->png = 0; u->perase = 0;
}
/* Promote the just-drawn frame to "previous" so the next frame can pair-erase
 * against it.  First erase any leftovers (the previous frame drew more
 * primitives than this one, so some old lines were never paired). */
static void bas_gfx_swap_frame(struct basic_ui *u)
{
    while (u->perase < u->png) bas_gop_erase(u, &u->pgops[u->perase++]);
    if (u->ngops > 0) memcpy(u->pgops, u->gops, (unsigned)u->ngops * sizeof(struct bas_gop));
    u->png = u->ngops; u->perase = 0;
    u->ngops = 0; u->gops_over = 0;
}
/* CLS mode: 1 = text screen, 2 = graphics screen, 3 = both. */
static void bas_cls(int mode)
{
    struct basic_ui *u = &bui[basic_curi()];
    if (mode == 3) {                         /* hard clear of the graphics layer */
        bas_gfx_clear_full(u);
    } else if (mode == 2) {                  /* soft clear: pair-erase the last frame */
        if (u->gops_over) bas_gfx_clear_full(u);   /* too many to track -> full */
        else              bas_gfx_swap_frame(u);
    }
    if (mode == 1 || mode == 3) {            /* clear the text screen */
        int r;
        for (r = 0; r < BAS_ROWS; r++) { u->ring[r][0] = 0; u->dirty[r] = 1; }
        u->row = 0; u->col = 0; u->filled = 0;
        u->ed_row = 0; u->ed_col = 0; u->full = 1;
    }
    /* show the graphics layer for a graphics clear, else the text layer */
    u->gfx = (mode == 2) ? 1 : 0;
}
/* PLOT x,y[,char]: a character at pixel cell (x,y) on the graphics screen. */
static void bas_plot(int x, int y, int ch)
{
    struct basic_ui *u = &bui[basic_curi()];
    u->gfx = 1;
    if (ch < 0x20 || ch > 0x7e) ch = '*';
    if (bas_pair_step(u, 2, x, y, ch, 0, 0)) bas_draw_plot_cell(u, x, y, ch, 0xFFB6FFB6U);
    bas_gop_add(u, 2, x, y, ch, 0, 0);
}
/* LINE(x1,y1)-(x2,y2),color: a line segment (Bresenham) on the graphics
 * screen, coordinates in content-area pixels, colour 0..15. */
static void bas_line(int x1, int y1, int x2, int y2, int color)
{
    struct basic_ui *u = &bui[basic_curi()];
    u->gfx = 1;
    if (bas_pair_step(u, 1, x1, y1, x2, y2, color)) bas_draw_seg(u, x1, y1, x2, y2, bas_palette(color));
    bas_gop_add(u, 1, x1, y1, x2, y2, color);
}
/* CIRCLE (cx,cy),r,color: outline circle (midpoint algorithm). */
static void bas_circle(int cx, int cy, int r, int color)
{
    struct basic_ui *u = &bui[basic_curi()];
    u->gfx = 1;
    if (bas_pair_step(u, 3, cx, cy, r, 0, color)) bas_draw_circle_col(u, cx, cy, r, bas_palette(color));
    bas_gop_add(u, 3, cx, cy, r, 0, color);
}
/* WIFI ON|OFF|STATUS from BASIC (mirrors the `wifi` shell command). */
static void bas_wifi(int action)
{
    extern int   wifi_connected(void);
    extern const char *wifi_ssid(void);
    extern int   wifi_join(const char *, const char *);
    extern int   wifi_dhcp(void);
    extern void  wifi_disconnect(void);
    extern void  wifi_serve_start(void);
    extern void  wifi_dhcp_diag(unsigned char *, unsigned char *, int *);
    extern int   wifi_load_cfg(char *, int, char *, int);
    char line[80];

    if (action == 0) { wifi_disconnect(); bas_emit("WiFi: disconnected\n"); return; }
    if (action == 2) {
        if (wifi_connected()) {
            unsigned char ip[4], gw[4]; int have = 0;
            bas_emit("WiFi: connected to "); bas_emit(wifi_ssid()); bas_emit("\n");
            wifi_dhcp_diag(ip, gw, &have);
            if (have) { sprintf(line, "  ip %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]); bas_emit(line); }
        } else bas_emit("WiFi: not connected\n");
        return;
    }
    /* action 1 = on: connect to the saved /microsd/wifi.txt network */
    if (wifi_connected()) { bas_emit("WiFi: already connected\n"); return; }
    {
        char ssid[40], pass[68];
        if (wifi_load_cfg(ssid, sizeof ssid, pass, sizeof pass) != 0) {
            bas_emit("WiFi: no /microsd/wifi.txt\n"); return;
        }
        bas_emit("WiFi: connecting to "); bas_emit(ssid); bas_emit(" ...\n");
        if (wifi_join(ssid, pass) != 0) { bas_emit("WiFi: join failed\n"); return; }
        if (wifi_dhcp() != 0 || !wifi_connected()) { bas_emit("WiFi: DHCP failed\n"); return; }
        wifi_serve_start();
        bas_wifi(2);
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
    { "FILES",  "files" },
    { "LIST",   "list" },
    { "CLS3",   "cls 3" },
    { "hello",  "run \"hello.bas\"" },
    { "rotate", "run \"rotate.bas\"" },
    { "koch",   "run \"koch.bas\"" },
    { "dragon", "run \"dragon.bas\"" },
    { "bubble", "run \"bubble.bas\"" },
    { "qsort",  "run \"qsort.bas\"" },
    { "hanoi",  "run \"hanoi.bas\"" },
    { "maze",   "run \"maze.bas\"" },
    { "glass",  "run \"glass.bas\"" },
    { "flight", "run \"flight.bas\"" },
    { "rescue", "run \"rescue.bas\"" },
};
#define BAS_NBTN ((int)(sizeof(bas_btns) / sizeof(bas_btns[0])))

/* Button i's rectangle in desktop coords, laid out left-to-right and wrapped
 * to a new row when it would overflow the window width. */
static void bas_btn_rect(window_t *self, int i, int *bx, int *by, int *bw, int *bh)
{
    int left = self->x + 4, right = self->x + self->width - 2;
    int x = left, row = 0, k;
    for (k = 0; ; k++) {
        int w = bas_slen(bas_btns[k].label) * FONT_WIDTH + 8;
        if (x + w > right && x > left) { row++; x = left; }   /* wrap */
        if (k == i) {
            *bx = x; *by = self->y + WM_TITLEBAR_H + 3 + row * BAS_ROW_H;
            *bw = w; *bh = BAS_BTN_H; return;
        }
        x += w + 4;
    }
}
static void basic_draw_toolbar(window_t *self)
{
    int i, bx, by, bw, bh;
    fill_rect(self->x + 1, self->y + WM_TITLEBAR_H + 2, self->width - 2, BAS_TB_H, 0xFF0A2A12U);
    for (i = 0; i < BAS_NBTN; i++) {
        bas_btn_rect(self, i, &bx, &by, &bw, &bh);
        fill_rect(bx, by, bw, bh, 0xFF1E6E38U);
        fill_rect(bx, by, bw, 1, 0xFF2EA050U);              /* top highlight  */
        draw_string_at(bx + 4, by + 1, bas_btns[i].label, 0xFFEFFFE0U, 0xFF1E6E38U);
    }
}

static void basic_draw(window_t *self, unsigned int frame)
{
    const int line_h = FONT_HEIGHT + 1;
    struct basic_ui *u = &bui[bas_index_of(self) < 0 ? 0 : bas_index_of(self)];
    if (u->full) { fill_rect(self->x + 1, self->y + WM_TITLEBAR_H + 2,
                             self->width - 2, self->height - WM_TITLEBAR_H - 3, self->content_bg); u->full = 0; }
    if (g_force_redraw) basic_draw_toolbar(self);   /* gate: avoid bleeding on top */
    /* Graphics mode: CLS/PLOT/LINE paint the content directly; leave it be. */
    if (u->gfx) return;
    int content_h = self->height - (BAS_TXT_TOP(self) - self->y) - 3;
    int max_rows = content_h / line_h; if (max_rows < 1) return; if (max_rows > BAS_ROWS) max_rows = BAS_ROWS;
    int rows, start;
    {                                    /* scrolling text console */
        int have = u->filled ? BAS_ROWS : u->row + 1;
        rows = have < max_rows ? have : max_rows;
        /* View normally anchored at the live bottom line, but follows the edit
         * cursor up into the scrollback: if the cursor sits above the visible
         * window, scroll up just enough to keep it on the top line. */
        int back = (u->row - u->ed_row + BAS_ROWS) % BAS_ROWS;  /* rows above live */
        int vbot_back = (back > rows - 1) ? (back - (rows - 1)) : 0;
        if (vbot_back > have - rows) vbot_back = have - rows;
        if (vbot_back < 0) vbot_back = 0;
        int vbot = (u->row - vbot_back + BAS_ROWS) % BAS_ROWS;
        start = (vbot - rows + 1 + BAS_ROWS) % BAS_ROWS;
    }
    /* When the view scrolls, the row->screen mapping shifts, so every visible
     * line must be repainted (not just the per-row dirty ones). */
    int force_rows = g_force_redraw || (start != u->prev_start);
    u->prev_start = start;
    for (int i = 0; i < rows; i++) {
        int r = (start + i) % BAS_ROWS;
        if (force_rows || u->dirty[r]) {
            int ry = BAS_TXT_TOP(self) + i * line_h;
            fill_rect(self->x + 4, ry, self->width - 8, line_h, self->content_bg);
            draw_string_at(self->x + 4, ry, u->ring[r], 0xFFB6FFB6U, self->content_bg);
            if (!g_force_redraw) u->dirty[r] = 0;
        }
    }
    {                                    /* blinking underline at the edit cursor */
        int i = (u->ed_row - start + BAS_ROWS) % BAS_ROWS;
        if (i >= 0 && i < rows) {
            int cr_y = BAS_TXT_TOP(self) + i * line_h;
            int cr_x = self->x + 4 + u->ed_col * FONT_WIDTH;
            if (cr_x + FONT_WIDTH <= self->x + self->width - 4) {
                unsigned int col = ((frame >> 4) & 1) ? 0xFF66FF66U : self->content_bg;
                fill_rect(cr_x, cr_y + FONT_HEIGHT - 2, FONT_WIDTH, 2, col);
            }
        }
    }
}

/* Click test (screen coords): index of the toolbar button under the point, or
 * -1.  Requires a BASIC window to be the topmost window there; *inst returns
 * which BASIC instance was hit. */
static int basic_toolbar_hit(int sx, int sy, int *inst)
{
    int dx = sx + vp_x, dy = sy + vp_y, i, bx, by, bw, bh, bi;
    window_t *top = window_at_point(sx, sy);
    bi = bas_index_of(top);
    if (bi < 0) return -1;
    if (inst) *inst = bi;
    for (i = 0; i < BAS_NBTN; i++) {
        bas_btn_rect(top, i, &bx, &by, &bw, &bh);
        if (dx >= bx && dx < bx + bw && dy >= by && dy < by + bh) return i;
    }
    return -1;
}

/* Enqueue a logical line onto window @u's interpreter queue + wake its REPL. */
static void bas_enqueue(struct basic_ui *u, const char *line)
{
    int k = 0;
    for (; line[k] && k < 255; k++) u->q[u->qt][k] = line[k];
    u->q[u->qt][k] = 0;
    u->qt = (u->qt + 1) % BAS_QN;
    if (u->sem != (semaphore)SYSERR) signaln(u->sem, 1);
}

/* ---- screen-editor primitives (operate on a window's ring grid) -------- */

static int bas_linelen(struct basic_ui *u, int r)
{
    int n = 0; while (n < BAS_COLS && u->ring[r][n]) n++; return n;
}
/* Highest ring row the cursor may visit: the live output row (or the whole
 * ring once it has wrapped). */
static int bas_ed_maxrow(struct basic_ui *u) { return u->filled ? BAS_ROWS - 1 : u->row; }

static void bas_ed_clampcol(struct basic_ui *u)
{
    int len = bas_linelen(u, u->ed_row);
    if (u->ed_col > len) u->ed_col = len;
    if (u->ed_col < 0)   u->ed_col = 0;
}

static void bas_ed_putchar(struct basic_ui *u, char ch)  /* overwrite, extend EOL */
{
    int len = bas_linelen(u, u->ed_row);
    if (u->ed_col >= BAS_COLS) return;
    if (u->ed_col > len) { for (int k = len; k < u->ed_col; k++) u->ring[u->ed_row][k] = ' '; len = u->ed_col; }
    u->ring[u->ed_row][u->ed_col] = ch;
    if (u->ed_col == len) u->ring[u->ed_row][u->ed_col + 1] = 0;   /* appended a char */
    if (u->ed_col < BAS_COLS - 1) u->ed_col++;
    /* keep the output cursor in step while editing the live bottom line so a
     * later newline doesn't truncate the freshly typed text */
    if (u->ed_row == u->row && u->ed_col > u->col) u->col = u->ed_col;
    u->dirty[u->ed_row] = 1;
}

static void bas_ed_backspace(struct basic_ui *u)         /* delete left, shift up */
{
    if (u->ed_col <= 0) return;
    u->ed_col--;
    int len = bas_linelen(u, u->ed_row);
    for (int k = u->ed_col; k < len; k++) u->ring[u->ed_row][k] = u->ring[u->ed_row][k + 1];
    if (u->ed_row == u->row && u->col > 0) u->col--;
    u->dirty[u->ed_row] = 1;
}

/* Submit the logical line under the cursor to the interpreter queue, then
 * return the cursor to the live bottom line. */
static void bas_ed_enter(struct basic_ui *u)
{
    char line[256]; int n = 0;
    for (; n < BAS_COLS && u->ring[u->ed_row][n]; n++) line[n] = u->ring[u->ed_row][n];
    while (n > 0 && line[n - 1] == ' ') n--;      /* trim trailing pad */
    line[n] = 0;

    if (u->ed_row == u->row) {                    /* editing the live line: scroll down */
        u->col = bas_linelen(u, u->row);
        bas_putc(u, '\n');
    }
    u->ed_row = u->row; u->ed_col = u->col;        /* cursor home to the bottom */
    bas_enqueue(u, line);
}

/* Route a keystroke into the active BASIC window's editor.  Parses the ANSI
 * arrow CSI sequences the USB keyboard emits (ESC [ A/B/C/D). */
void basic_feed(int c)
{
    int bi = bas_active_index();
    struct basic_ui *u;
    if (bi < 0) return;                           /* no BASIC window focused */
    u = &bui[bi];
    if (c == 3) { basic_break_n(bi); return; }    /* Ctrl-C this window       */
    if (u->esc == 1) { u->esc = (c == '[') ? 2 : 0; return; }
    if (u->esc == 2) {
        u->esc = 0;
        switch (c) {
            case 'A': if (u->ed_row > 0)                { u->dirty[u->ed_row] = 1; u->ed_row--; bas_ed_clampcol(u); u->dirty[u->ed_row] = 1; } return; /* up    */
            case 'B': if (u->ed_row < bas_ed_maxrow(u)) { u->dirty[u->ed_row] = 1; u->ed_row++; bas_ed_clampcol(u); u->dirty[u->ed_row] = 1; } return; /* down  */
            case 'C': { int len = bas_linelen(u, u->ed_row); if (u->ed_col < len && u->ed_col < BAS_COLS - 1) { u->ed_col++; u->dirty[u->ed_row] = 1; } } return; /* right */
            case 'D': if (u->ed_col > 0) { u->ed_col--; u->dirty[u->ed_row] = 1; } return; /* left */
            default:  return;
        }
    }
    if (c == 0x1b) { u->esc = 1; return; }
    if (c == '\r' || c == '\n') { bas_ed_enter(u); return; }
    if (c == 8 || c == 0x7f)    { bas_ed_backspace(u); return; }
    if (c >= 0x20 && c < 0x7f)  { bas_ed_putchar(u, (char)c); return; }
}

/* INPUT hook — runs on the BASIC thread, so basic_curi() picks the window. */
static int basic_getline(char *buf, int max)
{
    struct basic_ui *u = &bui[basic_curi()];
    wait(u->sem);
    int i = 0; for (; u->q[u->qh][i] && i < max - 1; i++) buf[i] = u->q[u->qh][i]; buf[i] = 0;
    u->qh = (u->qh + 1) % BAS_QN; return i;
}

/* Toolbar button -> run its command on window @inst: echo it (so it looks
 * typed) and push it onto that window's interpreter queue. */
static void basic_run_button(int inst, int idx)
{
    const char *cmd;
    struct basic_ui *u;
    if (inst < 0 || inst >= NBASIC || idx < 0 || idx >= BAS_NBTN) return;
    u = &bui[inst];
    cmd = bas_btns[idx].cmd;
    bas_emit_u(u, cmd); bas_emit_u(u, "\n");
    bas_enqueue(u, cmd);
}

/* ============================================================ *
 *  AIPL development window.  BASIC-style console + toolbar, but  *
 *  `run "Rotate4Lines.abcl"` launches the AIPL ACTOR program     *
 *  (apps/abcl_program.c Spinner/Controller, translated from      *
 *  abclc/Rotate4LinesXinu.abcl by abcl2c).  The 4 rotating lines *
 *  + Start/Stop buttons render into this window's graphics area  *
 *  via the abcl_xinu_gui_* hooks (offset by abcl_gui_ox/oy).     *
 * ============================================================ */
#define AIPL_TB_H  16            /* toolbar height                          */
#define AIPL_TXT_N 10            /* text-console rows (rest is graphics)    */
extern int  abcl_rotate4_init(void);
extern void abcl_rotate4_start(void);
extern void abcl_rotate4_stop(void);
extern void abcl_pingpong_init(void);
extern void abcl_xinu_gui_render(void);
extern void abcl_xinu_gui_tick_all(void);
extern int  abcl_xinu_gui_handle_click(int, int);
extern int  abcl_gui_ox, abcl_gui_oy;
extern unsigned short abcl_gui_bg;

static const char *aipl_files[] = { "Rotate4Lines.abcl", "PingPong.abcl" };
#define AIPL_NFILE ((int)(sizeof(aipl_files) / sizeof(aipl_files[0])))
static const char *aipl_src[] = {
  "// Rotate4LinesXinu.abcl  (abcl2c -> C -> kernel actors)",
  "class Spinner {",
  "  method tick() {",
  "    if (running==1) angle = angle + speed;",
  "    var c = cos(angle); var s = sin(angle);",
  "    var dx = c*radius/1024; var dy = s*radius/1024;",
  "    xinu_gui_set_line(idx, cx+dx,cy+dy, cx-dx,cy-dy, rr,gg,bb);",
  "  }",
  "}",
  "class Controller {",
  "  method init() {",
  "    s1..s4 = new Spinner(...);  register tickers;",
  "    add_button(Start); add_button(Stop);",
  "  }",
  "}",
  "var ctrl = new Controller();",
};
#define AIPL_NSRC ((int)(sizeof(aipl_src) / sizeof(aipl_src[0])))

static void aipl_draw(window_t *self, unsigned int frame);
static int  aipl_is_win(window_t *w) { return w == &aui.win; }
static void aipl_mark_closed(void) { aui.open = 0; }

static void aipl_newline(struct aipl_ui *u) {
  u->ring[u->row][u->col] = 0;
  u->row = (u->row + 1) % AIPL_ROWS; u->col = 0; u->ring[u->row][0] = 0;
  if (u->row == 0) u->filled = 1;
  for (int r = 0; r < AIPL_ROWS; r++) u->dirty[r] = 1;
}
static void aipl_putc(struct aipl_ui *u, char c) {
  if (c == '\r') return;
  if (c == '\n') { aipl_newline(u); return; }
  if (c == 8 || c == 0x7f) { if (u->col > 0) { u->col--; u->ring[u->row][u->col] = 0; u->dirty[u->row] = 1; } return; }
  if (c < 0x20) return;
  if (u->col >= AIPL_COLS) aipl_newline(u);
  u->ring[u->row][u->col++] = c; u->ring[u->row][u->col] = 0; u->dirty[u->row] = 1;
}
static void aipl_emit(const char *s) {
  while (*s) aipl_putc(&aui, *s++);
  aui.ed_row = aui.row; aui.ed_col = aui.col;
}
static void aipl_enqueue(const char *line) {
  int k = 0; for (; line[k] && k < 255; k++) aui.q[aui.qt][k] = line[k]; aui.q[aui.qt][k] = 0;
  aui.qt = (aui.qt + 1) % AIPL_QN;
  if (aui.sem != (semaphore)SYSERR) signaln(aui.sem, 1);
}
static int aipl_getline(char *buf, int max) {
  wait(aui.sem);
  int i = 0; for (; aui.q[aui.qh][i] && i < max - 1; i++) buf[i] = aui.q[aui.qh][i]; buf[i] = 0;
  aui.qh = (aui.qh + 1) % AIPL_QN; return i;
}
static int aipl_linelen(int r) { int n = 0; while (n < AIPL_COLS && aui.ring[r][n]) n++; return n; }
static int aipl_ed_maxrow(void) { return aui.filled ? AIPL_ROWS - 1 : aui.row; }
static void aipl_ed_clampcol(void) {
  int len = aipl_linelen(aui.ed_row);
  if (aui.ed_col > len) aui.ed_col = len;
  if (aui.ed_col < 0)   aui.ed_col = 0;
}
/* Editor: arrow-key cursor roam (ESC [ A/B/C/D) + append / backspace / enter. */
void aipl_feed(int c) {
  if (aui.esc == 1) { aui.esc = (c == '[') ? 2 : 0; return; }
  if (aui.esc == 2) {
    aui.esc = 0;
    switch (c) {
      case 'A': if (aui.ed_row > 0)                 { aui.dirty[aui.ed_row]=1; aui.ed_row--; aipl_ed_clampcol(); aui.dirty[aui.ed_row]=1; } return; /* up    */
      case 'B': if (aui.ed_row < aipl_ed_maxrow())  { aui.dirty[aui.ed_row]=1; aui.ed_row++; aipl_ed_clampcol(); aui.dirty[aui.ed_row]=1; } return; /* down  */
      case 'C': { int len = aipl_linelen(aui.ed_row); if (aui.ed_col < len && aui.ed_col < AIPL_COLS-1) { aui.ed_col++; aui.dirty[aui.ed_row]=1; } } return; /* right */
      case 'D': if (aui.ed_col > 0) { aui.ed_col--; aui.dirty[aui.ed_row]=1; } return; /* left */
      default:  return;
    }
  }
  if (c == 0x1b) { aui.esc = 1; return; }
  if (c == '\r' || c == '\n') {
    char line[256]; int n = 0;
    for (; n < AIPL_COLS && aui.ring[aui.ed_row][n]; n++) line[n] = aui.ring[aui.ed_row][n];
    while (n > 0 && line[n - 1] == ' ') n--; line[n] = 0;
    aui.col = aui.ed_col; aipl_putc(&aui, '\n');
    aui.ed_row = aui.row; aui.ed_col = aui.col;
    /* strip a leading "aipl> " prompt the editor returns (full-screen line) */
    { const char *pfx = "aipl> "; int i = 0; const char *s = line;
      while (pfx[i] && s[i] == pfx[i]) i++; if (pfx[i] == 0) aipl_enqueue(s + i); else aipl_enqueue(s); }
    return;
  }
  if (c == 8 || c == 0x7f) {
    if (aui.ed_col > 0) { aui.ed_col--; aui.ring[aui.ed_row][aui.ed_col] = 0;
      if (aui.ed_row == aui.row && aui.col > 0) aui.col--; aui.dirty[aui.ed_row] = 1; }
    return;
  }
  if (c >= 0x20 && c < 0x7f) {
    if (aui.ed_col < AIPL_COLS - 1) {
      aui.ring[aui.ed_row][aui.ed_col] = (char)c; aui.ed_col++; aui.ring[aui.ed_row][aui.ed_col] = 0;
      if (aui.ed_row == aui.row && aui.ed_col > aui.col) aui.col = aui.ed_col;
      aui.dirty[aui.ed_row] = 1;
    }
  }
}
static void aipl_exec_line(const char *line) {
  const char *p = line; while (*p == ' ' || *p == '\t') p++;
  if (*p == 0) { return; }
  if (0 == strncmp(p, "files", 5)) {
    int i; aui.gfx = 0; aui.full = 1; aipl_capture = 0; aipl_emit("AIPL programs:\n");
    for (i = 0; i < AIPL_NFILE; i++) { aipl_emit("  "); aipl_emit(aipl_files[i]); aipl_emit("\n"); }
  } else if (0 == strncmp(p, "list", 4) || 0 == strncmp(p, "cat", 3)) {
    int i; aui.gfx = 0; aui.full = 1; aipl_capture = 0; for (i = 0; i < AIPL_NSRC; i++) { aipl_emit(aipl_src[i]); aipl_emit("\n"); }
  } else if (0 == strncmp(p, "run", 3)) {
    /* pick the program by name (default Rotate4Lines); PingPong is text-only. */
    if (strstr(p, "Ping") || strstr(p, "ping")) {
      aipl_emit("running PingPong.abcl (abcl2c->C->actors):\n");
      aui.gfx = 0; aui.running = 0; aui.full = 1; aipl_capture = 1;
      abcl_pingpong_init();
    } else {
      aipl_emit("running Rotate4Lines.abcl (abcl2c->C->actors)...\n");
      aui.gfx = 1; aui.running = 1; aipl_capture = 0; aui.full = 1;
      { int cid = abcl_rotate4_init();
        char m[40]; int k = 0; const char *t = "controller id=";
        while (t[k]) { m[k] = t[k]; k++; }
        if (cid < 0) { m[k++]='-'; cid = -cid; }
        if (cid >= 100) m[k++] = '0' + (cid/100)%10;
        if (cid >= 10)  m[k++] = '0' + (cid/10)%10;
        m[k++] = '0' + cid%10; m[k++]='\n'; m[k]=0; aipl_emit(m); }
    }
  } else if (0 == strncmp(p, "start", 5)) {
    abcl_rotate4_start(); aui.gfx = 1; aui.running = 1; aui.full = 1; aipl_emit("started\n");
  } else if (0 == strncmp(p, "stop", 4)) {
    abcl_rotate4_stop(); aipl_emit("stopped\n");
  } else if (0 == strncmp(p, "text", 4)) {        /* leave graphics mode -> editor */
    aui.gfx = 0; aui.running = 0; aui.full = 1; aipl_capture = 0; aipl_emit("text mode\n");
  } else if (0 == strncmp(p, "help", 4)) {
    aui.gfx = 0; aui.full = 1;
    aipl_emit("AIPL commands:\n");
    aipl_emit("  files                   list the available .abcl programs\n");
    aipl_emit("  list                    show the selected program's source\n");
    aipl_emit("  run \"Rotate4Lines.abcl\"  run it: 4 lines rotate (Spinner actors)\n");
    aipl_emit("  run \"PingPong.abcl\"      run it: 2 actors trade a message (AIPL console)\n");
    aipl_emit("  start                   resume the rotating spinners\n");
    aipl_emit("  stop                    pause the rotating spinners\n");
    aipl_emit("  text                    leave graphics mode, back to the editor\n");
    aipl_emit("  help                    show this list\n");
  } else {
    aipl_emit("? unknown — try `help`\n");
  }
}
thread aipl_win_main(void) {
  char line[256];
  aui.sem = semcreate(0);
  aipl_emit("Xinu AIPL — actor-language dev window.  Type `help`.\n");
  for (;;) { aipl_getline(line, sizeof line); aipl_exec_line(line); }
  return OK;
}

static int aipl_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static const struct { const char *label, *cmd; } aipl_btns[] = {
  { "files", "files" }, { "list", "list" }, { "rotate", "run \"Rotate4Lines.abcl\"" },
  { "pingpong", "run \"PingPong.abcl\"" }, { "start", "start" }, { "stop", "stop" }, { "text", "text" },
};
#define AIPL_NBTN ((int)(sizeof(aipl_btns) / sizeof(aipl_btns[0])))
static void aipl_btn_rect(window_t *self, int i, int *bx, int *by, int *bw, int *bh) {
  int left = self->x + 4, right = self->x + self->width - 2, x = left, row = 0, k;
  for (k = 0; ; k++) {
    int w = aipl_slen(aipl_btns[k].label) * FONT_WIDTH + 8;
    if (x + w > right && x > left) { row++; x = left; }
    if (k == i) { *bx = x; *by = self->y + WM_TITLEBAR_H + 3 + row * 16; *bw = w; *bh = 12; return; }
    x += w + 4;
  }
}
static void aipl_draw_toolbar(window_t *self) {
  int i, bx, by, bw, bh;
  fill_rect(self->x + 1, self->y + WM_TITLEBAR_H + 2, self->width - 2, AIPL_TB_H, 0xFF1A0A2AU);
  for (i = 0; i < AIPL_NBTN; i++) {
    aipl_btn_rect(self, i, &bx, &by, &bw, &bh);
    fill_rect(bx, by, bw, bh, 0xFF5E2E8EU);
    fill_rect(bx, by, bw, 1, 0xFF8E5EC0U);
    draw_string_at(bx + 4, by + 1, aipl_btns[i].label, 0xFFE8D8FFU, 0xFF5E2E8EU);
  }
}
#define AIPL_TXT_TOP(self) ((self)->y + WM_TITLEBAR_H + 2 + AIPL_TB_H)
static void aipl_draw(window_t *self, unsigned int frame) {
  const int line_h = FONT_HEIGHT + 1;
  struct aipl_ui *u = &aui;
  (void)frame;
  if (u->full) {
    fill_rect(self->x + 1, self->y + WM_TITLEBAR_H + 2,
              self->width - 2, self->height - WM_TITLEBAR_H - 3, self->content_bg);
    u->full = 0;
  }
  if (g_force_redraw) aipl_draw_toolbar(self);   /* gate: avoid bleeding on top */
  int txt_top = AIPL_TXT_TOP(self);

  /* Graphics mode (Rotate4Lines): paint the rotating lines/buttons offset to
   * the window.  The render erases each line's previous position itself, so we
   * only clear the whole area ONCE (on entry, via u->full above) — no per-frame
   * full clear, which keeps the frame cheap and the mouse cursor steady. */
  if (u->gfx) {
    unsigned int bg = self->content_bg;            /* 0xAARRGGBB -> rgb565 */
    abcl_gui_bg = (unsigned short)(
        ((((bg >> 16) & 0xFF) >> 3) << 11) |
        ((((bg >> 8)  & 0xFF) >> 2) << 5)  |
        (((bg) & 0xFF) >> 3));
    abcl_gui_ox = self->x + 4;
    abcl_gui_oy = txt_top;
    abcl_xinu_gui_render();
    return;
  }

  /* Text mode: the console fills the whole content area, so the edit cursor
   * roams the full window (BASIC-style).  Scroll so the cursor stays visible. */
  int content_h = self->y + self->height - txt_top - 3;
  int max_rows = content_h / line_h;
  if (max_rows < 1) return;
  if (max_rows > AIPL_ROWS) max_rows = AIPL_ROWS;
  int have = u->filled ? AIPL_ROWS : u->row + 1;
  int rows = have < max_rows ? have : max_rows;
  int start = (u->row - rows + 1 + AIPL_ROWS) % AIPL_ROWS;   /* latest window */
  {                                       /* keep the cursor row on screen */
    int ei = (u->ed_row - start + AIPL_ROWS) % AIPL_ROWS;
    if (ei >= rows) start = (u->ed_row - rows + 1 + AIPL_ROWS) % AIPL_ROWS;
  }
  {                                       /* a scroll forces a full repaint */
    static int last_start = -1;
    if (start != last_start) { for (int r = 0; r < AIPL_ROWS; r++) u->dirty[r] = 1; last_start = start; }
  }
  for (int i = 0; i < rows; i++) {
    int r = (start + i) % AIPL_ROWS;
    if (g_force_redraw || u->dirty[r]) {
      int ry = txt_top + i * line_h;
      fill_rect(self->x + 4, ry, self->width - 8, line_h, self->content_bg);
      draw_string_at(self->x + 4, ry, u->ring[r], 0xFFD8C0FFU, self->content_bg);
      if (!g_force_redraw) u->dirty[r] = 0;
    }
  }
  {                                       /* blinking underline at the cursor */
    int ci = (u->ed_row - start + AIPL_ROWS) % AIPL_ROWS;
    if (ci >= 0 && ci < rows) {
      int cr_y = txt_top + ci * line_h;
      int cr_x = self->x + 4 + u->ed_col * FONT_WIDTH;
      if (cr_x + FONT_WIDTH <= self->x + self->width - 4) {
        unsigned int col = ((frame >> 4) & 1) ? 0xFFE0B0FFU : self->content_bg;
        fill_rect(cr_x, cr_y + FONT_HEIGHT - 2, FONT_WIDTH, 2, col);
      }
    }
  }
}
/* Open (idempotently) the AIPL window. */
int aipl_window_open(void) {
  if (aui.open) { active_win = &aui.win; wm_raise(&aui.win); g_need_full = 1; return OK; }
  for (int r = 0; r < AIPL_ROWS; r++) aui.dirty[r] = 1;
  if (aui.win.width == 0) { aui.win.x = 300; aui.win.y = 30; aui.win.width = 660; aui.win.height = 560; }
  title_set(&aui.win, "AIPL");
  aui.win.chrome_color = 0xFFB68AEEU;
  aui.win.title_bg     = 0xFF3A1060U;
  aui.win.title_fg     = 0xFFFFFFFFU;
  aui.win.content_bg   = 0xFF0A0414U;
  aui.win.draw_content = aipl_draw;
  wm_add(&aui.win);
  aui.open = 1; aui.full = 1;
  active_win = &aui.win; wm_raise(&aui.win); g_need_full = 1;
  return OK;
}
/* AIPL toolbar click test: button index under (sx,sy), or -1. */
static int aipl_toolbar_hit(int sx, int sy) {
  int dx = sx + vp_x, dy = sy + vp_y, i, bx, by, bw, bh;
  if (window_at_point(sx, sy) != &aui.win) return -1;
  for (i = 0; i < AIPL_NBTN; i++) {
    aipl_btn_rect(&aui.win, i, &bx, &by, &bw, &bh);
    if (dx >= bx && dx < bx + bw && dy >= by && dy < by + bh) return i;
  }
  return -1;
}
static void aipl_run_button(int idx) {
  if (idx < 0 || idx >= AIPL_NBTN) return;
  aipl_emit(aipl_btns[idx].cmd); aipl_emit("\n");
  aipl_enqueue(aipl_btns[idx].cmd);
}

/* Right-click menu action. */
static void menu_exec(int sel)
{
    if (sel == 0) {                              /* Shell: open a closed slot */
        int i, opened = 0;
        for (i = 0; i < NSHELL; i++)
            if (!shc[i].opened) { gwin_shell_window_open_n(i); opened = 1; break; }
        if (!opened) gwin_shell_window_open_n(0); /* all open -> raise Shell 0 */
    } else if (sel == 1) {                       /* BASIC: open a closed slot */
        int i, opened = 0;
        for (i = 0; i < NBASIC; i++)
            if (!bui[i].open) { basic_window_open_n(i); opened = 1; break; }
        if (!opened) basic_window_open_n(0);     /* all open -> raise BASIC 0 */
    } else if (sel == 2) {                       /* AIPL */
        aipl_window_open();
    }
    g_need_full = 1;
}

/* Set the interpreter hooks (global function pointers — idempotent; both
 * BASIC threads call this, the last write wins and they're identical). */
/* 1 while the current BASIC window is showing pixel graphics — do_run() uses
 * this to skip the "Ok" print (which would wipe a finished drawing). */
static int bas_gfx_active(void) { return bui[basic_curi()].gfx; }

/* Maximize (on=1) / restore (on=0) the calling thread's BASIC window — used by
 * the `debug` command so the line debugger runs full-screen.  Picks the
 * instance by thread (basic_curi), so it follows the window that ran `debug`. */
static int bas_fs_saved[NBASIC][4];
static int bas_fs_on[NBASIC];
static void bas_fullscreen(int on)
{
    int i = basic_curi();
    if (i < 0 || i >= NBASIC) return;
    struct basic_ui *u = &bui[i];
    if (on) {
        if (bas_fs_on[i]) return;
        bas_fs_saved[i][0] = u->win.x;     bas_fs_saved[i][1] = u->win.y;
        bas_fs_saved[i][2] = u->win.width; bas_fs_saved[i][3] = u->win.height;
        u->win.x = 0; u->win.y = 0;
        u->win.width  = (int)video_screen_width();
        u->win.height = (int)video_screen_height();
        bas_fs_on[i] = 1;
        active_win = &u->win; wm_raise(&u->win);
    } else {
        if (!bas_fs_on[i]) return;
        u->win.x = bas_fs_saved[i][0]; u->win.y = bas_fs_saved[i][1];
        u->win.width  = bas_fs_saved[i][2]; u->win.height = bas_fs_saved[i][3];
        bas_fs_on[i] = 0;
    }
    g_need_full = 1;
}
static void basic_install_hooks(void)
{
    extern void basic_set_cls(void (*)(int));
    extern void basic_set_plot(void (*)(int, int, int));
    extern void basic_set_pause(void (*)(int));
    extern void basic_set_line(void (*)(int, int, int, int, int));
    extern void basic_set_circle(void (*)(int, int, int, int));
    extern void basic_set_wifi(void (*)(int));
    extern void basic_set_gfx_active(int (*)(void));
    extern void basic_set_fullscreen(void (*)(int));
    basic_set_emit(bas_emit);
    basic_set_input(basic_getline);
    basic_set_cls(bas_cls);
    basic_set_plot(bas_plot);
    basic_set_pause(bas_pause);
    basic_set_line(bas_line);
    basic_set_circle(bas_circle);
    basic_set_wifi(bas_wifi);
    basic_set_gfx_active(bas_gfx_active);
    basic_set_fullscreen(bas_fullscreen);
}

/* One REPL thread per BASIC window (apps/basic.c instance @inst). */
thread basic_main_n(int inst)
{
    char line[256];
    struct basic_ui *u = &bui[inst];
    basic_bind_thread(inst);          /* so basic_curi() resolves to @inst    */
    u->sem = semcreate(0);
    basic_install_hooks();
    basic_init();                     /* inits this thread's instance (bs[inst]) */
    bas_emit_u(u, "Xinu BASIC ready.  Full-screen editor:\n"
                  "  type a line + ENTER to enter it; arrow keys roam the screen,\n"
                  "  edit any line and press ENTER to re-register it.\n"
                  "  RUN / LIST / NEW / FILES / RUN \"hello.bas\".\n");
    for (;;) {
        basic_getline(line, sizeof line);
        basic_exec_line(line);
    }
    return OK;
}
/* Back-compat entry (BASIC 0) for any caller still using the old name. */
thread basic_main(void) { return basic_main_n(0); }

/* Route a keystroke to the active window's input (shell or BASIC). */
void gwm_feed_key(int c)
{
    extern void gwincon_feed(int, int);          /* (minor, char) */
    if (bas_active_index() >= 0)        basic_feed(c);
    else if (active_win == &aui.win)    aipl_feed(c);
    else if (active_win == &shc[1].win) gwincon_feed(1, c);
    else                                gwincon_feed(0, c);
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
    bui[0].win.x = 23;    bui[0].win.y = 421;   bui[0].win.width = 560; bui[0].win.height = 360;
    shc[0].win.x = 824;   shc[0].win.y = 18;    shc[0].win.width = 456; shc[0].win.height = 476;
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
        { "basic_win",    &bui[0].win   },
        { "sh_win",       &shc[0].win   },
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
    bui[0].win.x = 23;    bui[0].win.y = 421;   bui[0].win.width = 560; bui[0].win.height = 360;
    basic_window_open();

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
