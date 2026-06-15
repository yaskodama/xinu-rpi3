/**
 * @file fbPutc.c
 */

/* Embedded Xinu, Copyright (C) 2009, 2013.  All rights reserved. */

#include <stddef.h>
#include <framebuffer.h>
#include <device.h>

extern int rows;
extern int cols;
extern int cursor_col;
extern int cursor_row;
extern ulong foreground;
extern ulong background;
extern bool screen_initialized;

/* Text caret (underline) at the current (cursor_col, cursor_row) cell.  The
 * framebuffer console drew characters but no caret, so the no-window-system
 * kernels (OS1/OS2) showed a prompt with nowhere visible to type.  Paint a
 * 2-pixel underline at the input position, erased just before the next glyph
 * is drawn so it always tracks the cursor. */
#define FB_CURSOR_H 2
static void fb_cursor_paint(ulong color)
{
    int x0 = cursor_col * CHAR_WIDTH;
    int y0 = cursor_row * CHAR_HEIGHT;
    int i, j;
    for (i = CHAR_HEIGHT - FB_CURSOR_H; i < CHAR_HEIGHT; i++)
        for (j = 0; j < CHAR_WIDTH; j++)
            drawPixel(x0 + j, y0 + i, color);
}

/**
 * @ingroup framebuffer
 *
 * Write a single character to the framebuffer
 * @param  devptr  pointer to framebuffer device
 * @param  ch    character to write
 */
devcall fbPutc(device *devptr, char ch)
{
    if (screen_initialized)
    {
        fb_cursor_paint(background);        /* erase caret at the old cell */

        if (ch == '\n')
        {
            cursor_row++;
            cursor_col = 0;
        }
        else if (ch == '\t')
        {
            cursor_col += 4;
        }
        else if (ch == '\b')                /* backspace: step back + blank */
        {
            if (cursor_col > 0)
                cursor_col--;
            else if (cursor_row > 0)
            {
                cursor_row--;
                cursor_col = cols - 1;
            }
            drawChar(' ', cursor_col * CHAR_WIDTH,
                     cursor_row * CHAR_HEIGHT, foreground);
            fb_cursor_paint(foreground);    /* re-draw caret at new cell */
            return (uchar)ch;
        }
        else
        {
            drawChar(ch, cursor_col * CHAR_WIDTH,
                     cursor_row * CHAR_HEIGHT, foreground);
            cursor_col++;
        }

        if (cursor_col == cols)
        {
            cursor_col = 0;
            cursor_row += 1;
        }
        if ( (minishell == TRUE) && (cursor_row == rows) )
        {
            minishellClear(background);
            cursor_row = rows - MINISHELLMINROW;
        }
        else if (cursor_row == rows)
        {
            screenClear(background);
            cursor_row = 0;
        }

        fb_cursor_paint(foreground);        /* draw caret at the new cell */
        return (uchar)ch;
    }
    return SYSERR;
}
