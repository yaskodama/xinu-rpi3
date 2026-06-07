/**
 * @file screenInit.c
 *
 * Initializes communication channels between VC and ARM.
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#include <stddef.h>
#include <framebuffer.h>
#include <stdlib.h>
#include <shell.h> /* for banner */
#include <kernel.h>
#include <bcm2835.h>

int rows;
int cols;
int cursor_row;
int cursor_col;
ulong background;
ulong foreground;
ulong linemap[MAPSIZE];
bool minishell;
ulong framebufferAddress;
int pitch;
bool screen_initialized;

/* screenInit(): Calls framebufferInit() several times to ensure we successfully initialize, just in case. */
void screenInit() {
	int i = 0;
    while (framebufferInit() == SYSERR) {
        if ( (i++) == MAXRETRIES) {
            screen_initialized = FALSE;
            return;
        }
    }
	// clear the screen to the background color.
	screenClear(background);
    initlinemap();
    screen_initialized = TRUE;
#ifdef _XINU_PLATFORM_ARM_RPI3_
    /* Draw a banner so the HDMI output is obviously alive on the Pi3. */
    fbprintf("\n  Embedded XINU  --  Raspberry Pi 3 (arm-rpi3)\n"
             "  HDMI framebuffer %dx%d %d-bit is alive!\n"
             "  serial console: xsh on UART0 @ 115200\n",
             DEFAULT_WIDTH, DEFAULT_HEIGHT, BIT_DEPTH);
#endif
}

/* Initializes the framebuffer used by the GPU. Returns OK on success; SYSERR on failure. */
int framebufferInit() {
    //GPU expects this struct to be 16 byte aligned
    struct framebuffer frame __attribute__((aligned (16)));

	frame.width_p = DEFAULT_WIDTH; //must be less than 4096
	frame.height_p = DEFAULT_HEIGHT; //must be less than 4096
	frame.width_v = DEFAULT_WIDTH; //must be less than 4096
	frame.height_v = DEFAULT_HEIGHT; //must be less than 4096
	frame.pitch = 0; //no space between rows
	frame.depth = BIT_DEPTH; //must be equal to or less than 32
	frame.x = 0; //no x offset
	frame.y = 0; //no y offset
	frame.address = 0; //always initializes to 0x48006000
	frame.size = 0;

#ifdef _XINU_PLATFORM_ARM_RPI3_
    /* Pass the struct's UNCACHED bus alias (0xC0000000 | phys).  Xinu runs
     * with the D-cache off, so CPU writes are already in RAM; giving the GPU
     * the uncached alias makes it read RAM directly instead of a stale GPU-L2
     * view (the 0x0 cached alias), which left address/size = 0. */
    mailboxWrite(physToBus(&frame));
#else
    mailboxWrite((ulong)&frame);
#endif

	ulong result = mailboxRead();

	/* Error checking */
	if (result) { //if anything but zero
		return SYSERR;
	}
    if (!frame.address) { //if address remains zero
		return SYSERR;
	}

    /* Initialize global variables */
#ifdef _XINU_PLATFORM_ARM_RPI3_
    /* The GPU returns a VideoCore bus address; mask to the ARM physical
     * address (the 0xC0000000 uncached bus alias maps to phys & 0x3FFFFFFF). */
    framebufferAddress = frame.address & 0x3FFFFFFF;
#else
    framebufferAddress = frame.address;
#endif
    rows = frame.height_p / CHAR_HEIGHT;
    cols = frame.width_p / CHAR_WIDTH;
    pitch = frame.pitch;
    cursor_row = 0;
    cursor_col = 0;
    background = BLACK;
    foreground = WHITE;
    minishell = FALSE;
	return OK;
}

/* Clear the framebuffer console and move the cursor home.  Called by the
 * keyboard shell before each command runs, so every command's output starts
 * on a fresh screen and is fully visible (smooth scrolling is impractical
 * here because the GPU framebuffer cannot be read back with the D-cache
 * off). */
void fbConsoleClear(void) {
    if (screen_initialized) {
        screenClear(background);
        cursor_row = 0;
        cursor_col = 0;
    }
}

/* Very heavy handed clearing of the screen to a single color. */
void screenClear(ulong color) {
	ulong *address = (ulong *)(framebufferAddress);
    ulong *maxaddress = (ulong *)(framebufferAddress + (DEFAULT_HEIGHT * pitch) + (DEFAULT_WIDTH * (BIT_DEPTH / 8)));
    while (address != maxaddress) {
	    *address = color;
        address++;
    }
}

/* Clear the minishell window */
void minishellClear(ulong color) {
    ulong *address = (ulong *)(framebufferAddress + (pitch * (DEFAULT_HEIGHT - (MINISHELLMINROW * CHAR_HEIGHT))) +  (DEFAULT_WIDTH * (BIT_DEPTH / 8)));
    ulong *maxaddress = (ulong *)(framebufferAddress + (DEFAULT_HEIGHT * pitch) + (DEFAULT_WIDTH * (BIT_DEPTH / 8)));
    while (address != maxaddress) {
	    *address = color;
        address++;
    }
}

/* Clear the "linemapping" array used to keep track of pixels we need to remember */
void initlinemap() {
    int i = MAPSIZE;
    while (i != 0) {
        i--;
        linemap[i] = background;
    }
}
