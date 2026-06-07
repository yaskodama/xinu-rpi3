// include/gvideo.h — Pi 3 HDMI framebuffer console (ported from xinu-rpi5).
//
// On the Pi 3 (arm-rpi3) the framebuffer is already brought up by
// screenInit() during boot, so video_init() does NOT issue a mailbox
// call — it simply latches the existing framebuffer globals
// (framebufferAddress / pitch from <framebuffer.h>).  Renders text and
// the window-manager chrome into that buffer using an embedded 8x8 font.
//
// Fails gracefully: if the framebuffer was never set up, screen_ready()
// stays false and every drawing primitive becomes a no-op.

#ifndef XINU_RPI3_GVIDEO_H
#define XINU_RPI3_GVIDEO_H

/* Pi 3 panel geometry (matches DEFAULT_WIDTH / DEFAULT_HEIGHT in
 * <framebuffer.h>).  Drawing uses the runtime fb_width / fb_height, so
 * these are the panel grid only. */
#define SCREEN_WIDTH    1024
#define SCREEN_HEIGHT   768
#define SCREEN_DEPTH    32
#define FONT_WIDTH      8
#define FONT_HEIGHT     8

/* Latch the (already-initialised) framebuffer.  Returns 0 on success,
 * -1 if no framebuffer is available. */
int  video_init(void);

/* True iff video_init() succeeded and screen_putc is live. */
int  screen_ready(void);

/* Push one character to the screen console.  Honours '\n' (new line
 * + carriage return) and '\r' (column 0).  Wraps at right edge,
 * scrolls one row at the bottom. */
void screen_putc(char c);

void screen_puts(const char *s);

/* Glyph table — declared here so the font file is the only place
 * that owns the bitmap data.  96 printable chars (ASCII 0x20..0x7F),
 * 8 bytes each (one per row, MSB = leftmost pixel). */
extern const unsigned char font8x8[96][8];

/* ===== Drawing primitives (implemented in gvideo.c) ============== */

/* Live FB dimensions — only meaningful after a successful
 * video_init().  Used by gwm.c to size the desktop. */
unsigned int video_screen_width(void);
unsigned int video_screen_height(void);

/* Viewport (camera) on a larger virtual desktop.  All drawing
 * primitives below (fill_rect / draw_rect / draw_string_at /
 * draw_glyph_at) accept *virtual* desktop coordinates and apply
 * this offset + clip-to-physical-screen before writing pixels. */
void video_set_viewport(int x, int y);
int  video_viewport_x(void);
int  video_viewport_y(void);

/* Clip rectangle (screen coords) limiting where fill_rect / draw_glyph_at
 * draw.  Used by wm_run() to repaint only the area under the cursor. */
void video_set_clip(int x, int y, int w, int h);
void video_clear_clip(void);

/* Solid fill / outline rectangle at pixel (x,y), dimensions w×h. */
void fill_rect(int x, int y, int w, int h, unsigned int color);
void draw_rect(int x, int y, int w, int h, unsigned int color);

/* 8x8 glyph drawing with explicit foreground / background colour.
 * draw_string_at advances 8 pixels per character; caller wraps. */
void draw_glyph_at(int px, int py, char c,
                   unsigned int fg, unsigned int bg);
void draw_string_at(int px, int py, const char *s,
                    unsigned int fg, unsigned int bg);

/* Sleep `ms` milliseconds (Xinu sleep() — yields the thread).
 * Used for animation pacing in wm_run(). */
void delay_ms(unsigned int ms);

#endif /* XINU_RPI3_GVIDEO_H */
