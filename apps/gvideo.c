// apps/gvideo.c — HDMI framebuffer + 8x8 text console (ported from
// xinu-rpi5 device/video/video.c for the Pi 3 / arm-rpi3 platform).
//
// On the Pi 3 the framebuffer is already initialised by screenInit()
// during boot, so video_init() does NOT issue a mailbox call — it
// latches the existing framebufferAddress / pitch globals declared in
// <framebuffer.h>.

#ifdef _XINU_PLATFORM_ARM_RPI3_

#include <gvideo.h>
#include <framebuffer.h>

/* The Pi 3 framebuffer is set up by screenInit() during boot; these
 * globals (from <framebuffer.h>) carry the base address and stride. */
extern ulong framebufferAddress;
extern int   pitch;

/* Xinu sleep() — yields the thread for `ms` milliseconds. */
extern int sleep(unsigned int ms);

static volatile unsigned char *fb_base;
static unsigned int            fb_pitch;
static unsigned int            fb_width;
static unsigned int            fb_height;
static int                     fb_ready;

/* Text-console cursor.  Named with a g_ prefix to avoid clashing with
 * the global int cursor_col / cursor_row declared in <framebuffer.h>
 * (used by the boot fb console). */
static int g_cursor_col;
static int g_cursor_row;
#define COLS  (SCREEN_WIDTH  / FONT_WIDTH)
#define ROWS  (SCREEN_HEIGHT / FONT_HEIGHT)

/* ARGB8888 packing: B in byte 0 ... A in byte 3. */
static unsigned int color_fg = 0x00FFFFFFu;  /* white */
static unsigned int color_bg = 0x00101820u;  /* near-black */

int video_init(void)
{
    /* The Pi 3 framebuffer is already live (screenInit at boot). Just
     * latch the existing globals — no mailbox call here. */
    fb_base   = (volatile unsigned char *)framebufferAddress;
    fb_pitch  = (unsigned int)pitch;
    fb_width  = DEFAULT_WIDTH;
    fb_height = DEFAULT_HEIGHT;
    fb_ready  = (framebufferAddress != 0 && pitch != 0) ? 1 : 0;

    if (!fb_ready) return -1;

    /* Clear screen to background colour. */
    for (unsigned int y = 0; y < fb_height; y++) {
        unsigned int *row = (unsigned int *)(fb_base + y * fb_pitch);
        for (unsigned int x = 0; x < fb_width; x++) row[x] = color_bg;
    }
    g_cursor_col = 0;
    g_cursor_row = 0;
    return 0;
}

int screen_ready(void) { return fb_ready; }

static void draw_glyph(int col, int row, char c)
{
    unsigned char ci = (unsigned char)c;
    if (ci < 0x20 || ci > 0x7F) ci = '?';
    const unsigned char *glyph = font8x8[ci - 0x20];

    int px = col * FONT_WIDTH;
    int py = row * FONT_HEIGHT;
    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        unsigned char bits = glyph[gy];
        unsigned int *line =
            (unsigned int *)(fb_base + (py + gy) * fb_pitch + px * 4);
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            line[gx] = (bits & (0x80 >> gx)) ? color_fg : color_bg;
        }
    }
}

static void scroll_one_row(void)
{
    /* Move every row up by FONT_HEIGHT pixels, then clear the bottom. */
    for (unsigned int y = 0; y < fb_height - FONT_HEIGHT; y++) {
        unsigned int *dst = (unsigned int *)(fb_base + y * fb_pitch);
        unsigned int *src =
            (unsigned int *)(fb_base + (y + FONT_HEIGHT) * fb_pitch);
        for (unsigned int x = 0; x < fb_width; x++) dst[x] = src[x];
    }
    for (unsigned int y = fb_height - FONT_HEIGHT; y < fb_height; y++) {
        unsigned int *row = (unsigned int *)(fb_base + y * fb_pitch);
        for (unsigned int x = 0; x < fb_width; x++) row[x] = color_bg;
    }
}

void screen_putc(char c)
{
    if (!fb_ready) return;

    if (c == '\r') { g_cursor_col = 0; return; }
    if (c == '\n') {
        g_cursor_col = 0;
        g_cursor_row++;
        if (g_cursor_row >= ROWS) {
            scroll_one_row();
            g_cursor_row = ROWS - 1;
        }
        return;
    }
    if (c == '\b') {
        if (g_cursor_col > 0) {
            g_cursor_col--;
            draw_glyph(g_cursor_col, g_cursor_row, ' ');
        }
        return;
    }
    if (g_cursor_col >= COLS) {
        g_cursor_col = 0;
        g_cursor_row++;
        if (g_cursor_row >= ROWS) {
            scroll_one_row();
            g_cursor_row = ROWS - 1;
        }
    }
    draw_glyph(g_cursor_col, g_cursor_row, c);
    g_cursor_col++;
}

void screen_puts(const char *s)
{
    while (*s) screen_putc(*s++);
}

/* ===================================================================
 * Drawing primitives for the window system (gwm.c).
 *
 * All coordinates are pixels relative to the framebuffer's top-left.
 * When fb_ready is false every primitive becomes a no-op so builds
 * without HDMI just silently skip drawing.
 * ===================================================================
 */

unsigned int video_screen_width(void)  { return fb_width;  }
unsigned int video_screen_height(void) { return fb_height; }

/* Viewport offset: subtracted from every virtual-coord input before
 * it touches the framebuffer.  All primitives also clip to
 * [0, fb_width) × [0, fb_height). */
static int view_x = 0;
static int view_y = 0;

void video_set_viewport(int x, int y) { view_x = x; view_y = y; }
int  video_viewport_x(void) { return view_x; }
int  video_viewport_y(void) { return view_y; }

/* Optional clip rectangle in SCREEN coords (applied after the viewport
 * offset).  When set, fill_rect / draw_glyph_at only touch pixels inside
 * it, so the WM can repaint just the 12x12 area under the moving cursor
 * each frame instead of the whole screen — a full-screen redraw flickers
 * badly on this uncached, MMU-off framebuffer.  Default = whole screen. */
#define CLIP_INF (1 << 28)
static int clip_x0 = 0, clip_y0 = 0;
static int clip_x1 = CLIP_INF, clip_y1 = CLIP_INF;

void video_set_clip(int x, int y, int w, int h)
{
    clip_x0 = x; clip_y0 = y;
    clip_x1 = x + w; clip_y1 = y + h;
}
void video_clear_clip(void)
{
    clip_x0 = 0; clip_y0 = 0;
    clip_x1 = CLIP_INF; clip_y1 = CLIP_INF;
}

void fill_rect(int x, int y, int w, int h, unsigned int color)
{
    if (!fb_ready) return;

    int sx = x - view_x;
    int sy = y - view_y;
    if (sx < 0) { w += sx; sx = 0; }
    if (sy < 0) { h += sy; sy = 0; }
    if (sx + w > (int)fb_width)  w = (int)fb_width  - sx;
    if (sy + h > (int)fb_height) h = (int)fb_height - sy;
    /* Intersect with the active clip rectangle. */
    if (sx < clip_x0) { w -= (clip_x0 - sx); sx = clip_x0; }
    if (sy < clip_y0) { h -= (clip_y0 - sy); sy = clip_y0; }
    if (sx + w > clip_x1) w = clip_x1 - sx;
    if (sy + h > clip_y1) h = clip_y1 - sy;
    if (w <= 0 || h <= 0) return;

    for (int dy = 0; dy < h; dy++) {
        unsigned int *row =
            (unsigned int *)(fb_base + (sy + dy) * fb_pitch + sx * 4);
        for (int dx = 0; dx < w; dx++) row[dx] = color;
    }
}

/* Single pixel honouring the viewport-adjusted clip + screen bounds. */
static void put_px(int sx, int sy, unsigned int color)
{
    if (sx < clip_x0 || sx >= clip_x1 || sy < clip_y0 || sy >= clip_y1) return;
    if (sx < 0 || sy < 0 || sx >= (int)fb_width || sy >= (int)fb_height) return;
    *(unsigned int *)(fb_base + sy * fb_pitch + sx * 4) = color;
}

/* Bresenham line in screen coords (viewport + clip applied) — used by the
 * gwm graphics window's 3-D wireframe. */
void draw_line(int x0, int y0, int x1, int y1, unsigned int color)
{
    if (!fb_ready) return;
    x0 -= view_x; y0 -= view_y; x1 -= view_x; y1 -= view_y;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = adx - ady;
    for (;;) {
        put_px(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -ady) { err -= ady; x0 += sx; }
        if (e2 <  adx) { err += adx; y0 += sy; }
    }
}

void draw_rect(int x, int y, int w, int h, unsigned int color)
{
    /* Express the four edges as 1-px-thick fill_rects — they
     * already handle viewport + clipping uniformly. */
    fill_rect(x,         y,         w, 1, color);   /* top    */
    fill_rect(x,         y + h - 1, w, 1, color);   /* bottom */
    fill_rect(x,         y,         1, h, color);   /* left   */
    fill_rect(x + w - 1, y,         1, h, color);   /* right  */
}

void draw_glyph_at(int px, int py, char c,
                   unsigned int fg, unsigned int bg)
{
    if (!fb_ready) return;
    unsigned char ci = (unsigned char)c;
    if (ci < 0x20 || ci > 0x7F) ci = '?';
    const unsigned char *glyph = font8x8[ci - 0x20];

    int sx = px - view_x;
    int sy = py - view_y;
    /* Trivial reject if the entire glyph cell is off-screen. */
    if (sx >= (int)fb_width || sy >= (int)fb_height) return;
    if (sx + FONT_WIDTH <= 0 || sy + FONT_HEIGHT <= 0) return;

    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        int rsy = sy + gy;
        if (rsy < 0 || rsy >= (int)fb_height) continue;
        if (rsy < clip_y0 || rsy >= clip_y1) continue;
        unsigned char bits = glyph[gy];
        unsigned int *line =
            (unsigned int *)(fb_base + rsy * fb_pitch);
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            int rsx = sx + gx;
            if (rsx < 0 || rsx >= (int)fb_width) continue;
            if (rsx < clip_x0 || rsx >= clip_x1) continue;
            line[rsx] = (bits & (0x80 >> gx)) ? fg : bg;
        }
    }
}

void draw_string_at(int px, int py, const char *s,
                    unsigned int fg, unsigned int bg)
{
    if (!fb_ready) return;
    while (*s) {
        draw_glyph_at(px, py, *s, fg, bg);
        px += FONT_WIDTH;
        s++;
    }
}

/* Sleep `ms` milliseconds via the Xinu syscall (yields the thread).
 * Used by wm_run() between frame redraws for animation pacing. */
void delay_ms(unsigned int ms)
{
    sleep(ms);
}

#endif /* _XINU_PLATFORM_ARM_RPI3_ */
