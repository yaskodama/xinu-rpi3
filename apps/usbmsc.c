/**
 * @file usbmsc.c
 *
 * USB Mass-Storage (Bulk-Only Transport + SCSI) class driver for the Xinu
 * USB host stack, so a USB SD-card reader plugged into the Pi shows up as
 * "/sd".  Provides 512-byte block read/write; the FAT layer (apps/fat.c) is
 * pointed at usbmsc_read_block via fat_set_blkdev() so `ls /sd` reuses all the
 * existing FAT32 directory code.
 *
 *   BBB:   CBW(31) bulk-OUT -> data IN/OUT -> CSW(13) bulk-IN
 *   SCSI:  TEST UNIT READY / INQUIRY / READ CAPACITY(10) / READ(10) / WRITE(10)
 */

#include <usb_core_driver.h>
#include <usb_std_defs.h>
#include <usb_util.h>
#include <semaphore.h>
#include <thread.h>
#include <string.h>
#include <stdio.h>
#include <fat.h>

#define MSC_SUBCLASS_SCSI  0x06
#define MSC_PROTOCOL_BBB   0x50
#define CBW_SIG  0x43425355u            /* "USBC" */
#define CSW_SIG  0x53425355u            /* "USBS" */

static struct usb_device *msc_dev;
static const struct usb_endpoint_descriptor *ep_in, *ep_out;
static volatile int    msc_present_flag;
static unsigned long   msc_nblocks;
static unsigned        msc_bsize = 512;
static unsigned int    msc_tag = 1;

struct __attribute__((packed)) cbw {
    unsigned int  sig, tag, dlen;
    unsigned char flags, lun, cblen, cb[16];
};
struct __attribute__((packed)) csw {
    unsigned int  sig, tag, residue;
    unsigned char status;
};

static volatile int msc_done;
static volatile int msc_wedged;     /* set on a timeout; blocks reuse of the requests */
static void msc_complete(struct usb_xfer_request *req) { (void)req; msc_done = 1; }

/* CRITICAL: this HCD keeps the bulk data toggle (DATA0/DATA1) in the request's
 * next_data_pid, updating it after each transfer.  So we must REUSE one request
 * per endpoint (never re-init it) — otherwise the toggle resets to DATA0 every
 * time and the device NAKs the 2nd+ transfer (timeout).  One persistent request
 * per direction, initialised once, mirrors how smsc9512 drives its bulk pipes. */
static struct usb_xfer_request req_in  __attribute__((aligned(4)));
static struct usb_xfer_request req_out __attribute__((aligned(4)));
static int reqs_inited;

/* Synchronous bulk transfer on @ep, with a ~1.5 s timeout.  Returns bytes
 * transferred, or -1. */
static int msc_bulk(const struct usb_endpoint_descriptor *ep, void *buf, unsigned int len)
{
    struct usb_xfer_request *req;
    int waited;
    if (msc_wedged) return -1;
    if (!reqs_inited) {
        usb_init_xfer_request(&req_in);
        usb_init_xfer_request(&req_out);
        reqs_inited = 1;
    }
    req = ((ep->bEndpointAddress >> 7) == USB_DIRECTION_IN) ? &req_in : &req_out;
    req->dev = msc_dev;
    req->endpoint_desc = ep;
    req->recvbuf = buf;                 /* union with sendbuf */
    req->size = len;
    req->completion_cb_func = msc_complete;
    msc_done = 0;
    if (usb_submit_xfer_request(req) != USB_STATUS_SUCCESS) {
        printf("[/sd] usb submit failed (ep %02x)\n", ep->bEndpointAddress);
        return -1;
    }
    for (waited = 0; !msc_done && waited < 1500; waited += 5) sleep(5);
    if (!msc_done) {
        msc_wedged = 1;
        printf("[/sd] usb bulk TIMEOUT ep %02x len %u\n", ep->bEndpointAddress, len);
        return -1;
    }
    if (req->status != USB_STATUS_SUCCESS) {
        printf("[/sd] usb bulk status=%d ep %02x\n", req->status, ep->bEndpointAddress);
        return -1;
    }
    return (int)req->actual_size;
}

/* Run one SCSI command via Bulk-Only Transport.  datain=1 reads into data,
 * datain=0 writes from data; dlen may be 0.  Returns 0 on success, -1 else. */
static int msc_scsi(const unsigned char *cmd, int cmdlen, void *data, int dlen, int datain)
{
    /* 4-byte aligned for the DWC2 DMA path (single-threaded here, so static
     * buffers are safe and guarantee alignment). */
    static struct cbw c __attribute__((aligned(4)));
    static struct csw s __attribute__((aligned(4)));
    int i;
    if (!msc_dev) return -1;
    memset(&c, 0, sizeof c);
    c.sig = CBW_SIG;
    c.tag = msc_tag++;
    c.dlen = (unsigned)dlen;
    c.flags = datain ? 0x80 : 0x00;
    c.lun = 0;
    c.cblen = (unsigned char)cmdlen;
    for (i = 0; i < cmdlen && i < 16; i++) c.cb[i] = cmd[i];

    if (msc_bulk(ep_out, &c, 31) != 31) return -1;
    if (dlen > 0) {
        int n = msc_bulk(datain ? ep_in : ep_out, data, (unsigned)dlen);
        if (n < 0) return -1;
    }
    if (msc_bulk(ep_in, &s, 13) != 13) return -1;
    if (s.sig != CSW_SIG || s.status != 0) return -1;
    return 0;
}

static int msc_test_unit_ready(void)
{
    unsigned char cmd[6] = { 0x00, 0, 0, 0, 0, 0 };
    return msc_scsi(cmd, 6, 0, 0, 1);
}
static int msc_request_sense(void)
{
    unsigned char cmd[6] = { 0x03, 0, 0, 0, 18, 0 };
    unsigned char sense[18];
    return msc_scsi(cmd, 6, sense, 18, 1);
}
static int msc_read_capacity(void)
{
    unsigned char cmd[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned char cap[8];
    if (msc_scsi(cmd, 10, cap, 8, 1) != 0) return -1;
    msc_nblocks = ((unsigned long)cap[0] << 24) | ((unsigned long)cap[1] << 16)
                | ((unsigned long)cap[2] << 8)  |  (unsigned long)cap[3];     /* last LBA */
    msc_bsize   = ((unsigned)cap[4] << 24) | ((unsigned)cap[5] << 16)
                | ((unsigned)cap[6] << 8)  |  (unsigned)cap[7];
    if (msc_bsize == 0 || msc_bsize > 4096) msc_bsize = 512;
    return 0;
}

/* Lazy device spin-up: TEST UNIT READY + READ CAPACITY.  Done on first access
 * (shell context), NOT in bind_device, so the USB enumeration/hub thread is
 * never blocked by SCSI traffic (which would freeze keyboard/mouse input). */
static volatile int msc_ready;
static int usbmsc_ensure(void)
{
    int tries;
    if (!msc_present_flag) return -1;
    if (msc_ready) return 0;
    /* Spin-up: TEST UNIT READY may return check-condition until the card is
     * ready; clear it with REQUEST SENSE each time (now that the data toggle is
     * maintained, its data-IN phase works). */
    for (tries = 0; tries < 6 && !msc_wedged; tries++) {
        if (msc_test_unit_ready() == 0) break;
        msc_request_sense();
        sleep(60);
    }
    if (msc_read_capacity() != 0) return -1;
    msc_ready = 1;
    return 0;
}

/* ---- public block API (signature matches the FAT block reader) ---- */
int usbmsc_present(void) { return msc_present_flag; }

int usbmsc_read_block(unsigned long lba, void *buf)
{
    unsigned char cmd[10] = { 0x28, 0,
        (unsigned char)(lba >> 24), (unsigned char)(lba >> 16),
        (unsigned char)(lba >> 8),  (unsigned char)lba, 0, 0, 1, 0 };
    if (usbmsc_ensure() != 0) return -1;
    return msc_scsi(cmd, 10, buf, (int)msc_bsize, 1);
}
int usbmsc_write_block(unsigned long lba, const void *buf)
{
    unsigned char cmd[10] = { 0x2A, 0,
        (unsigned char)(lba >> 24), (unsigned char)(lba >> 16),
        (unsigned char)(lba >> 8),  (unsigned char)lba, 0, 0, 1, 0 };
    if (usbmsc_ensure() != 0) return -1;
    return msc_scsi(cmd, 10, (void *)buf, (int)msc_bsize, 0);
}

/* ---- `ls /sd`: list the USB card's FAT32 root via the shared FAT code ---- */
static int usb_ls_cb(const struct fat_dirent *e, void *ctx)
{
    int lf = *(int *)ctx;
    if (lf)
        printf("%c %8lu %s%s\n", e->is_dir ? 'd' : '-', e->size, e->name, e->is_dir ? "/" : "");
    else
        printf("%s%s\n", e->name, e->is_dir ? "/" : "");
    return 0;
}
int usbmsc_fat_list(int long_form)
{
    int r;
    if (!msc_present_flag) {
        printf("ls: /sd: no USB mass-storage device (plug a USB SD-card reader)\n");
        return -1;
    }
    if (usbmsc_ensure() != 0) {
        printf("ls: /sd: USB card not responding (SCSI spin-up failed)\n");
        return -1;
    }
    fat_set_blkdev(usbmsc_read_block, usbmsc_write_block);   /* FAT on the USB card */
    r = fat_mount();
    if (r != 0) printf("ls: /sd: no FAT32 partition / read error\n");
    else        r = fat_list_root(usb_ls_cb, &long_form);
    fat_set_blkdev(0, 0);                         /* restore the on-board SD     */
    return r;
}

/* `cp .. /sd/NAME`: write a file to the USB card's FAT32 root. */
int usbmsc_fat_write(const char *name, const unsigned char *data, unsigned long size)
{
    int r;
    if (!msc_present_flag) { printf("cp: /sd: no USB mass-storage device\n"); return -1; }
    if (usbmsc_ensure() != 0) { printf("cp: /sd: USB card not responding\n"); return -1; }
    fat_set_blkdev(usbmsc_read_block, usbmsc_write_block);
    r = fat_write_root(name, data, size);
    fat_set_blkdev(0, 0);
    return r;
}

/* Point the FAT layer at the USB card (spinning it up first).  The caller must
 * restore the on-board SD with fat_set_blkdev(0,0) when done.  Returns 0/-1. */
int usbmsc_fat_select(void)
{
    if (!msc_present_flag) return -1;
    if (usbmsc_ensure() != 0) return -1;
    fat_set_blkdev(usbmsc_read_block, usbmsc_write_block);
    return 0;
}

/* ---- USB device-driver callbacks ---- */
static usb_status_t usbmsc_bind(struct usb_device *dev)
{
    uint i, j;
    if (msc_present_flag) return USB_STATUS_DEVICE_UNSUPPORTED;   /* one card at a time */

    for (i = 0; i < dev->config_descriptor->bNumInterfaces; i++) {
        struct usb_interface_descriptor *itf = dev->interfaces[i];
        const struct usb_endpoint_descriptor *bin = NULL, *bout = NULL;
        if (itf->bInterfaceClass != USB_CLASS_CODE_MASS_STORAGE) continue;
        if (itf->bInterfaceProtocol != MSC_PROTOCOL_BBB) continue;     /* Bulk-Only */

        for (j = 0; j < itf->bNumEndpoints; j++) {
            struct usb_endpoint_descriptor *ep = dev->endpoints[i][j];
            if ((ep->bmAttributes & 0x3) != USB_TRANSFER_TYPE_BULK) continue;
            if ((ep->bEndpointAddress >> 7) == USB_DIRECTION_IN) bin = ep; else bout = ep;
        }
        if (!bin || !bout) continue;

        /* IMPORTANT: bind runs on the USB enumeration/hub thread.  Do NO SCSI
         * here — just record the device and endpoints and return immediately.
         * Any bulk traffic here would block the hub thread and freeze keyboard
         * / mouse input.  SCSI spin-up happens lazily in usbmsc_ensure(). */
        msc_dev = dev; ep_in = bin; ep_out = bout;
        msc_ready = 0; msc_wedged = 0; reqs_inited = 0;   /* fresh data toggle */
        msc_present_flag = 1;
        kprintf("[usbmsc] USB mass-storage bound (bulk in/out found); SCSI init deferred\r\n");
        return USB_STATUS_SUCCESS;
    }
    return USB_STATUS_DEVICE_UNSUPPORTED;
}

static void usbmsc_unbind(struct usb_device *dev)
{
    if (dev == msc_dev) { msc_present_flag = 0; msc_dev = 0; ep_in = ep_out = 0; }
}

static const struct usb_device_driver usbmsc_driver = {
    .name          = "USB Mass-Storage (SCSI/BBB) Driver",
    .bind_device   = usbmsc_bind,
    .unbind_device = usbmsc_unbind,
};

/* Register the driver with the USB core (call once at boot, before usbinit so
 * a card present at power-on binds during the first enumeration; hot-plugged
 * readers bind later via the hub driver either way). */
void usbmsc_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    usb_register_device_driver(&usbmsc_driver);
}
