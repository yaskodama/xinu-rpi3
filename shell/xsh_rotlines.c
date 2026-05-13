/**
 * @file xsh_rotlines.c
 *
 * Draws four lines through the screen center, rotating around it.
 * Runs as an overlay on top of the WM by registering wm_set_user_render().
 *
 * Usage:  rotlines [DURATION_FRAMES]    (default 240 = ~4 seconds at 60Hz)
 */

#include <stddef.h>
#include <shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread.h>

/* Provided by apps/wm.c */
extern void wm_set_user_render(void (*)(void));
extern void wm_draw_line(int x0, int y0, int x1, int y1, unsigned short c);

/* Screen layout — must match apps/wm.c constants. */
#define ROT_SCREEN_W 1024
#define ROT_SCREEN_H 768

#define ROT_RGB(r, g, b)                                                       \
    ((unsigned short)(((((b) >> 3)) << 11) | ((((g) >> 2)) << 5) | (((r) >> 3))))

/* Animation state shared between this thread and the WM render hook. */
static volatile int rot_angle;        /* current angle in degrees */
static volatile int rot_radius;       /* line length from center  */
static volatile int rot_active;       /* 1 while animating        */

/* Bhaskara's approximation of sin(deg) in Q12 (4096 = 1.0).  Accurate to
 * ~0.2% over [0,180]; reflected for negative half. */
static int isin_q12(int deg)
{
    int t, num, den, s;
    deg = ((deg % 360) + 360) % 360;
    if (deg <= 180)
    {
        t = deg;
        num = 16384 * t * (180 - t);
        den = 40500 - t * (180 - t);
        s = num / den;
    }
    else
    {
        t = deg - 180;
        num = 16384 * t * (180 - t);
        den = 40500 - t * (180 - t);
        s = -(num / den);
    }
    return s;
}
static int icos_q12(int deg) { return isin_q12(deg + 90); }

/* Render hook — called by the WM thread once per frame.  Reads the latest
 * angle/radius and draws four lines through (cx, cy) at θ, θ+90°, θ+180°,
 * θ+270°. */
static void rot_render(void)
{
    int a       = rot_angle;
    int r       = rot_radius;
    int cx      = ROT_SCREEN_W / 2;
    int cy      = ROT_SCREEN_H / 2;
    int ca      = icos_q12(a);
    int sa      = isin_q12(a);
    int dx      = (r * ca) / 4096;
    int dy      = (r * sa) / 4096;
    /* The four lines are pairs of opposite endpoints from the center.
     * Their offsets are (dx, dy), (-dy, dx), (-dx, -dy), (dy, -dx) — i.e.
     * 90° rotations of the first endpoint. */
    unsigned short c0 = ROT_RGB(255,  80,  80);
    unsigned short c1 = ROT_RGB( 80, 255,  80);
    unsigned short c2 = ROT_RGB( 80, 160, 255);
    unsigned short c3 = ROT_RGB(255, 220,  60);

    wm_draw_line(cx, cy, cx + dx, cy + dy, c0);
    wm_draw_line(cx, cy, cx - dy, cy + dx, c1);
    wm_draw_line(cx, cy, cx - dx, cy - dy, c2);
    wm_draw_line(cx, cy, cx + dy, cy - dx, c3);
}

shellcmd xsh_rotlines(int nargs, char *args[])
{
    int frames = 240;
    int speed  = 3;        /* degrees per frame */
    int i;

    if (nargs >= 2) frames = atoi(args[1]);
    if (frames <= 0) frames = 240;
    if (nargs >= 3) speed = atoi(args[2]);
    if (speed == 0) speed = 3;

    rot_angle  = 0;
    rot_radius = 320;
    rot_active = 1;
    wm_set_user_render(rot_render);

    printf("rotlines: %d frames at %d deg/frame (Ctrl-C-style: just wait)\n",
           frames, speed);

    for (i = 0; i < frames; i++)
    {
        rot_angle += speed;
        sleep(16);          /* ~60 Hz */
    }

    rot_active = 0;
    wm_set_user_render(NULL);
    printf("rotlines: done\n");
    return 0;
}
