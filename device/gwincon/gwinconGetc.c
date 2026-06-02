/**
 * @file gwinconGetc.c
 *
 * Input side of the window-console pseudo-device.  The windowed xsh
 * shell opens GWINCON0 for stdin and reads keystrokes one at a time
 * via gwinconGetc().  Keys are not produced by a physical keyboard on
 * the Pi3 — instead the Mac dashboard injects them over HTTP
 * (/api/wifi/key in apps/webactor.c), which calls gwincon_feed().
 *
 * A small ring buffer decouples the producer (webactor HTTP thread)
 * from the consumer (shell thread blocked in shell_readline -> getc).
 * A counting semaphore makes getc block until a key is available.
 */
/* Embedded Xinu, arm-rpi3 window manager add-on. */

#include <gwincon.h>
#include <semaphore.h>
#include <interrupt.h>

#define GWIN_IBLEN 256

static volatile uchar gwin_inbuf[GWIN_IBLEN];
static volatile uint  gwin_ihead;          /* next write index (feed) */
static volatile uint  gwin_itail;          /* next read index  (getc) */
static semaphore      gwin_isema = 0;      /* count of bytes ready    */
static int            gwin_iready = 0;     /* set once initialised    */

/**
 * Initialise the input ring + its semaphore.  Called from gwinconInit()
 * (i.e. when GWINCON0 is opened in main()), after sysinit() so that
 * semcreate() is usable.
 */
void gwincon_input_init(void)
{
    if (gwin_iready) return;
    gwin_ihead = 0;
    gwin_itail = 0;
    gwin_isema = semcreate(0);
    gwin_iready = 1;
}

/**
 * Inject one keystroke into the shell's input stream.  Safe to call
 * from any thread; drops the key if the device is not yet open or the
 * ring is full.
 *
 * @param c  character code (0..255)
 */
void gwincon_feed(int c)
{
    irqmask im;
    uint nh;

    if (!gwin_iready) return;

    im = disable();
    nh = (gwin_ihead + 1) % GWIN_IBLEN;
    if (nh == gwin_itail) {            /* ring full — drop */
        restore(im);
        return;
    }
    gwin_inbuf[gwin_ihead] = (uchar)c;
    gwin_ihead = nh;
    restore(im);

    signal(gwin_isema);
}

/**
 * Read a single character (blocking) for the windowed shell's stdin.
 *
 * @param devptr  device table entry (unused)
 * @return        the character read, or SYSERR if not initialised
 */
/**
 * Diagnostic: report the input-ring state so the HTTP layer can tell
 * whether injected keys are being consumed by the shell.
 */
void gwincon_input_stat(int *head, int *tail, int *ready)
{
    if (head)  *head  = (int)gwin_ihead;
    if (tail)  *tail  = (int)gwin_itail;
    if (ready) *ready = gwin_iready;
}

devcall gwinconGetc(device *devptr)
{
    irqmask im;
    uchar ch;

    (void)devptr;
    if (!gwin_iready) return SYSERR;

    wait(gwin_isema);                 /* block until a key is fed */

    im = disable();
    ch = gwin_inbuf[gwin_itail];
    gwin_itail = (gwin_itail + 1) % GWIN_IBLEN;
    restore(im);

    return ch;
}
