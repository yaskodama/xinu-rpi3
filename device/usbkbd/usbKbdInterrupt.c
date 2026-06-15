/**
 * @file usbKbdInterrupt.c
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#include <usbkbd.h>
#include <usb_core_driver.h>
#include <string.h>
#include <interrupt.h>

#define HID_BOOT_LEFT_CTRL   (1 << 0)
#define HID_BOOT_LEFT_SHIFT  (1 << 1)
#define HID_BOOT_LEFT_ALT    (1 << 2)
#define HID_BOOT_LEFT_GUI    (1 << 3)
#define HID_BOOT_RIGHT_CTRL  (1 << 4)
#define HID_BOOT_RIGHT_SHIFT (1 << 5)
#define HID_BOOT_RIGHT_ALT   (1 << 6)
#define HID_BOOT_RIGHT_GUI   (1 << 7)

/* Map of HID keyboard usage IDs to characters.
 *
 * Entries not filled in are left 0 and are interpreted as unrecognized input
 * and ignored.
 *
 * Source: section 10 of the Universal Serial Bus HID Usage Tables v1.11.
 */
static const char keymap[256][2] = {
    [4]  = {'a', 'A'},
    [5]  = {'b', 'B'},
    [6]  = {'c', 'C'},
    [7]  = {'d', 'D'},
    [8]  = {'e', 'E'},
    [9]  = {'f', 'F'},
    [10] = {'g', 'G'},
    [11] = {'h', 'H'},
    [12] = {'i', 'I'},
    [13] = {'j', 'J'},
    [14] = {'k', 'K'},
    [15] = {'l', 'L'},
    [16] = {'m', 'M'},
    [17] = {'n', 'N'},
    [18] = {'o', 'O'},
    [19] = {'p', 'P'},
    [20] = {'q', 'Q'},
    [21] = {'r', 'R'},
    [22] = {'s', 'S'},
    [23] = {'t', 'T'},
    [24] = {'u', 'U'},
    [25] = {'v', 'V'},
    [26] = {'w', 'W'},
    [27] = {'x', 'X'},
    [28] = {'y', 'Y'},
    [29] = {'z', 'Z'},
    [30] = {'1', '!'},
    [31] = {'2', '@'},
    [32] = {'3', '#'},
    [33] = {'4', '$'},
    [34] = {'5', '%'},
    [35] = {'6', '^'},
    [36] = {'7', '&'},
    [37] = {'8', '*'},
    [38] = {'9', '('},
    [39] = {'0', ')'},
    [40] = {'\n', '\n'},     /* Enter      */
    /* ... */
    [42] = {'\b', '\b'},     /* Backspace  */
    [43] = {'\t', '\t'},     /* Tab        */
    [44] = {' ', ' '},       /* Space      */
    [45] = {'-', '_'},
    [46] = {'=', '+'},
    [47] = {'[', '{'},
    [48] = {']', '}'},
    [49] = {'\\', '|'},
    /* ... */
    [51] = {';', ':'},
    [52] = {'\'', '"'},
    [53] = {'`', '~'},
    [54] = {',', '<'},
    [55] = {'.', '>'},
    [56] = {'/', '?'},
    [57] = {},               /* Caps lock  */
    /* ... */
    [76] = {'\x7f', '\x7f'}, /* Delete                    */
    /* ... */
    [84] = {'/'},            /* Keypad /                  */
    [85] = {'*'},            /* Keypad *                  */
    [86] = {'-'},            /* Keypad -                  */
    [87] = {'+'},            /* Keypad +                  */
    [88] = {'\n'},           /* Keypad Enter              */
    [89] = {'1'},            /* Keypad 1 and End          */
    [90] = {'2'},            /* Keypad 2 and Down Arrow   */
    [91] = {'3'},            /* ...                       */
    [92] = {'4'},
    [93] = {'5'},
    [94] = {'6'},
    [95] = {'7'},
    [96] = {'8'},
    [97] = {'9'},
    [98] = {'0'},
    [99] = {'.', '\x7f'},    /* Keypad . and Delete       */
    [103] = {'=', '='},      /* Keypad =                  */
};

/* ---- auto-repeat (typematic) state ---------------------------------------
 * USB boot keyboards only send a report when a key changes, so a *held* key
 * produces a single keypress.  We publish the most-recently-pressed key here
 * (its translated byte sequence + the raw usage id so we can tell when it is
 * released); a separate slow timer thread (kbd_repeat_bridge in main.c) re-
 * injects the sequence while the key stays down, giving cursor keys / typing
 * a "hold to repeat" behaviour. */
volatile char     g_kbd_repeat_seq[3] = {0,0,0};
volatile int      g_kbd_repeat_len    = 0;     /* 0 = nothing held to repeat   */
volatile unsigned g_kbd_repeat_gen    = 0;     /* bumps on every NEW keypress  */
static   uchar    g_kbd_repeat_usage  = 0;     /* usage id of the repeat key   */

/**
 * Called when the USB transfer request from a USB keyboard's IN interrupt
 * endpoint completes or fails.
 */
/* ---- diagnostics (read via /api/kbdstat) -------------------------------- *
 * Localises the "physical keyboard dies after using the soft keyboard" bug:
 *   int_calls   bumps every time the HID completion callback fires (i.e. the
 *               keyboard's interrupt transfer completed and was re-armed).
 *   int_reports bumps on each well-formed 8-byte report actually parsed.
 *   last_status records req->status of the most recent callback.
 *   inject_cnt  bumps on each soft-key usbKbdInject().
 * If pressing a physical key no longer bumps int_calls, the USB transfer has
 * halted at the HCD level; if int_calls bumps but no char appears, the bug is
 * above the HCD (ring/delivery). */
volatile unsigned g_kbd_int_calls   = 0;
volatile unsigned g_kbd_int_reports = 0;
volatile int      g_kbd_last_status = -999;
volatile unsigned g_kbd_inject_cnt  = 0;
volatile unsigned g_kbd_resubmit_fail = 0;
volatile unsigned g_kbd_err_resubmits = 0;   /* throttled re-arms after errors */
volatile unsigned g_kbd_clear_halts   = 0;   /* CLEAR_FEATURE(ENDPOINT_HALT) recoveries */

/* Auto re-enumeration: when the interrupt endpoint hard-errors and re-arming
 * cannot clear it (USB_STATUS_HARDWARE_ERROR storm), software-replug the
 * keyboard.  Counts consecutive errors; past a threshold a worker thread
 * (usbKbdRecover, driven from kbd_repeat_bridge) re-enumerates the device. */
#define KBD_ERR_REENUM_THRESH 40     /* ~1.5 s of solid errors before replug   */
#define KBD_MAX_AUTO_REENUM   8      /* give up after this many failed replugs  */
volatile unsigned g_kbd_consec_err  = 0;
volatile int      g_kbd_need_reenum = 0;
volatile unsigned g_kbd_reenum_cnt  = 0;     /* auto re-enumerations performed   */

/* Set by usbKbdInterrupt when an errored transfer's re-arm is deferred; the
 * throttle thread (usbKbdResubmitPending) re-submits it at a low rate. */
static struct usb_xfer_request *g_kbd_intr_req;
volatile int g_kbd_resubmit_pending = 0;

void usbKbdDiag(unsigned *calls, unsigned *reports, int *last_status,
                unsigned *injects, int *icount, int *istart, unsigned *resub_fail)
{
    struct usbkbd *kbd = &usbkbds[0];
    if (calls)       *calls       = g_kbd_int_calls;
    if (reports)     *reports     = g_kbd_int_reports;
    if (last_status) *last_status = g_kbd_last_status;
    if (injects)     *injects     = g_kbd_inject_cnt;
    if (icount)      *icount      = (int)kbd->icount;
    if (istart)      *istart      = (int)kbd->istart;
    if (resub_fail)  *resub_fail  = g_kbd_resubmit_fail;
}

void usbKbdInterrupt(struct usb_xfer_request *req)
{
    struct usbkbd *kbd = req->private;

    g_kbd_int_calls++;
    g_kbd_last_status = (int)req->status;

    if (req->status == USB_STATUS_SUCCESS && req->actual_size == 8)
    {
        g_kbd_int_reports++;
        /* An 8 byte report was successfully received from the USB keyboard.
         * Note that we're using the boot protocol, so the interpretation of
         * this data is fixed and not affected by the HID report descriptor.  */

        USBKBD_TRACE("Keyboard report received");

        const uchar *data = req->recvbuf;
        uint mod_idx = 0;
        uint count = 0;
        uint i;

        /* Byte 0 is modifiers mask.  We currently only care about shift.  */
        if (data[0] & (HID_BOOT_LEFT_SHIFT | HID_BOOT_RIGHT_SHIFT))
        {
            mod_idx++;
        }

        /* Byte 1 must be ignored.  */

        /* Bytes 2 through 8 are the usage IDs of non-modifier keys currently
         * pressed, or 0 to indicate no key pressed.
         *
         * Note that the keyboard sends a full report when any key is pressed or
         * released.  If a key is down in two consecutive reports, it should
         * only be interpreted as one keypress.
         */
        for (i = 2; i < 8; i++)
        {
            uchar usage_id = data[i];
            char  seq[3];
            int   slen, k;

            if (0 == usage_id)
                continue;
            if (NULL != memchr(kbd->recent_usage_ids, usage_id, 6))
                continue;                       /* key held from last report   */

            /* Arrow keys -> ANSI escape ESC [ A/B/C/D so the line editor
             * (shell_readline) sees them for history recall / cursor move.
             * HID usage: 0x4F right, 0x50 left, 0x51 down, 0x52 up. */
            if (usage_id >= 0x4F && usage_id <= 0x52)
            {
                char dir = (usage_id == 0x52) ? 'A'    /* up    -> prev hist  */
                         : (usage_id == 0x51) ? 'B'    /* down  -> next hist  */
                         : (usage_id == 0x4F) ? 'C'    /* right               */
                         :                      'D';   /* left  (0x50)        */
                seq[0] = 0x1b; seq[1] = '['; seq[2] = dir; slen = 3;
            }
            else
            {
                uchar c = keymap[usage_id][mod_idx];
                if (0 == c)
                    continue;
                /* Ctrl-<letter> -> control code (Ctrl-C = 0x03), so a running
                 * program can be interrupted and the shell sees Ctrl-P/N etc. */
                if (data[0] & (HID_BOOT_LEFT_CTRL | HID_BOOT_RIGHT_CTRL))
                {
                    if (c >= 'a' && c <= 'z')      c = c - 'a' + 1;
                    else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
                }
                seq[0] = (char)c; slen = 1;
            }

            for (k = 0; k < slen; k++)
            {
                if (kbd->icount < USBKBD_IBLEN)
                {
                    kbd->in[(kbd->istart + kbd->icount) % USBKBD_IBLEN] = seq[k];
                    kbd->icount++;
                    count++;
                }
                /* else: input ring overrun — drop */
            }

            /* Publish this key as the auto-repeat candidate (the newest key in
             * the report wins).  The repeat thread re-injects it while held. */
            for (k = 0; k < slen && k < 3; k++) g_kbd_repeat_seq[k] = seq[k];
            g_kbd_repeat_len   = slen;
            g_kbd_repeat_usage = usage_id;
            g_kbd_repeat_gen++;                 /* restart the repeat delay     */
        }

        /* Stop repeating once the repeat key is no longer held down. */
        if (g_kbd_repeat_len > 0)
        {
            int still = 0;
            for (i = 2; i < 8; i++)
                if (data[i] != 0 && data[i] == g_kbd_repeat_usage) still = 1;
            if (!still) g_kbd_repeat_len = 0;
        }

        USBKBD_TRACE("Reported %u new characters", count);
        signaln(kbd->isema, count);
        memcpy(kbd->recent_usage_ids, data + 2, 6);
    }
    else
    {
        USBKBD_TRACE("Bad xfer: status=%d, actual_size=%u",
                     req->status, req->actual_size);
    }
    /* Re-arm the interrupt transfer.  On SUCCESS, re-submit immediately for
     * low typing latency.  On an ERROR (notably USB_STATUS_HARDWARE_ERROR from
     * split/transaction errors that the mouse's bus traffic provokes), do NOT
     * re-submit here: blindly re-arming spins at thousands/sec, saturating the
     * shared bus so the keyboard endpoint never recovers (this is exactly the
     * "physical keyboard dies after using the soft keyboard" failure).  Defer
     * the re-arm to a ~25 ms throttle (usbKbdResubmitPending), which breaks the
     * spin and lets the endpoint recover once the bus calms down. */
    if (req->status == USB_STATUS_SUCCESS)
    {
        g_kbd_consec_err = 0;            /* healthy — reset the error run */
        g_kbd_reenum_cnt = 0;           /* and the replug budget         */
        if (USB_STATUS_SUCCESS != usb_submit_xfer_request(req))
            g_kbd_resubmit_fail++;
    }
    else
    {
        /* The transfer errored (e.g. USB_STATUS_HARDWARE_ERROR).  For a
         * low/full-speed keyboard behind the Pi's hub, transfers are SPLIT
         * transactions; if the error struck mid-split the request is left in a
         * COMPLETE-SPLIT state, and blindly re-arming retries that stuck CSPLIT
         * forever (the endpoint then errors on every poll and never recovers —
         * the AIPL-window keyboard wedge).  Reset the split state so the
         * throttled re-arm begins a clean START-SPLIT.  No control transfers
         * are issued, so this cannot disturb the mouse.  (Leave next_data_pid
         * alone to avoid desyncing the data toggle.) */
        req->complete_split = 0;
        req->csplit_retries = 0;
        req->need_sof       = 0;
        g_kbd_intr_req = req;
        g_kbd_consec_err++;
        /* If re-arming has not cleared the error after many tries, the endpoint
         * is hard-wedged: ask the worker thread to software-replug (re-enumerate)
         * the keyboard, and STOP the hot re-arm loop (which only saturates the
         * bus and can starve the mouse).  Otherwise keep the gentle throttled
         * re-arm. */
        if (g_kbd_consec_err >= KBD_ERR_REENUM_THRESH &&
            !g_kbd_need_reenum && g_kbd_reenum_cnt < KBD_MAX_AUTO_REENUM)
        {
            g_kbd_need_reenum      = 1;
            g_kbd_resubmit_pending = 0;
        }
        else if (!g_kbd_need_reenum)
        {
            g_kbd_resubmit_pending = 1;
        }
    }
}

/**
 * Re-arm a keyboard interrupt transfer that a previous error left deferred.
 * Called at a steady, low rate from a bridge thread (kbd_repeat_bridge in
 * system/main.c) so an error storm retries at ~tens/sec instead of hot-
 * spinning at thousands/sec.  Safe to call when nothing is pending.
 */
void usbKbdResubmitPending(void)
{
    /* Throttled re-arm only.  (An earlier CLEAR_FEATURE(ENDPOINT_HALT)
     * recovery here was REMOVED: issuing control transfers every ~250 ms from
     * this thread saturated the shared USB host and took down the mouse too,
     * and it did not actually recover the keyboard — the wedge is a bus-level
     * transaction error, not a device endpoint halt.) */
    if (g_kbd_resubmit_pending && g_kbd_intr_req)
    {
        g_kbd_resubmit_pending = 0;
        g_kbd_err_resubmits++;
        if (USB_STATUS_SUCCESS != usb_submit_xfer_request(g_kbd_intr_req))
            g_kbd_resubmit_fail++;
    }
}

/**
 * Worker-thread tick (called from kbd_repeat_bridge in system/main.c): if the
 * keyboard's endpoint has been hard-wedged long enough that re-arming gave up,
 * software-replug it by re-enumerating the device on its hub port.  Runs in
 * thread context (re-enumeration issues control transfers and sleeps).  After a
 * replug, usbKbdBindDevice() re-arms a fresh interrupt transfer automatically.
 */
void usbKbdRecover(void)
{
    struct usbkbd *kbd = &usbkbds[0];

    if (!g_kbd_need_reenum)
        return;
    g_kbd_need_reenum = 0;
    g_kbd_consec_err  = 0;
    g_kbd_reenum_cnt++;

    if (kbd->initialized && NULL != kbd->intr && NULL != kbd->intr->dev)
        usb_reenumerate_device(kbd->intr->dev);
}

/**
 * Force-recover a wedged USB keyboard.  When the keyboard's interrupt IN
 * transfer has hung (the "keyboard dies after the soft keyboard / AIPL window"
 * bus-level split-transaction error), this drops any stuck buffered input,
 * resets the request's split-transaction state, and queues a clean re-arm
 * through the SAME throttled path the error handler uses (no control transfers
 * are issued, so the mouse is never disturbed).  Exposed to the shell as the
 * `kbd` command.  Returns OK if a re-arm was queued, SYSERR if the keyboard is
 * not initialized.
 */
int usbKbdRevive(void)
{
    irqmask im;
    struct usbkbd *kbd = &usbkbds[0];

    if (!kbd->initialized || NULL == kbd->intr)
        return SYSERR;

    im = disable();
    kbd->icount = 0;                 /* drop any stuck buffered input */
    kbd->istart = 0;
    kbd->intr->complete_split = 0;   /* clean START-SPLIT on the next poll */
    kbd->intr->csplit_retries = 0;
    kbd->intr->need_sof       = 0;
    g_kbd_intr_req         = kbd->intr;
    g_kbd_resubmit_pending = 1;      /* the throttle thread re-arms it */
    restore(im);
    return OK;
}

/**
 * Stronger recovery (shell `kbd hard`).  A plain re-arm cannot clear a
 * persistent USB_STATUS_HARDWARE_ERROR (-3) bus/split fault, so this also
 * RE-INITIALISES the keyboard's HID endpoint: it re-places the keyboard in boot
 * protocol (a SET_PROTOCOL control transfer on EP0, which is separate from the
 * wedged interrupt-IN endpoint), resets the data toggle to DATA0, and re-arms
 * the interrupt transfer immediately (coordinating with the throttle thread so
 * the request is submitted exactly once).  One control transfer on demand — not
 * the 250 ms storm that previously starved the mouse.  Returns OK if re-armed.
 */
#define KBD_REQ_SET_PROTOCOL  0x0B
#define KBD_BOOT_PROTOCOL     0
int usbKbdReviveHard(void)
{
    irqmask im;
    struct usbkbd *kbd = &usbkbds[0];
    struct usb_device *dev;

    if (!kbd->initialized || NULL == kbd->intr || NULL == kbd->intr->dev)
        return SYSERR;
    dev = kbd->intr->dev;

    im = disable();
    g_kbd_resubmit_pending = 0;          /* take ownership from the throttle */
    kbd->icount = 0;
    kbd->istart = 0;
    kbd->intr->complete_split = 0;
    kbd->intr->csplit_retries = 0;
    kbd->intr->need_sof       = 0;
    kbd->intr->next_data_pid  = 0;       /* resync the data toggle to DATA0 */
    restore(im);

    /* Re-place the keyboard in boot-protocol mode (re-inits the HID endpoint).
     * Interface 0 is the HID interface on essentially all boot keyboards. */
    (void)usb_control_msg(dev, NULL, KBD_REQ_SET_PROTOCOL,
                          USB_BMREQUESTTYPE_TYPE_CLASS |
                              USB_BMREQUESTTYPE_DIR_OUT |
                              USB_BMREQUESTTYPE_RECIPIENT_INTERFACE,
                          KBD_BOOT_PROTOCOL, 0, NULL, 0);

    if (USB_STATUS_SUCCESS != usb_submit_xfer_request(kbd->intr))
    {
        g_kbd_resubmit_fail++;
        return SYSERR;
    }
    g_kbd_err_resubmits++;
    return OK;
}

/**
 * Inject a synthetic character into USBKBD0's input ring, exactly as the HID
 * interrupt handler would.  This lets the on-screen soft keyboard deliver its
 * keystrokes through the SAME single-consumer path as the physical keyboard
 * (the gwin_kbd_bridge thread draining getc(USBKBD0)), so only one thread ever
 * calls gwm_feed_key — avoiding the data race that froze keyboard input when
 * the soft keyboard fed the window editors directly from the wm/mouse thread.
 *
 * @param c  character code to inject (0..255)
 */
void usbKbdInject(int c)
{
    irqmask im;
    struct usbkbd *kbd = &usbkbds[0];        /* USBKBD0 — the bridge's device */
    int queued = 0;

    g_kbd_inject_cnt++;

    im = disable();
    if (kbd->initialized && kbd->icount < USBKBD_IBLEN)
    {
        kbd->in[(kbd->istart + kbd->icount) % USBKBD_IBLEN] = (uchar)c;
        kbd->icount++;
        queued = 1;
    }
    restore(im);

    if (queued)
        signaln(kbd->isema, 1);              /* wake the bridge's usbKbdRead */
}
