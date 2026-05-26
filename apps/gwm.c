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

/* Append one character to the shell text ring (mirrors the Pi5
 * shellwin_record_char): '\n' = newline/scroll, '\r' ignored,
 * '\b'/0x7F = backspace, other control chars dropped, printable
 * appended (wrapping at SHW_COLS). */
void gwin_shell_record(char c)
{
    if (c == '\r') return;
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

    /* Reset the ring: all rows empty + dirty so the first frame paints. */
    for (int r = 0; r < SHW_ROWS; r++) {
        sh_ring[r][0]  = 0;
        sh_row_dirty[r] = 1;
    }
    sh_cur_row = 0;
    sh_cur_col = 0;
    sh_filled  = 0;

    sh_win.x = 540;
    sh_win.y = 40;
    sh_win.width  = 460;
    sh_win.height = 680;
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
        "HDMI framebuffer 1024x768 (no USB input yet)",
        0xFF888888U, self->content_bg);
    draw_string_at(self->x + 8, self->y + WM_TITLEBAR_H + 42,
        "wm thread: cooperative scheduler",
        0xFF888888U, self->content_bg);
}

static void title_set(window_t *w, const char *t)
{
    int i;
    for (i = 0; i < WM_TITLE_MAX && t[i]; i++) w->title[i] = t[i];
    w->title[i] = 0;
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

    /* Soft keyboard window: lower-left, sized for the 1024-wide panel. */
    softkbd_win.x = 16;
    softkbd_win.y = 200;
    softkbd_win.width  = 640;
    softkbd_win.height = 220;
    title_set(&softkbd_win, "Soft keyboard");
    softkbd_win.chrome_color = 0xFFFFB060U;
    softkbd_win.title_bg     = 0xFF704020U;
    softkbd_win.title_fg     = 0xFFFFFFFFU;
    softkbd_win.content_bg   = 0xFF0A0A14U;
    softkbd_win.draw_content = softkbd_draw;
    wm_add(&softkbd_win);

    /* Start the cursor at the centre of the screen. */
    wm_cursor_set(sw / 2, sh / 2, 1);

    wm_run();   /* never returns */
    return OK;  /* unreachable — keeps the compiler happy */
}

#endif /* _XINU_PLATFORM_ARM_RPI3_ */
