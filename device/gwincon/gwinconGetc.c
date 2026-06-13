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
#define NGWIN      2               /* GWINCON0 + GWINCON1 */

static volatile uchar gwin_inbuf[NGWIN][GWIN_IBLEN];
static volatile uint  gwin_ihead[NGWIN];   /* next write index (feed) */
static volatile uint  gwin_itail[NGWIN];   /* next read index  (getc) */
static semaphore      gwin_isema[NGWIN];   /* count of bytes ready    */
static int            gwin_iready[NGWIN];  /* set once initialised    */

/**
 * Initialise one input ring + its semaphore.  Called from gwinconInit()
 * (i.e. when GWINCONx is opened in main()), after sysinit() so that
 * semcreate() is usable.
 *
 * @param m  minor number (0..NGWIN-1)
 */
void gwincon_input_init(int m)
{
    if (m < 0 || m >= NGWIN) return;
    if (gwin_iready[m]) return;
    gwin_ihead[m] = 0;
    gwin_itail[m] = 0;
    gwin_isema[m] = semcreate(0);
    gwin_iready[m] = 1;
}

/**
 * Inject one keystroke into a shell's input stream.  Safe to call
 * from any thread; drops the key if the device is not yet open or the
 * ring is full.
 *
 * @param m  minor number (which shell window)
 * @param c  character code (0..255)
 */
void gwincon_feed(int m, int c)
{
    irqmask im;
    uint nh;

    if (m < 0 || m >= NGWIN || !gwin_iready[m]) return;

    im = disable();
    nh = (gwin_ihead[m] + 1) % GWIN_IBLEN;
    if (nh == gwin_itail[m]) {         /* ring full — drop */
        restore(im);
        return;
    }
    gwin_inbuf[m][gwin_ihead[m]] = (uchar)c;
    gwin_ihead[m] = nh;
    restore(im);

    signal(gwin_isema[m]);
}

/**
 * Diagnostic: report the input-ring state (minor 0) so the HTTP layer
 * can tell whether injected keys are being consumed by the shell.
 */
void gwincon_input_stat(int *head, int *tail, int *ready)
{
    if (head)  *head  = (int)gwin_ihead[0];
    if (tail)  *tail  = (int)gwin_itail[0];
    if (ready) *ready = gwin_iready[0];
}

/**
 * Read a single character (blocking) for a windowed shell's stdin.
 *
 * @param devptr  device table entry (minor selects the window)
 * @return        the character read, or SYSERR if not initialised
 */
devcall gwinconGetc(device *devptr)
{
    irqmask im;
    uchar ch;
    int   m = devptr->minor;

    if (m < 0 || m >= NGWIN || !gwin_iready[m]) return SYSERR;

    wait(gwin_isema[m]);              /* block until a key is fed */

    im = disable();
    ch = gwin_inbuf[m][gwin_itail[m]];
    gwin_itail[m] = (gwin_itail[m] + 1) % GWIN_IBLEN;
    restore(im);

    return ch;
}
