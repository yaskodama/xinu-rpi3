/**
 * @file wm.c
 *
 * Mouse-driven multi-window demo for the arm-qemu (Versatile PB) platform.
 *
 * Hardware used:
 *   - ARM PrimeCell PL110 LCD controller at 0x10120000  (640x480, 16bpp)
 *   - ARM PrimeCell PL050 KMI mouse      at 0x10007000  (PS/2)
 *
 * Layout: a desktop background, a thin top status bar, three demo windows.
 * Interaction: drag a window by its title bar; click a window body to focus.
 *
 * The framebuffer lives in BSS at link time. arm-qemu boots without the
 * MMU/caches, so writes are coherent with PL110 reads.
 */

#include <stddef.h>
#include <thread.h>
#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xfs.h>
#include <aout.h>
#include <abclc.h>

/* ============================================================
 *  PL110 LCD controller
 * ============================================================ */

#define LCD_BASE   ((volatile unsigned int *)0x10120000)
#define LCD_TIM0   0   /* +0x000 */
#define LCD_TIM1   1   /* +0x004 */
#define LCD_TIM2   2   /* +0x008 */
#define LCD_TIM3   3   /* +0x00C */
#define LCD_UPBASE 4   /* +0x010 */
#define LCD_LPBASE 5   /* +0x014 */
/* QEMU's pl110_versatile (and PL111) swap LCDControl and LCDIMSC vs the
 * stock PL110: control sits at +0x018, IMSC at +0x01C. */
#define LCD_CTRL   6   /* +0x018 */
#define LCD_IMSC   7   /* +0x01C */

/* PL110's PPL field is 6 bits (max 1024 px wide); LPP is 10 bits (max 1024
 * px tall).  We use 1024x1024 (the hardware max) and rely on Cocoa's
 * zoom-to-fit to let the user grow the host window beyond that for an
 * effective 4x visual size. */
#define SCREEN_W 1024
#define SCREEN_H 1024
#define FONT_SCALE 2

/* Versatile system controller (for SYS_LOCK / SYS_CLCD). */
#define SYS_LOCK ((volatile unsigned int *)0x10000020)
#define SYS_CLCD ((volatile unsigned int *)0x10000050)

/* Framebuffer. 640*480*2 = 614,400 bytes in .bss. */
static unsigned short framebuffer[SCREEN_W * SCREEN_H]
    __attribute__((aligned(4096)));

static void lcd_init(void)
{
    /* Approximate 640x480 timing. QEMU's PL110 model only cares about PPL/LPP. */
    LCD_BASE[LCD_TIM0]   = ((SCREEN_W / 16 - 1) << 2);
    LCD_BASE[LCD_TIM1]   = (SCREEN_H - 1);
    LCD_BASE[LCD_TIM2]   = 0;
    LCD_BASE[LCD_TIM3]   = 0;
    LCD_BASE[LCD_UPBASE] = (unsigned int)(unsigned long)framebuffer;
    LCD_BASE[LCD_LPBASE] = 0;
    LCD_BASE[LCD_IMSC]   = 0;

    /* PL110 control register layout:
     *   bit 0  : LcdEn
     *   bit 3:1: BPP (4 = 16bpp)
     *   bit 5  : LcdTFT
     *   bit 11 : LcdPwr
     *
     * Enable in two steps: turn on the controller (LcdEn) first, leave PWR
     * off; then assert PWR. PL110 specs require >20ms between the two so
     * panels can come up cleanly. QEMU does not enforce the delay, but this
     * is the documented sequence. */
    LCD_BASE[LCD_CTRL] = (4u << 1) | (1u << 5) | 1u;            /* EN */
    LCD_BASE[LCD_CTRL] = (4u << 1) | (1u << 5) | (1u << 11) | 1u; /* +PWR */
}

/* PL110_VERSATILE 16bpp default is 5:6:5 with BGR channel order at the
 * pixel layer (R and B swapped vs the conventional RGB565). Swap them
 * in the encode so callers can use natural (r, g, b). */
#define RGB(r, g, b) (unsigned short)( (((b) >> 3) << 11) | (((g) >> 2) << 5) | ((r) >> 3) )

#define COL_BLACK     RGB(  0,   0,   0)
#define COL_WHITE     RGB(255, 255, 255)
#define COL_DESK      RGB( 30,  60, 110)
#define COL_TOPBAR    RGB( 15,  25,  60)
#define COL_TITLE_ON  RGB( 50,  90, 200)
#define COL_TITLE_OFF RGB(110, 110, 130)
#define COL_TITLE_FG  RGB(255, 255, 255)
#define COL_BORDER    RGB( 10,  10,  20)
#define COL_TEXT      RGB( 20,  20,  30)
#define COL_BG1       RGB(245, 245, 250)
#define COL_BG2       RGB(255, 245, 215)
#define COL_BG3       RGB(225, 245, 225)
#define COL_CURSOR    RGB(255, 230,   0)

/* ============================================================
 *  Drawing primitives
 * ============================================================ */

static inline void put_pixel(int x, int y, unsigned short c)
{
    if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H)
        framebuffer[y * SCREEN_W + x] = c;
}

/* abcl_xinu_gui.c から呼ばれる公開ラッパ (このファイル内の static
   関数を後で呼ぶため、ここでは前方宣言だけ。実体は各関数の直後に置く) */
void put_pixel_pub(int x, int y, unsigned short c);
void fill_rect_pub(int x, int y, int w, int h, unsigned short c);
void rect_outline_pub(int x, int y, int w, int h, unsigned short c);
void draw_string_pub(int x, int y, const char *s, unsigned short c);
void wm_draw_line(int x0, int y0, int x1, int y1, unsigned short c);

void put_pixel_pub(int x, int y, unsigned short c) { put_pixel(x, y, c); }

/* User render hook — called once per WM frame after windows are drawn.
 * Used by `rotlines` and similar overlays. */
typedef void (*wm_user_render_t)(void);
static wm_user_render_t g_wm_user_render = (wm_user_render_t)0;
void wm_set_user_render(wm_user_render_t fn) { g_wm_user_render = fn; }

/* User line table — populated by VM builtins (and `rotlines`-like programs)
 * via wm_user_set_line().  Re-rendered every WM frame. */
#define WM_USER_LINES_MAX 16
struct wm_user_line {
    short          x1, y1, x2, y2;
    unsigned short color;
    char           active;
};
static struct wm_user_line g_user_lines[WM_USER_LINES_MAX];

void wm_user_set_line(int idx, int x1, int y1, int x2, int y2,
                      unsigned short color)
{
    if (idx < 0 || idx >= WM_USER_LINES_MAX) return;
    g_user_lines[idx].x1     = (short)x1;
    g_user_lines[idx].y1     = (short)y1;
    g_user_lines[idx].x2     = (short)x2;
    g_user_lines[idx].y2     = (short)y2;
    g_user_lines[idx].color  = color;
    g_user_lines[idx].active = 1;
}

void wm_user_clear_lines(void)
{
    int i;
    for (i = 0; i < WM_USER_LINES_MAX; i++) g_user_lines[i].active = 0;
}

static void wm_user_render_lines(void)
{
    int i;
    for (i = 0; i < WM_USER_LINES_MAX; i++)
        if (g_user_lines[i].active)
            wm_draw_line(g_user_lines[i].x1, g_user_lines[i].y1,
                         g_user_lines[i].x2, g_user_lines[i].y2,
                         g_user_lines[i].color);
}

void wm_user_render_enable(int on)
{
    if (on) {
        g_wm_user_render = wm_user_render_lines;
    } else {
        g_wm_user_render = (wm_user_render_t)0;
        wm_user_clear_lines();
    }
}

/* Bresenham line. */
void wm_draw_line(int x0, int y0, int x1, int y1, unsigned short c)
{
    int dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy, e2;
    for (;;)
    {
        put_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void fill_rect(int x, int y, int w, int h, unsigned short c)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > SCREEN_W) x1 = SCREEN_W;
    int y1 = y + h; if (y1 > SCREEN_H) y1 = SCREEN_H;
    int xx, yy;
    for (yy = y0; yy < y1; yy++) {
        unsigned short *row = &framebuffer[yy * SCREEN_W];
        for (xx = x0; xx < x1; xx++)
            row[xx] = c;
    }
}

static void rect_outline(int x, int y, int w, int h, unsigned short c)
{
    fill_rect(x,         y,         w, 1, c);
    fill_rect(x,         y + h - 1, w, 1, c);
    fill_rect(x,         y,         1, h, c);
    fill_rect(x + w - 1, y,         1, h, c);
}

void fill_rect_pub(int x, int y, int w, int h, unsigned short c) { fill_rect(x, y, w, h, c); }
void rect_outline_pub(int x, int y, int w, int h, unsigned short c) { rect_outline(x, y, w, h, c); }

/* 8x8 ASCII font for printable chars 0x20..0x7F. Public-domain pixel design. */
static const unsigned char font8x8[96][8] = {
    {0,0,0,0,0,0,0,0},                                     /* SP */
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},             /* !  */
    {0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00},             /* "  */
    {0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x00,0x00},             /* #  */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},             /* $  */
    {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00},             /* %  */
    {0x3C,0x66,0x3C,0x38,0x67,0x66,0x3F,0x00},             /* &  */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},             /* '  */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},             /* (  */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},             /* )  */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},             /* *  */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},             /* +  */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},             /* ,  */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},             /* -  */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},             /* .  */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},             /* /  */
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},             /* 0  */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},             /* 1  */
    {0x3C,0x66,0x06,0x1C,0x30,0x60,0x7E,0x00},             /* 2  */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},             /* 3  */
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00},             /* 4  */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},             /* 5  */
    {0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00},             /* 6  */
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},             /* 7  */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},             /* 8  */
    {0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00},             /* 9  */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},             /* :  */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},             /* ;  */
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},             /* <  */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},             /* =  */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},             /* >  */
    {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00},             /* ?  */
    {0x3C,0x66,0x6E,0x6E,0x60,0x60,0x3E,0x00},             /* @  */
    {0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},             /* A  */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},             /* B  */
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},             /* C  */
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},             /* D  */
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00},             /* E  */
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00},             /* F  */
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00},             /* G  */
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},             /* H  */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},             /* I  */
    {0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0x00},             /* J  */
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},             /* K  */
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},             /* L  */
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},             /* M  */
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00},             /* N  */
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},             /* O  */
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00},             /* P  */
    {0x3C,0x66,0x66,0x66,0x66,0x6C,0x36,0x00},             /* Q  */
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00},             /* R  */
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},             /* S  */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},             /* T  */
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},             /* U  */
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},             /* V  */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},             /* W  */
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},             /* X  */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},             /* Y  */
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00},             /* Z  */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},             /* [  */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},             /* \  */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},             /* ]  */
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00},             /* ^  */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE},             /* _  */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},             /* `  */
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},             /* a  */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00},             /* b  */
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00},             /* c  */
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00},             /* d  */
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},             /* e  */
    {0x1C,0x36,0x30,0x78,0x30,0x30,0x30,0x00},             /* f  */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C},             /* g  */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00},             /* h  */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},             /* i  */
    {0x06,0x00,0x06,0x06,0x06,0x06,0x66,0x3C},             /* j  */
    {0x60,0x60,0x6C,0x78,0x70,0x78,0x6C,0x00},             /* k  */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},             /* l  */
    {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00},             /* m  */
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},             /* n  */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},             /* o  */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},             /* p  */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},             /* q  */
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00},             /* r  */
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},             /* s  */
    {0x30,0x30,0x78,0x30,0x30,0x36,0x1C,0x00},             /* t  */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},             /* u  */
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},             /* v  */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},             /* w  */
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},             /* x  */
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C},             /* y  */
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00},             /* z  */
    {0x0C,0x18,0x18,0x70,0x18,0x18,0x0C,0x00},             /* {  */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},             /* |  */
    {0x30,0x18,0x18,0x0E,0x18,0x18,0x30,0x00},             /* }  */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},             /* ~  */
    {0,0,0,0,0,0,0,0},                                     /* DEL */
};

static void draw_char(int x, int y, char c, unsigned short fg)
{
    unsigned char ch = (unsigned char)c;
    if (ch < 0x20 || ch > 0x7F) ch = '?';
    const unsigned char *g = font8x8[ch - 0x20];
    int i, j;
    for (i = 0; i < 8; i++) {
        unsigned char b = g[i];
        for (j = 0; j < 8; j++) {
            if (b & (0x80 >> j))
                fill_rect(x + j * FONT_SCALE, y + i * FONT_SCALE,
                          FONT_SCALE, FONT_SCALE, fg);
        }
    }
}

static void draw_string(int x, int y, const char *s, unsigned short fg)
{
    while (*s) {
        draw_char(x, y, *s, fg);
        x += 8 * FONT_SCALE;
        s++;
    }
}

void draw_string_pub(int x, int y, const char *s, unsigned short c) { draw_string(x, y, s, c); }
void draw_char_pub(int x, int y, char c, unsigned short fg) { draw_char(x, y, c, fg); }

#define CHAR_W (8 * FONT_SCALE)
#define CHAR_H (8 * FONT_SCALE)

/* ============================================================
 *  PL050 PS/2 — shared register layout
 * ============================================================ */

#define KMI_MOUSE  ((volatile unsigned int *)0x10007000)
#define KMI_KBD    ((volatile unsigned int *)0x10006000)
#define KMI_CR     0
#define KMI_STAT   1
#define KMI_DATA   2
#define KMI_CLKDIV 3
#define KMI_IIR    4

/* PL050 status bits (matches QEMU hw/input/pl050.c) */
#define KMI_TXEMPTY  0x40
#define KMI_TXBUSY   0x20
#define KMI_RXFULL   0x10
#define KMI_RXBUSY   0x08

#define KMI_CR_EN    (1u << 2)

static int kmi_has_data(volatile unsigned int *kmi)
{
    return kmi[KMI_STAT] & KMI_RXFULL;
}

static void kmi_send(volatile unsigned int *kmi, unsigned char b)
{
    while (kmi[KMI_STAT] & KMI_TXBUSY) { /* spin */ }
    kmi[KMI_DATA] = b;
}

static void mouse_init(void)
{
    KMI_MOUSE[KMI_CR] = KMI_CR_EN;
    /* Enable data reporting (mouse responds with 0xFA ACK, then streams). */
    kmi_send(KMI_MOUSE, 0xF4);
    int spin;
    for (spin = 0; spin < 200000 && !kmi_has_data(KMI_MOUSE); spin++) { }
    while (kmi_has_data(KMI_MOUSE)) (void)KMI_MOUSE[KMI_DATA];
}

static int packet_idx;
static unsigned char packet_buf[3];

/* Pull as many complete 3-byte packets as available; report only the
 * cumulative delta and the latest button state. */
static int mouse_drain(int *dx_out, int *dy_out, unsigned char *btn_out)
{
    int dx = 0, dy = 0;
    int got = 0;
    unsigned char btn = 0;

    while (kmi_has_data(KMI_MOUSE)) {
        unsigned char b = KMI_MOUSE[KMI_DATA] & 0xFF;
        if (packet_idx == 0 && !(b & 0x08)) continue; /* sync */
        packet_buf[packet_idx++] = b;
        if (packet_idx == 3) {
            packet_idx = 0;
            unsigned char f = packet_buf[0];
            int x = packet_buf[1];
            int y = packet_buf[2];
            if (f & 0x10) x |= ~0xFF;
            if (f & 0x20) y |= ~0xFF;
            dx += x;
            dy -= y;            /* PS/2 +Y is up; screen +Y is down */
            btn = f & 0x07;
            got = 1;
        }
    }
    if (got) { *dx_out = dx; *dy_out = dy; *btn_out = btn; }
    return got;
}

/* ============================================================
 *  PL050 PS/2 keyboard (Set 2 scancodes, US layout)
 * ============================================================ */

/* Scan-code -> ASCII tables. Indexed by Set 2 make-code. */
static const char sc_lower[256] = {
    [0x1C]='a', [0x32]='b', [0x21]='c', [0x23]='d',
    [0x24]='e', [0x2B]='f', [0x34]='g', [0x33]='h',
    [0x43]='i', [0x3B]='j', [0x42]='k', [0x4B]='l',
    [0x3A]='m', [0x31]='n', [0x44]='o', [0x4D]='p',
    [0x15]='q', [0x2D]='r', [0x1B]='s', [0x2C]='t',
    [0x3C]='u', [0x2A]='v', [0x1D]='w', [0x22]='x',
    [0x35]='y', [0x1A]='z',
    [0x16]='1', [0x1E]='2', [0x26]='3', [0x25]='4',
    [0x2E]='5', [0x36]='6', [0x3D]='7', [0x3E]='8',
    [0x46]='9', [0x45]='0',
    [0x29]=' ',  [0x66]='\b', [0x5A]='\n',
    [0x4E]='-',  [0x55]='=',
    [0x54]='[',  [0x5B]=']',  [0x5D]='\\',
    [0x4C]=';',  [0x52]='\'',
    [0x41]=',',  [0x49]='.',  [0x4A]='/',
};
static const char sc_upper[256] = {
    [0x1C]='A', [0x32]='B', [0x21]='C', [0x23]='D',
    [0x24]='E', [0x2B]='F', [0x34]='G', [0x33]='H',
    [0x43]='I', [0x3B]='J', [0x42]='K', [0x4B]='L',
    [0x3A]='M', [0x31]='N', [0x44]='O', [0x4D]='P',
    [0x15]='Q', [0x2D]='R', [0x1B]='S', [0x2C]='T',
    [0x3C]='U', [0x2A]='V', [0x1D]='W', [0x22]='X',
    [0x35]='Y', [0x1A]='Z',
    [0x16]='!', [0x1E]='@', [0x26]='#', [0x25]='$',
    [0x2E]='%', [0x36]='^', [0x3D]='&', [0x3E]='*',
    [0x46]='(', [0x45]=')',
    [0x29]=' ',  [0x66]='\b', [0x5A]='\n',
    [0x4E]='_',  [0x55]='+',
    [0x54]='{',  [0x5B]='}',  [0x5D]='|',
    [0x4C]=':',  [0x52]='"',
    [0x41]='<',  [0x49]='>',  [0x4A]='?',
};

static int kbd_release_pending;
static int kbd_extended;
static int kbd_shift;
static int kbd_ctrl;          /* PS/2 Ctrl modifier (left or right) */

static void kbd_init(void)
{
    KMI_KBD[KMI_CR] = KMI_CR_EN;
    kmi_send(KMI_KBD, 0xF4);    /* enable scanning */
    int spin;
    for (spin = 0; spin < 200000 && !kmi_has_data(KMI_KBD); spin++) { }
    while (kmi_has_data(KMI_KBD)) (void)KMI_KBD[KMI_DATA];
}

/* Pull all queued bytes; return last key produced (0 if none).  Return
 * values 1..127 are printable ASCII or control characters (Ctrl-A=0x01
 * through Ctrl-Z=0x1A, Backspace=0x08, Enter=0x0A).  Extended PS/2
 * scancodes (arrow keys etc.) are remapped to their Emacs/readline
 * control-char equivalents so the input editor handles both paths
 * uniformly:
 *
 *      Up    -> Ctrl-P (0x10)
 *      Down  -> Ctrl-N (0x0E)
 *      Left  -> Ctrl-B (0x02)
 *      Right -> Ctrl-F (0x06)
 *      Home  -> Ctrl-A (0x01)
 *      End   -> Ctrl-E (0x05)
 *
 * Ctrl+letter is also detected (left or right Ctrl modifier) so users
 * who prefer Ctrl-P over the Up arrow get the same code.
 */
static int kbd_drain_char(void)
{
    int last = 0;
    while (kmi_has_data(KMI_KBD)) {
        unsigned char b = KMI_KBD[KMI_DATA] & 0xFF;
        if (b == 0xE0) { kbd_extended = 1; continue; }
        if (b == 0xF0) { kbd_release_pending = 1; continue; }

        if (kbd_release_pending) {
            if (!kbd_extended && (b == 0x12 || b == 0x59))
                kbd_shift = 0;
            if (b == 0x14)
                kbd_ctrl = 0;     /* L-Ctrl or R-Ctrl release */
            kbd_release_pending = 0;
            kbd_extended = 0;
            continue;
        }
        /* Shift press */
        if (!kbd_extended && (b == 0x12 || b == 0x59)) {
            kbd_shift = 1;
            continue;
        }
        /* Ctrl press (left or right) */
        if (b == 0x14) {
            kbd_ctrl = 1;
            kbd_extended = 0;
            continue;
        }
        /* Extended-key press (E0-prefixed): arrows, home, end */
        if (kbd_extended) {
            kbd_extended = 0;
            switch (b) {
                case 0x75: last = 0x10; break;  /* Up    -> Ctrl-P */
                case 0x72: last = 0x0E; break;  /* Down  -> Ctrl-N */
                case 0x6B: last = 0x02; break;  /* Left  -> Ctrl-B */
                case 0x74: last = 0x06; break;  /* Right -> Ctrl-F */
                case 0x6C: last = 0x01; break;  /* Home  -> Ctrl-A */
                case 0x69: last = 0x05; break;  /* End   -> Ctrl-E */
                default:   break;               /* drop */
            }
            continue;
        }
        /* Backspace key (Set 2 scancode 0x66) */
        if (b == 0x66) { last = 0x08; continue; }

        {
            char c = kbd_shift ? sc_upper[b] : sc_lower[b];
            if (c) {
                /* Ctrl + a..z (or A..Z) -> 0x01..0x1A */
                if (kbd_ctrl) {
                    if (c >= 'a' && c <= 'z') c = c - ('a' - 1);
                    else if (c >= 'A' && c <= 'Z') c = c - ('A' - 1);
                }
                last = c;
            }
        }
    }
    return last;
}

/* ============================================================
 *  Cursor (12x18 arrow drawn in software)
 * ============================================================ */

static int cursor_x = SCREEN_W / 2;
static int cursor_y = SCREEN_H / 2;
static unsigned char last_btn;

#define CURSOR_SCALE 2

static void draw_cursor(void)
{
    static const char *cur[] = {
        "X.........",
        "XX........",
        "XFX.......",
        "XFFX......",
        "XFFFX.....",
        "XFFFFX....",
        "XFFFFFX...",
        "XFFFFFFX..",
        "XFFFFFFFX.",
        "XFFFFFFFFX",
        "XFFFFFXXXX",
        "XFFXFFX...",
        "XFX.XFFX..",
        "XX..XFFX..",
        "X....XFFX.",
        "......XFFX",
        ".......XX.",
    };
    int rows = sizeof(cur) / sizeof(cur[0]);
    int r, c;
    for (r = 0; r < rows; r++) {
        for (c = 0; cur[r][c]; c++) {
            char k = cur[r][c];
            if (k == 'X')
                fill_rect(cursor_x + c * CURSOR_SCALE,
                          cursor_y + r * CURSOR_SCALE,
                          CURSOR_SCALE, CURSOR_SCALE, COL_BLACK);
            else if (k == 'F')
                fill_rect(cursor_x + c * CURSOR_SCALE,
                          cursor_y + r * CURSOR_SCALE,
                          CURSOR_SCALE, CURSOR_SCALE, COL_CURSOR);
        }
    }
}

/* ============================================================
 *  Window manager
 * ============================================================ */

#define MAX_WINDOWS 12
#define TITLE_H     (10 * FONT_SCALE + 4)
#define TOPBAR_H    (8 * FONT_SCALE + 4)

struct window {
    int x, y, w, h;
    const char *title;
    const char *body1;
    const char *body2;
    /* If non-NULL, render as a wrap-and-scroll text buffer of length
     * `text_len` instead of body1/body2. */
    const char *text;
    int        *text_len;
    unsigned short bg;
    int z;
    int visible;
};

static struct window winlist[MAX_WINDOWS];
static int nwin;
static int focused = -1;

static int dragging;
static int drag_target = -1;
static int drag_off_x, drag_off_y;

static int wm_add(int x, int y, int w, int h,
                  const char *title, const char *b1, const char *b2,
                  unsigned short bg)
{
    int i = nwin++;
    winlist[i].x = x; winlist[i].y = y;
    winlist[i].w = w; winlist[i].h = h;
    winlist[i].title = title;
    winlist[i].body1 = b1;
    winlist[i].body2 = b2;
    winlist[i].text = NULL;
    winlist[i].text_len = NULL;
    winlist[i].bg = bg;
    winlist[i].z = i;
    winlist[i].visible = 1;
    return i;
}

static int wm_add_text(int x, int y, int w, int h, const char *title,
                       const char *text, int *text_len, unsigned short bg)
{
    int i = wm_add(x, y, w, h, title, NULL, NULL, bg);
    winlist[i].text = text;
    winlist[i].text_len = text_len;
    return i;
}

static int wm_hit(int x, int y)
{
    int top = -1, top_z = -1, i;
    for (i = 0; i < nwin; i++) {
        struct window *w = &winlist[i];
        if (!w->visible) continue;
        if (x >= w->x && x < w->x + w->w &&
            y >= w->y && y < w->y + w->h && w->z > top_z) {
            top = i; top_z = w->z;
        }
    }
    return top;
}

static int in_titlebar(int i, int x, int y)
{
    struct window *w = &winlist[i];
    return (x >= w->x && x < w->x + w->w &&
            y >= w->y && y < w->y + TITLE_H);
}

static void bring_to_front(int idx)
{
    int max_z = -1, i;
    for (i = 0; i < nwin; i++)
        if (winlist[i].z > max_z) max_z = winlist[i].z;
    if (winlist[idx].z != max_z)
        winlist[idx].z = max_z + 1;
    focused = idx;
}

/* Draw the text buffer with wrap-on-width and \n handling. Show only
 * the lines that fit; on overflow, show the most recent ones. */
/* When >= 0, render the caret here (offset into the text buffer)
 * instead of at end-of-text.  Used by the input editor to show the
 * cursor mid-line during left/right movement. */
static int g_input_caret = -1;

static void draw_text_buffer(int x0, int y0, int w_px, int h_px,
                             const char *text, int len)
{
    int cols = (w_px - 16) / CHAR_W;
    int rows = (h_px - 16) / CHAR_H;
    if (cols <= 0 || rows <= 0) return;

    /* First pass: walk text and assign each character a (line, col).
     * Track line breaks at \n or wrap at `cols`. We only need positions
     * for the last `rows` logical lines. Since the buffer is small
     * (<=2048), do the trivial O(N) pass. */

    /* Step 1: count total lines + record caret position if overridden. */
    int line = 0, col = 0;
    int i;
    int caret_l = -1, caret_c = -1;
    int caret_off = (g_input_caret >= 0 && g_input_caret <= len)
                    ? g_input_caret : -1;
    for (i = 0; i < len; i++) {
        if (caret_off == i) { caret_l = line; caret_c = col; }
        char c = text[i];
        if (c == '\n') { line++; col = 0; }
        else {
            if (col >= cols) { line++; col = 0; }
            col++;
        }
    }
    if (caret_off == len) { caret_l = line; caret_c = col; }
    int total_lines = line + 1;
    int first_line = total_lines > rows ? total_lines - rows : 0;

    /* Step 2: render, skipping until first_line. */
    line = 0; col = 0;
    int draw_x = x0 + 8, draw_y = y0;
    int rendered_lines = 0;
    for (i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n') {
            line++; col = 0;
            continue;
        }
        if (col >= cols) { line++; col = 0; }
        if (line >= first_line) {
            int rx = x0 + 8 + col * CHAR_W;
            int ry = y0 + (line - first_line) * CHAR_H;
            draw_char(rx, ry, c, COL_TEXT);
        }
        col++;
        (void)draw_x; (void)draw_y; (void)rendered_lines;
    }

    /* Step 3: caret — either at the override offset (mid-line editing)
     *           or end-of-text (default). */
    int cret_line, cret_col;
    if (caret_l >= 0) { cret_line = caret_l; cret_col = caret_c; }
    else              { cret_line = line;    cret_col = col;    }
    cret_line -= first_line;
    if (cret_line >= 0 && cret_line < rows) {
        int cx = x0 + 8 + cret_col * CHAR_W;
        int cy = y0 + cret_line * CHAR_H;
        /* Thicker, visible block when the caret is mid-line. */
        int w = (g_input_caret >= 0 && caret_l >= 0)
                ? FONT_SCALE * 2 : FONT_SCALE;
        fill_rect(cx, cy, w, CHAR_H, COL_TEXT);
    }
}

/* Console window — renders the wmcon cell grid as a sub-body of a
 * regular window.  Implementation at the bottom of this file. */
#include <wmcon.h>
static int g_console_win_idx = -1;
static void render_console_body(int x, int y, int w, int h);

static void draw_window(int idx)
{
    struct window *w = &winlist[idx];
    /* Body */
    fill_rect(w->x, w->y + TITLE_H, w->w, w->h - TITLE_H, w->bg);
    /* Title bar */
    unsigned short tb = (idx == focused) ? COL_TITLE_ON : COL_TITLE_OFF;
    fill_rect(w->x, w->y, w->w, TITLE_H, tb);
    /* Decorations */
    rect_outline(w->x, w->y, w->w, w->h, COL_BORDER);
    fill_rect(w->x, w->y + TITLE_H - 1, w->w, 1, COL_BORDER);
    /* Title text */
    draw_string(w->x + 8, w->y + 4, w->title, COL_TITLE_FG);
    /* Body */
    int by = w->y + TITLE_H + 8;
    if (w->text) {
        draw_text_buffer(w->x, by, w->w, w->h - TITLE_H - 16,
                         w->text, *w->text_len);
    } else if (g_console_win_idx == idx) {
        render_console_body(w->x, by, w->w, w->h - TITLE_H - 16);
    } else {
        if (w->body1) draw_string(w->x + 12, by,             w->body1, COL_TEXT);
        if (w->body2) draw_string(w->x + 12, by + CHAR_H + 4, w->body2, COL_TEXT);
    }
}

/* Halt button in the topbar (top-right).  Click to shut down the OS
 * cleanly — same code path as `halt` typed at the xsh prompt or in
 * the WM mini-shell input box. */
#define HALT_BTN_W   72
#define HALT_BTN_H   (TOPBAR_H - 4)
#define HALT_BTN_X   (SCREEN_W - HALT_BTN_W - 4)
#define HALT_BTN_Y   2

static int in_halt_button(int x, int y)
{
    return  x >= HALT_BTN_X && x < HALT_BTN_X + HALT_BTN_W
         && y >= HALT_BTN_Y && y < HALT_BTN_Y + HALT_BTN_H;
}

static void wm_redraw(void)
{
    fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_DESK);
    /* Top status bar */
    fill_rect(0, 0, SCREEN_W, TOPBAR_H, COL_TOPBAR);
    draw_string(8, 4,
        "XINU WM  -  arm-qemu (Versatile PB)  -  drag titlebars  -  click body to focus",
        COL_WHITE);
    /* Halt button at top-right. */
    {
        unsigned short btn_bg     = RGB(180,  40,  40);
        unsigned short btn_border = RGB(255, 220, 220);
        fill_rect(HALT_BTN_X, HALT_BTN_Y, HALT_BTN_W, HALT_BTN_H, btn_bg);
        fill_rect(HALT_BTN_X, HALT_BTN_Y, HALT_BTN_W, 1, btn_border);
        fill_rect(HALT_BTN_X, HALT_BTN_Y + HALT_BTN_H - 1, HALT_BTN_W, 1, btn_border);
        fill_rect(HALT_BTN_X, HALT_BTN_Y, 1, HALT_BTN_H, btn_border);
        fill_rect(HALT_BTN_X + HALT_BTN_W - 1, HALT_BTN_Y, 1, HALT_BTN_H, btn_border);
        /* "[Halt]" — 6 chars × 16px = 96, narrower than HALT_BTN_W=72 with
         *  smaller FONT_SCALE, so just use "Halt" centred. */
        draw_string(HALT_BTN_X + (HALT_BTN_W - 4 * 8 * FONT_SCALE) / 2,
                    HALT_BTN_Y + (HALT_BTN_H - 8 * FONT_SCALE) / 2,
                    "Halt", COL_WHITE);
    }

    /* Sort window indices by z ascending. */
    int order[MAX_WINDOWS], i, j;
    for (i = 0; i < nwin; i++) order[i] = i;
    for (i = 1; i < nwin; i++) {
        int v = order[i];
        for (j = i; j > 0 && winlist[order[j-1]].z > winlist[v].z; j--)
            order[j] = order[j-1];
        order[j] = v;
    }
    for (i = 0; i < nwin; i++)
        if (winlist[order[i]].visible)
            draw_window(order[i]);

    draw_cursor();
}

/* ============================================================
 *  Demo body strings (mutable; updated each frame)
 * ============================================================ */

static char status_xy[40];
static char status_bn[40];

static void format_status(unsigned char btn)
{
    /* xy: "x=NNN y=NNN" */
    int v;
    char *p = status_xy;
    *p++ = 'x'; *p++ = '='; v = cursor_x;
    if (v >= 100) *p++ = '0' + (v / 100) % 10;
    if (v >= 10)  *p++ = '0' + (v / 10) % 10;
    *p++ = '0' + v % 10;
    *p++ = ' '; *p++ = 'y'; *p++ = '=';
    v = cursor_y;
    if (v >= 100) *p++ = '0' + (v / 100) % 10;
    if (v >= 10)  *p++ = '0' + (v / 10) % 10;
    *p++ = '0' + v % 10;
    *p = 0;

    p = status_bn;
    *p++ = 'b'; *p++ = 'u'; *p++ = 't'; *p++ = 't'; *p++ = 'o'; *p++ = 'n';
    *p++ = 's'; *p++ = ':'; *p++ = ' ';
    *p++ = (btn & 0x01) ? 'L' : '-';
    *p++ = (btn & 0x04) ? 'M' : '-';
    *p++ = (btn & 0x02) ? 'R' : '-';
    *p = 0;
}

/* ============================================================
 *  Thread entry
 * ============================================================ */

/* ============================================================
 *  Input window: line-buffered shell
 *
 *  The Input window doubles as a scrollback console + a one-line
 *  command editor. `input_buf` holds the entire scrollback plus the
 *  partially-typed command. `cmd_start` is the offset where the
 *  current command begins (right after the most recent "$ " prompt).
 * ============================================================ */
#define INPUT_BUF_LEN 4096
static char input_buf[INPUT_BUF_LEN];
static int  input_len;
static int  cmd_start;
static int  cmd_cursor;            /* offset within current line */

/* History for the WM mini-shell — same data structure as the
 * console-side readline. */
#include <shell_readline.h>
static struct shell_history g_wm_hist;
static int   g_wm_hist_view = -1;  /* -1 = editing fresh line */
static char  g_wm_hist_saved[160];
static int   g_wm_hist_has_saved = 0;

static const char *input_seed =
    "XINU mini-shell.  cwd = /home    Type 'help' for commands.\n"
    "C demos:    `make && hello`\n"
    "ABCL demos: `cd abclcp/abclc && make && PingPong`\n"
    "$ ";

/* Append a single byte to the scrollback. Drops oldest quarter when full. */
static void sb_putc(char c)
{
    if (input_len >= INPUT_BUF_LEN - 1) {
        int drop = INPUT_BUF_LEN / 4;
        int i;
        for (i = drop; i < input_len; i++)
            input_buf[i - drop] = input_buf[i];
        input_len -= drop;
        cmd_start = (cmd_start > drop) ? cmd_start - drop : 0;
    }
    input_buf[input_len++] = c;
}

static void sb_puts(const char *s) { while (*s) sb_putc(*s++); }

static void prompt(void)
{
    sb_puts("\n$ ");
    cmd_start = input_len;
    cmd_cursor = 0;
}

static void input_seed_reset(void)
{
    int i = 0;
    while (input_seed[i] && i < INPUT_BUF_LEN - 1) {
        input_buf[i] = input_seed[i];
        i++;
    }
    input_len = i;
    cmd_start = i;
    cmd_cursor = 0;
    shell_history_init(&g_wm_hist);
    g_wm_hist_view = -1;
    g_wm_hist_has_saved = 0;
}

/* Forward declarations. */
static void execute_command(char *line, int len);

/* Replace the current command line (everything from cmd_start onwards)
 * with `s`, then place the cursor at the end of `s`.  Used by history
 * navigation and Ctrl-U. */
static void replace_current_line(const char *s)
{
    int i, n = 0;
    while (s[n]) n++;
    if (n > 100) n = 100;             /* room left in input_buf */
    input_len = cmd_start;
    for (i = 0; i < n && input_len + 1 < INPUT_BUF_LEN; i++) {
        input_buf[input_len++] = s[i];
    }
    cmd_cursor = input_len - cmd_start;
}

static void input_append(int c)
{
    int line_len = input_len - cmd_start;
    int i;

    switch (c) {
        case 0x08:                              /* Backspace */
            if (cmd_cursor > 0) {
                for (i = cmd_start + cmd_cursor - 1; i < input_len - 1; i++)
                    input_buf[i] = input_buf[i + 1];
                input_len--;
                cmd_cursor--;
            }
            return;

        case '\n': {                            /* Enter */
            char line[160];
            int  ll = line_len;
            if (ll > (int)sizeof(line) - 1) ll = sizeof(line) - 1;
            for (i = 0; i < ll; i++) line[i] = input_buf[cmd_start + i];
            line[ll] = 0;
            shell_history_add(&g_wm_hist, line);
            g_wm_hist_view     = -1;
            g_wm_hist_has_saved = 0;
            sb_putc('\n');
            execute_command(line, ll);
            prompt();
            return;
        }

        case 0x02:                              /* Ctrl-B / Left */
            if (cmd_cursor > 0) cmd_cursor--;
            return;
        case 0x06:                              /* Ctrl-F / Right */
            if (cmd_cursor < line_len) cmd_cursor++;
            return;
        case 0x01:                              /* Ctrl-A / Home */
            cmd_cursor = 0;
            return;
        case 0x05:                              /* Ctrl-E / End */
            cmd_cursor = line_len;
            return;

        case 0x0B:                              /* Ctrl-K — kill to end */
            input_len = cmd_start + cmd_cursor;
            return;
        case 0x15:                              /* Ctrl-U — kill to start */
            if (cmd_cursor > 0) {
                int tail = line_len - cmd_cursor;
                for (i = 0; i < tail; i++)
                    input_buf[cmd_start + i] =
                        input_buf[cmd_start + cmd_cursor + i];
                input_len -= cmd_cursor;
                cmd_cursor = 0;
            }
            return;

        case 0x10:                              /* Ctrl-P / Up */
            if (g_wm_hist_view + 1 < shell_history_size(&g_wm_hist)) {
                const char *src;
                if (g_wm_hist_view == -1 && !g_wm_hist_has_saved) {
                    int sv = line_len;
                    if (sv >= (int)sizeof(g_wm_hist_saved))
                        sv = sizeof(g_wm_hist_saved) - 1;
                    for (i = 0; i < sv; i++)
                        g_wm_hist_saved[i] = input_buf[cmd_start + i];
                    g_wm_hist_saved[sv] = 0;
                    g_wm_hist_has_saved = 1;
                }
                g_wm_hist_view++;
                src = shell_history_at(&g_wm_hist, g_wm_hist_view);
                if (src) replace_current_line(src);
            }
            return;

        case 0x0E:                              /* Ctrl-N / Down */
            if (g_wm_hist_view > 0) {
                const char *src;
                g_wm_hist_view--;
                src = shell_history_at(&g_wm_hist, g_wm_hist_view);
                if (src) replace_current_line(src);
            } else if (g_wm_hist_view == 0) {
                g_wm_hist_view = -1;
                if (g_wm_hist_has_saved) replace_current_line(g_wm_hist_saved);
                else                     replace_current_line("");
            }
            return;

        case 0x0C:                              /* Ctrl-L — clear & redraw */
            input_seed_reset();
            return;

        default:
            if (c >= 0x20 && c < 0x7F) {
                if (input_len + 1 < INPUT_BUF_LEN) {
                    /* Insert at cursor position; shift the rest of
                     * the current line right by one byte. */
                    for (i = input_len; i > cmd_start + cmd_cursor; i--)
                        input_buf[i] = input_buf[i - 1];
                    input_buf[cmd_start + cmd_cursor] = (char)c;
                    input_len++;
                    cmd_cursor++;
                }
            }
            return;
    }
}

/* ---------------- command dispatch ---------------- */

static int tokenize(char *line, char **argv, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

/* Use sprintf() into a stack buf then sb_puts() everywhere. */
#define SBP(...) do {                       \
        char _b[160];                       \
        sprintf(_b, __VA_ARGS__);           \
        sb_puts(_b);                        \
    } while (0)

static const char *col_name(unsigned short c)
{
    if (c == COL_BG1) return "white";
    if (c == COL_BG2) return "cream";
    if (c == COL_BG3) return "mint";
    return "?";
}

static unsigned short parse_color(const char *name)
{
    if (!strcmp(name, "white")) return COL_BG1;
    if (!strcmp(name, "cream")) return COL_BG2;
    if (!strcmp(name, "mint"))  return COL_BG3;
    if (!strcmp(name, "blue"))  return RGB(180, 200, 255);
    if (!strcmp(name, "pink"))  return RGB(255, 200, 220);
    if (!strcmp(name, "gray"))  return RGB(220, 220, 225);
    return 0;
}

static void cmd_help(void)
{
    sb_puts(
      "WM commands:\n"
      "  help, clear, echo ARGS\n"
      "  windows, move IDX X Y, hide/show IDX, add TITLE, color IDX NAME\n"
      "  mouse, bye\n"
      "filesystem:\n"
      "  ls [PATH], pwd, cd [PATH], cat FILE\n"
      "  mkdir DIR, rmdir DIR, rm FILE, touch FILE, cp SRC DST, mv SRC DST\n"
      "compiler / build:\n"
      "  cc SRC [-o OUT]            compile C\n"
      "  abclc SRC [-o OUT]         translate ABCL/c+ + cc\n"
      "  make [TARGET ...]          build via ./Makefile (auto-gen if missing)\n"
      "  run PATH                   execute a.out (output goes to host terminal)\n"
      "  <name>                     same as `run <name>` if a.out is in cwd\n"
      "system:\n"
      "  ps                         list threads\n"
      "  halt                       shut the OS down (also: click [Halt] in topbar)\n"
      "edit keys:\n"
      "  ^P / Up    prev history       ^N / Down  next history\n"
      "  ^B / Left  cursor left        ^F / Right cursor right\n"
      "  ^A / Home  line start         ^E / End   line end\n"
      "  ^K kill to end   ^U kill to start   ^L clear");
}

static void cmd_clear(void)
{
    /* Reset to just an empty line; prompt() adds the next "$ ". */
    input_len = 0;
}

static void cmd_echo(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (i > 1) sb_putc(' ');
        sb_puts(argv[i]);
    }
}

static void cmd_windows(void)
{
    int i;
    SBP("idx  pos          size       z  vis  bg     title\n");
    for (i = 0; i < nwin; i++) {
        struct window *w = &winlist[i];
        SBP("%2d   (%4d,%4d)  %4dx%-4d %2d   %c   %-6s %s",
            i, w->x, w->y, w->w, w->h, w->z,
            w->visible ? 'y' : 'n',
            col_name(w->bg), w->title);
        if (i < nwin - 1) sb_putc('\n');
    }
}

static void cmd_move(int argc, char **argv)
{
    if (argc != 4) { sb_puts("usage: move IDX X Y"); return; }
    int i = atoi(argv[1]), x = atoi(argv[2]), y = atoi(argv[3]);
    if (i < 0 || i >= nwin) { sb_puts("bad idx"); return; }
    winlist[i].x = x;
    winlist[i].y = y;
    SBP("moved window %d to (%d,%d)", i, x, y);
}

static void cmd_visibility(int argc, char **argv, int vis)
{
    if (argc != 2) { sb_puts("usage: hide|show IDX"); return; }
    int i = atoi(argv[1]);
    if (i < 0 || i >= nwin) { sb_puts("bad idx"); return; }
    winlist[i].visible = vis;
    SBP("window %d %s", i, vis ? "shown" : "hidden");
}

static char added_titles[8][24];
static int  added_count;

static void cmd_add(int argc, char **argv)
{
    if (argc < 2) { sb_puts("usage: add TITLE"); return; }
    if (nwin >= MAX_WINDOWS) { sb_puts("window table full"); return; }
    if (added_count >= 8) { sb_puts("title table full"); return; }
    /* Copy title into stable storage. */
    int i;
    for (i = 0; i < (int)sizeof(added_titles[0]) - 1 && argv[1][i]; i++)
        added_titles[added_count][i] = argv[1][i];
    added_titles[added_count][i] = 0;
    /* Pseudo-random position based on count. */
    int x = 80 + (added_count * 47) % 600;
    int y = 80 + (added_count * 73) % 400;
    unsigned short bgs[3] = { COL_BG1, COL_BG2, COL_BG3 };
    int idx = wm_add(x, y, 360, 160, added_titles[added_count],
                     "(new)", "(drag me)", bgs[added_count % 3]);
    added_count++;
    bring_to_front(idx);
    SBP("added window %d at (%d,%d)", idx, x, y);
}

static void cmd_color(int argc, char **argv)
{
    if (argc != 3) { sb_puts("usage: color IDX NAME"); return; }
    int i = atoi(argv[1]);
    if (i < 0 || i >= nwin) { sb_puts("bad idx"); return; }
    unsigned short c = parse_color(argv[2]);
    if (!c) { sb_puts("unknown color (white/cream/mint/blue/pink/gray)"); return; }
    winlist[i].bg = c;
    SBP("colored window %d %s", i, argv[2]);
}

static void cmd_mouse(void)
{
    SBP("cursor=(%d,%d)  buttons=%c%c%c",
        cursor_x, cursor_y,
        (last_btn & 1) ? 'L' : '-',
        (last_btn & 4) ? 'M' : '-',
        (last_btn & 2) ? 'R' : '-');
}

/* ---------------- filesystem + compiler commands (mirror xsh_*) ---------- */

static int has_suffix_wm(const char *s, const char *suf)
{
    int sl = strlen(s), sfl = strlen(suf);
    if (sl < sfl) return 0;
    return 0 == memcmp(s + sl - sfl, suf, sfl);
}

static void cmd_ls(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : ".";
    struct xdirent ent;
    uint32_t idx = 0, next;
    int n = 0, j;
    while (OK == xfsReaddir(path, idx, &ent, &next)) {
        idx = next;
        if (n > 0) sb_putc(' ');
        for (j = 0; j < ent.name_len; j++) sb_putc(ent.name[j]);
        if (ent.type == XFS_T_DIR) sb_putc('/');
        n++;
    }
    if (n == 0) sb_puts("(empty)");
}

static void cmd_pwd(void)
{
    char buf[XFS_PATH_MAX];
    if (OK == xfsGetcwd(buf, sizeof(buf))) sb_puts(buf);
}

static void cmd_cd(int argc, char **argv)
{
    const char *p = (argc >= 2) ? argv[1] : "/";
    if (OK != xfsChdir(p)) SBP("cd: %s: no such directory", p);
}

static void cmd_cat_wm(int argc, char **argv)
{
    int fd, n, i;
    char buf[256];
    if (argc < 2) { sb_puts("usage: cat FILE"); return; }
    fd = xfsOpen(argv[1], XFS_O_RDONLY);
    if (fd < 0) { SBP("cat: %s: cannot open", argv[1]); return; }
    while ((n = xfsRead(fd, buf, sizeof(buf))) > 0)
        for (i = 0; i < n; i++) sb_putc(buf[i]);
    xfsClose(fd);
}

static void cmd_mkdir_wm(int argc, char **argv)
{
    if (argc < 2) { sb_puts("usage: mkdir DIR"); return; }
    if (OK != xfsMkdir(argv[1])) SBP("mkdir: %s: failed", argv[1]);
}

static void cmd_rmdir_wm(int argc, char **argv)
{
    if (argc < 2) { sb_puts("usage: rmdir DIR"); return; }
    if (OK != xfsRmdir(argv[1])) SBP("rmdir: %s: failed", argv[1]);
}

static void cmd_rm_wm(int argc, char **argv)
{
    if (argc < 2) { sb_puts("usage: rm FILE"); return; }
    if (OK != xfsUnlink(argv[1])) SBP("rm: %s: failed", argv[1]);
}

static void cmd_touch_wm(int argc, char **argv)
{
    if (argc < 2) { sb_puts("usage: touch FILE"); return; }
    if (OK != xfsTouch(argv[1])) SBP("touch: %s: failed", argv[1]);
}

static void cmd_cp_wm(int argc, char **argv)
{
    int in, out, n;
    char buf[256];
    if (argc != 3) { sb_puts("usage: cp SRC DST"); return; }
    in = xfsOpen(argv[1], XFS_O_RDONLY);
    if (in < 0) { sb_puts("cp: cannot open src"); return; }
    out = xfsOpen(argv[2], XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
    if (out < 0) { xfsClose(in); sb_puts("cp: cannot create dst"); return; }
    while ((n = xfsRead(in, buf, sizeof(buf))) > 0)
        xfsWrite(out, buf, n);
    xfsClose(in); xfsClose(out);
    SBP("copied %s -> %s", argv[1], argv[2]);
}

static void cmd_mv_wm(int argc, char **argv)
{
    if (argc != 3) { sb_puts("usage: mv SRC DST"); return; }
    if (OK != xfsRename(argv[1], argv[2])) sb_puts("mv: failed");
}

static void cmd_cc_wm(int argc, char **argv)
{
    const char *src = NULL, *out = "a.out";
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] == '-') { sb_puts("cc: unknown opt"); return; }
        else src = argv[i];
    }
    if (!src) { sb_puts("usage: cc SRC [-o OUT]"); return; }
    SBP("    cc %s -o %s", src, out);
    if (OK != ccCompile(src, out)) sb_puts("    -> failed (see terminal)");
    else                            sb_puts("    -> ok");
}

static void cmd_abclc_wm(int argc, char **argv)
{
    const char *src = NULL, *out = NULL;
    char cpath[XFS_PATH_MAX], binpath[XFS_PATH_MAX];
    int i, dot, len;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else src = argv[i];
    }
    if (!src) { sb_puts("usage: abclc SRC [-o OUT]"); return; }
    if (!out) {
        len = strlen(src);
        dot = -1;
        for (i = 0; src[i]; i++) if (src[i] == '.') dot = i;
        if (dot < 0) dot = len;
        if (dot >= (int)sizeof(binpath) - 4)
        { sb_puts("path too long"); return; }
        memcpy(cpath, src, dot); memcpy(cpath + dot, ".c", 3);
        memcpy(binpath, src, dot); binpath[dot] = 0;
    } else {
        sprintf(cpath, "%s.c", out);
        strlcpy(binpath, out, sizeof(binpath));
    }
    SBP("    abclc %s -> %s -> %s", src, cpath, binpath);
    if (OK != abclcTranslate(src, cpath))
    { sb_puts("    -> translate failed"); return; }
    if (OK != ccCompile(cpath, binpath))
    { sb_puts("    -> cc failed"); return; }
    sb_puts("    -> ok");
}

static void cmd_run_wm(int argc, char **argv)
{
    if (argc < 2) { sb_puts("usage: run PATH"); return; }
    /* Program output goes to this thread's stdout (= host terminal). */
    if (SYSERR == aoutRun(argv[1])) SBP("run: %s failed", argv[1]);
    else sb_puts("(program output: see host terminal)");
}

/* Mirror of console xsh_ps but writing to the WM scrollback. */
static const char * const ps_state_names[] = {
    "?",       /* 0  */
    "curr ",   /* THRCURR  = 1 */
    "free ",   /* THRFREE  = 2 */
    "ready",   /* THRREADY = 3 */
    "recv ",   /* THRRECV  = 4 */
    "sleep",   /* THRSLEEP = 5 */
    "susp ",   /* THRSUSP  = 6 */
    "wait ",   /* THRWAIT  = 7 */
    "rtim ",   /* THRTMOUT = 8 */
    "migr "    /* THRMIGRATE = 9 */
};

static void cmd_ps(void)
{
    int i;
    SBP("%3s %-16s %5s %4s %4s",
        "TID", "NAME", "STATE", "PRIO", "PPID");
    for (i = 0; i < NTHREAD; i++) {
        struct thrent *t = &thrtab[i];
        const char *st;
        if (t->state == THRFREE) continue;
        st = (t->state < (int)(sizeof(ps_state_names) /
                               sizeof(ps_state_names[0])))
             ? ps_state_names[t->state]
             : "?";
        SBP("%3d %-16s %5s %4d %4d",
            i, t->name, st, t->prio, (int)t->parent);
    }
}

/* Inline make: parse Makefile if present, else auto-generate from cwd's
 * *.c and *.abcl, then build all targets (or just the requested ones). */

static void mk_emit_targets_to(int fd, const char *cwd)
{
    struct xdirent ent;
    uint32_t idx = 0, next;
    char nm[XFS_MAX_NAME + 1];
    (void)cwd;
    while (OK == xfsReaddir(cwd, idx, &ent, &next)) {
        idx = next;
        if (ent.type == XFS_T_DIR) continue;
        memcpy(nm, ent.name, ent.name_len); nm[ent.name_len] = 0;
        if (has_suffix_wm(nm, ".c") || has_suffix_wm(nm, ".abcl")) {
            int suf = has_suffix_wm(nm, ".c") ? 2 : 5;
            nm[ent.name_len - suf] = 0;
            xfsWrite(fd, " ", 1);
            xfsWrite(fd, nm, strlen(nm));
        }
    }
}

static void mk_emit_rules_to(int fd, const char *cwd)
{
    struct xdirent ent;
    uint32_t idx = 0, next;
    char nm[XFS_MAX_NAME + 1], base[XFS_MAX_NAME + 1];
    while (OK == xfsReaddir(cwd, idx, &ent, &next)) {
        idx = next;
        if (ent.type == XFS_T_DIR) continue;
        memcpy(nm, ent.name, ent.name_len); nm[ent.name_len] = 0;
        int suf = 0;
        if      (has_suffix_wm(nm, ".c"))    suf = 2;
        else if (has_suffix_wm(nm, ".abcl")) suf = 5;
        else continue;
        memcpy(base, nm, ent.name_len - suf); base[ent.name_len - suf] = 0;
        xfsWrite(fd, base, strlen(base));
        xfsWrite(fd, ": ", 2);
        xfsWrite(fd, nm, strlen(nm));
        xfsWrite(fd, "\n", 1);
    }
}

static int mk_build_target(const char *target)
{
    char src[XFS_PATH_MAX], cpath[XFS_PATH_MAX];
    struct xinode tmp;
    sprintf(src, "%s.c", target);
    if (OK == xfsStat(src, &tmp, NULL)) {
        SBP("    cc %s -o %s", src, target);
        if (OK != ccCompile(src, target)) { sb_puts("    -> failed"); return SYSERR; }
        sb_puts("    -> ok");
        return OK;
    }
    sprintf(src, "%s.abcl", target);
    if (OK == xfsStat(src, &tmp, NULL)) {
        sprintf(cpath, "%s.c", target);
        SBP("    abclc %s -> %s -> %s", src, cpath, target);
        if (OK != abclcTranslate(src, cpath)) { sb_puts("    -> abclc failed"); return SYSERR; }
        if (OK != ccCompile(cpath, target))   { sb_puts("    -> cc failed");    return SYSERR; }
        sb_puts("    -> ok");
        return OK;
    }
    SBP("    %s: no source", target);
    return SYSERR;
}

static void cmd_make_wm(int argc, char **argv)
{
    char cwd[XFS_PATH_MAX], mk[XFS_PATH_MAX];
    struct xinode tmp;
    int fd, i, built = 0, ok = 1;

    if (OK != xfsGetcwd(cwd, sizeof(cwd))) { sb_puts("make: cwd?"); return; }
    {
        const char *sep = (strlen(cwd) > 0 && cwd[strlen(cwd)-1] != '/') ? "/" : "";
        sprintf(mk, "%s%sMakefile", cwd, sep);
    }

    /* Auto-generate Makefile if missing. */
    if (OK != xfsStat(mk, &tmp, NULL)) {
        fd = xfsOpen(mk, XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
        if (fd < 0) { sb_puts("make: cannot create Makefile"); return; }
        const char *hdr =
            "# XINU Makefile (auto-generated)\nTARGETS =";
        xfsWrite(fd, hdr, strlen(hdr));
        mk_emit_targets_to(fd, cwd);
        xfsWrite(fd, "\n\n", 2);
        mk_emit_rules_to(fd, cwd);
        xfsClose(fd);
        sb_puts("make: generated Makefile");
    }

    /* Determine target list. */
    char targets[16][32];
    int  ntargets = 0;
    if (argc >= 2) {
        for (i = 1; i < argc && ntargets < 16; i++) {
            strlcpy(targets[ntargets], argv[i], sizeof(targets[0]));
            ntargets++;
        }
    } else {
        /* Read TARGETS = ... from Makefile. */
        char *buf;
        if (OK == xfsStat(mk, &tmp, NULL) &&
            (buf = (char *)memget(tmp.size + 1)) != (char *)SYSERR) {
            int fd2 = xfsOpen(mk, XFS_O_RDONLY);
            int n   = (fd2 >= 0) ? xfsRead(fd2, buf, tmp.size) : 0;
            if (fd2 >= 0) xfsClose(fd2);
            if (n < 0) n = 0; buf[n] = 0;
            char *p = buf;
            while (*p) {
                char *line = p;
                while (*p && *p != '\n') p++;
                char *eol = p;
                if (*p) p++;
                while (line < eol && (*line == ' ' || *line == '\t')) line++;
                if (line >= eol || *line == '#') continue;
                if (eol - line > 8 && 0 == memcmp(line, "TARGETS", 7)) {
                    char *q = line + 7;
                    while (q < eol && (*q == ' ' || *q == '\t' || *q == '=')) q++;
                    while (q < eol && ntargets < 16) {
                        while (q < eol && (*q == ' ' || *q == '\t')) q++;
                        if (q >= eol) break;
                        int tn = 0;
                        while (q < eol && *q != ' ' && *q != '\t' &&
                               tn < (int)sizeof(targets[0]) - 1)
                            targets[ntargets][tn++] = *q++;
                        targets[ntargets][tn] = 0;
                        if (tn > 0) ntargets++;
                    }
                    break;
                }
            }
            memfree(buf, tmp.size + 1);
        }
    }

    if (ntargets == 0) { sb_puts("make: no targets"); return; }

    for (i = 0; i < ntargets; i++) {
        if (OK == mk_build_target(targets[i])) built++;
        else ok = 0;
    }
    if (ok) SBP("make: built %d target(s)", built);
    else    sb_puts("make: had failures");
}

static void execute_command(char *line, int len)
{
    (void)len;
    char *argv[8];
    int argc = tokenize(line, argv, 8);
    if (argc == 0) return;
    const char *c = argv[0];

    if      (!strcmp(c, "help"))    cmd_help();
    else if (!strcmp(c, "clear"))   cmd_clear();
    else if (!strcmp(c, "echo"))    cmd_echo(argc, argv);
    else if (!strcmp(c, "windows")) cmd_windows();
    else if (!strcmp(c, "move"))    cmd_move(argc, argv);
    else if (!strcmp(c, "hide"))    cmd_visibility(argc, argv, 0);
    else if (!strcmp(c, "show"))    cmd_visibility(argc, argv, 1);
    else if (!strcmp(c, "add"))     cmd_add(argc, argv);
    else if (!strcmp(c, "color"))   cmd_color(argc, argv);
    else if (!strcmp(c, "mouse"))   cmd_mouse();
    else if (!strcmp(c, "bye"))     sb_puts("good bye!");
    else if (!strcmp(c, "halt")) {
        extern void system_halt(void);
        sb_puts("System halt requested.");
        system_halt();    /* never returns */
    }
    /* Filesystem + compiler (mirror the console shell). */
    else if (!strcmp(c, "ls"))      cmd_ls(argc, argv);
    else if (!strcmp(c, "pwd"))     cmd_pwd();
    else if (!strcmp(c, "cd"))      cmd_cd(argc, argv);
    else if (!strcmp(c, "cat"))     cmd_cat_wm(argc, argv);
    else if (!strcmp(c, "mkdir"))   cmd_mkdir_wm(argc, argv);
    else if (!strcmp(c, "rmdir"))   cmd_rmdir_wm(argc, argv);
    else if (!strcmp(c, "rm"))      cmd_rm_wm(argc, argv);
    else if (!strcmp(c, "touch"))   cmd_touch_wm(argc, argv);
    else if (!strcmp(c, "cp"))      cmd_cp_wm(argc, argv);
    else if (!strcmp(c, "mv"))      cmd_mv_wm(argc, argv);
    else if (!strcmp(c, "cc"))      cmd_cc_wm(argc, argv);
    else if (!strcmp(c, "abclc"))   cmd_abclc_wm(argc, argv);
    else if (!strcmp(c, "make"))    cmd_make_wm(argc, argv);
    else if (!strcmp(c, "run"))     cmd_run_wm(argc, argv);
    else if (!strcmp(c, "ps"))      cmd_ps();
    else {
        /* Fall-through: try the token as an a.out file in the cwd or with
         * an absolute path.  If it loads, run it; otherwise complain. */
        if (SYSERR != aoutRun(c)) {
            sb_puts("(program output: see host terminal)");
        } else {
            SBP("unknown command: %s  (try 'help')", c);
        }
    }
}

thread wm_main(void)
{
    kprintf("[WM] starting; framebuffer at 0x%x\r\n",
            (unsigned int)(unsigned long)framebuffer);
    lcd_init();
    kprintf("[WM] lcd_init done; CTRL=0x%x UPBASE=0x%x\r\n",
            LCD_BASE[LCD_CTRL], LCD_BASE[LCD_UPBASE]);
    mouse_init();
    kbd_init();
    kprintf("[WM] mouse + keyboard ready\r\n");

    /* Initial cwd for the WM mini-shell: somewhere with sample files. */
    xfsChdir("/home");

    input_seed_reset();

    /* Demo windows on the 1024x1024 desktop.  Each is enlarged ~4x in area
     * relative to the original 1024x768 layout; the Input window in
     * particular spans the full bottom half so that long output is visible. */
    wm_add(  20,  20, 540, 460, "Welcome",     "Hello from XINU!",
            "Embedded Xinu on QEMU.", COL_BG1);
    wm_add( 580,  20, 424, 200, "About",       "Embedded Xinu v2.0",
            "Versatile Platform B.",  COL_BG3);
    wm_add( 580, 240, 424, 240, "Mouse Info",  "(move the mouse)",
            "(L / M / R buttons)",    COL_BG2);
    /* Real xsh shell hosted in the WM (WMCON0 device).  60 cols × 16
     * rows in a wide bottom-mid window — supports `edit`, history,
     * arrow keys, everything the console xsh has. */
    wm_add(  20, 500, 984, 290, "Console", NULL, NULL, COL_BG3);
    g_console_win_idx = nwin - 1;
    /* Existing mini-shell stays at the very bottom in a slimmed-down
     * window for quick commands without going through wmcon. */
    wm_add_text(20, 800, 984, 204, "Input",
                input_buf, &input_len, COL_BG1);
    bring_to_front(2);
    format_status(0);
    winlist[2].body1 = status_xy;
    winlist[2].body2 = status_bn;

    int dirty = 1;
    for (;;) {
        int dx, dy;
        unsigned char btn = last_btn;
        if (mouse_drain(&dx, &dy, &btn)) {
            cursor_x += dx;
            cursor_y += dy;
            if (cursor_x < 0) cursor_x = 0;
            if (cursor_y < 0) cursor_y = 0;
            if (cursor_x >= SCREEN_W) cursor_x = SCREEN_W - 1;
            if (cursor_y >= SCREEN_H) cursor_y = SCREEN_H - 1;

            unsigned char pressed  =  btn & ~last_btn;
            unsigned char released = ~btn &  last_btn;

            if (pressed & 0x01) {
                /* [Halt] button in the topbar — clean shutdown. */
                if (in_halt_button(cursor_x, cursor_y)) {
                    extern void system_halt(void);
                    system_halt();      /* never returns */
                }
                /* ABCL スライダー/ボタンを優先 */
                extern int abcl_xinu_gui_handle_click(int mx, int my);
                if (!abcl_xinu_gui_handle_click(cursor_x, cursor_y)) {
                    int hit = wm_hit(cursor_x, cursor_y);
                    if (hit >= 0) {
                        bring_to_front(hit);
                        if (in_titlebar(hit, cursor_x, cursor_y)) {
                            dragging = 1;
                            drag_target = hit;
                            drag_off_x = cursor_x - winlist[hit].x;
                            drag_off_y = cursor_y - winlist[hit].y;
                        }
                    }
                }
            }
            if (released & 0x01) {
                extern void abcl_xinu_gui_handle_release(void);
                abcl_xinu_gui_handle_release();
                dragging = 0; drag_target = -1;
            }
            if (dragging && drag_target >= 0) {
                winlist[drag_target].x = cursor_x - drag_off_x;
                winlist[drag_target].y = cursor_y - drag_off_y;
            }
            /* スライダードラッグ更新 */
            {
                extern void abcl_xinu_gui_handle_drag(int mx, int my);
                abcl_xinu_gui_handle_drag(cursor_x, cursor_y);
            }

            last_btn = btn;
            format_status(btn);
            dirty = 1;
        }

        int kc = kbd_drain_char();
        if (kc) {
            if (focused == g_console_win_idx && g_console_win_idx >= 0) {
                /* Convert "\r"-only Enter from the line buffer to "\n"
                 * so the shell reads a complete line. */
                if (kc == '\r') wm_console_feed_key('\n');
                else            wm_console_feed_key(kc);
            } else {
                input_append(kc);
            }
            dirty = 1;
        }

        /* ABCL/c+ 連携: ticker に毎フレーム tick() を配信 */
        {
            extern void abcl_xinu_gui_tick_all(void);
            abcl_xinu_gui_tick_all();
        }

        /* Position the input caret at (cmd_start + cmd_cursor) so the
         * renderer draws it mid-line during left/right movement.  When
         * cmd_cursor == line_len this coincides with the natural
         * end-of-text caret. */
        g_input_caret = cmd_start + cmd_cursor;

        /* 線分は毎フレーム再描画 (アクターがフィールドを更新している) */
        wm_redraw();
        {
            extern void abcl_xinu_gui_render(void);
            abcl_xinu_gui_render();
        }
        /* User overlay (e.g. `rotlines`) — drawn last so it sits on top. */
        if (g_wm_user_render) g_wm_user_render();
        dirty = 0;

        sleep(16); /* ~60 Hz */
    }
    return OK;
}

/* ============================================================
 *  Console window body renderer — paints the wmcon cell grid.
 * ============================================================ */
static void render_console_body(int x, int y, int w, int h)
{
    int rows, cols;
    const char *cells;
    int cur_r, cur_c;
    int r, c;

    (void)w; (void)h;

    wm_console_get_state(&rows, &cols, &cells, &cur_r, &cur_c);

    for (r = 0; r < rows; r++) {
        int ry = y + r * CHAR_H;
        for (c = 0; c < cols; c++) {
            char ch = cells[r * cols + c];
            if (ch != ' ') {
                int rx = x + 8 + c * CHAR_W;
                draw_char(rx, ry, ch, COL_TEXT);
            }
        }
    }

    /* Caret at (cur_r, cur_c) — small block. */
    if (cur_r >= 0 && cur_r < rows && cur_c >= 0 && cur_c <= cols) {
        int cx = x + 8 + cur_c * CHAR_W;
        int cy = y + cur_r * CHAR_H;
        fill_rect(cx, cy, FONT_SCALE, CHAR_H, COL_TEXT);
    }
}
