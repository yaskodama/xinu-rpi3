/**
 * @file abcl_xinu_net.c
 *
 * AIPL <-> Xinu TCP/IP bridge (N1).
 *
 * Built-ins (callable from AIPL via aipl2c --xinu):
 *
 *   net_init(timeout_sec)               -> 1 ok / 0 fail
 *      Brings the smc91c111 interface up, runs the DHCP client to
 *      acquire IP/mask/gateway, then netUp().  Caches the local IP
 *      for use as the source address on subsequent connects.
 *
 *   net_connect(a, b, c, d, port)       -> fd  (>=0) on success / -1
 *      Allocates a TCP tcb and opens it actively to a.b.c.d:port.
 *      a/b/c/d are 8-bit integers passed as separate AIPL ints to
 *      avoid 32-bit packing (and because AIPL has no hex literals).
 *
 *   net_send(fd, string)                -> bytes written, or -1
 *   net_recv(fd, max)                   -> bytes read,    or -1
 *      Synchronous TCP I/O.  recv prints the received payload to
 *      the serial console as `[aipl] net_recv data=...` so smokes
 *      can verify without parsing the AIPL value back out.
 *
 *   net_close(fd)                       -> 0
 */

#include <stddef.h>
#include <kernel.h>
#include <device.h>
#include <ipv4.h>
#include <network.h>
#include <ether.h>
#include <tcp.h>
#include <thread.h>
#include <string.h>
#include <conf.h>

/* Must match the value_t layout in c_translator.ml's runtime_prelude_xinu. */
typedef enum { V_NIL, V_INT, V_FLOAT, V_STR, V_OBJ } vtag_t;
typedef struct {
    vtag_t      tag;
    long        i;
    double      f;
    const char *s;
    int         obj_id;
} value_t;

static value_t v_int(long n)
{
    value_t v;
    v.tag    = V_INT;
    v.i      = n;
    v.f      = 0;
    v.s      = 0;
    v.obj_id = 0;
    return v;
}

int           g_net_up = 0;
struct netaddr g_local_ip;
static struct netaddr g_local_mask;
static struct netaddr g_gateway;

/* ---------- abcl_net_autoinit — kernel-side helper -------------------
 *
 * Same effect as the AIPL net_init() builtin but callable from C boot
 * code (system/main.c) so the HTTP listener can come up regardless of
 * whether the AIPL program calls net_init itself. */
int abcl_net_autoinit(void)
{
    struct netaddr ip, mask, gw;
    if (g_net_up) {
        return 1;
    }
#ifdef ETH0
    if (SYSERR == dot2ipv4("10.0.2.15",    &ip)
     || SYSERR == dot2ipv4("255.255.255.0", &mask)
     || SYSERR == dot2ipv4("10.0.2.2",      &gw))
    {
        kprintf("[abcl_net] autoinit dot2ipv4 failed\r\n");
        return 0;
    }
    if (OK != netUp(ETH0, &ip, &mask, &gw)) {
        kprintf("[abcl_net] autoinit netUp failed\r\n");
        return 0;
    }
    g_local_ip   = ip;
    g_local_mask = mask;
    g_gateway    = gw;
    g_net_up     = 1;
    kprintf("[abcl_net] autoinit ok ip=%d.%d.%d.%d gw=%d.%d.%d.%d\r\n",
            ip.addr[0], ip.addr[1], ip.addr[2], ip.addr[3],
            gw.addr[0], gw.addr[1], gw.addr[2], gw.addr[3]);
    return 1;
#else
    return 0;
#endif
}

/* ---------- net_init ---------- */

value_t net_init(int n_args, value_t *args)
{
    struct netaddr ip, mask, gw;

    (void)n_args; (void)args;

    if (g_net_up) {
        kprintf("[aipl] net_init already-up\r\n");
        return v_int(1);
    }

#ifdef ETH0
    /* arm-qemu's xinu.conf doesn't enable WITH_DHCPC, and QEMU's
     * SLIRP user-mode network hands out 10.0.2.15 / 24 / gw 10.0.2.2
     * deterministically anyway.  Bring the interface up with that
     * static config so net_connect can use g_local_ip as the source
     * address.  ETH0 itself is already open()'d by system/main.c. */
    if (SYSERR == dot2ipv4("10.0.2.15",    &ip)
     || SYSERR == dot2ipv4("255.255.255.0", &mask)
     || SYSERR == dot2ipv4("10.0.2.2",      &gw))
    {
        kprintf("[aipl] net_init: dot2ipv4 failed\r\n");
        return v_int(0);
    }
    if (OK != netUp(ETH0, &ip, &mask, &gw)) {
        kprintf("[aipl] net_init netUp failed\r\n");
        return v_int(0);
    }
    g_local_ip   = ip;
    g_local_mask = mask;
    g_gateway    = gw;
    g_net_up     = 1;
    kprintf("[aipl] net_init ok ip=%d.%d.%d.%d gw=%d.%d.%d.%d\r\n",
            ip.addr[0], ip.addr[1], ip.addr[2], ip.addr[3],
            gw.addr[0], gw.addr[1], gw.addr[2], gw.addr[3]);
    return v_int(1);
#else
    kprintf("[aipl] net_init: ETH0 not configured\r\n");
    return v_int(0);
#endif
}

/* ---------- net_connect ---------- */

value_t net_connect(int n_args, value_t *args)
{
#ifdef NTCP
    int a0, a1, a2, a3, port;
    struct netaddr dst;
    int dev;

    if (n_args < 5) return v_int(-1);
    if (!g_net_up) {
        kprintf("[aipl] net_connect: net not up — call net_init first\r\n");
        return v_int(-1);
    }
    a0   = (int)args[0].i;
    a1   = (int)args[1].i;
    a2   = (int)args[2].i;
    a3   = (int)args[3].i;
    port = (int)args[4].i;

    dst.type = NETADDR_IPv4;
    dst.len  = IPv4_ADDR_LEN;
    dst.addr[0] = (uchar)a0;
    dst.addr[1] = (uchar)a1;
    dst.addr[2] = (uchar)a2;
    dst.addr[3] = (uchar)a3;

    dev = tcpAlloc();
    if (SYSERR == dev) {
        kprintf("[aipl] net_connect tcpAlloc failed\r\n");
        return v_int(-1);
    }
    if (SYSERR == open(dev, &g_local_ip, &dst, NULL, (int)(ushort)port,
                       TCP_ACTIVE)) {
        kprintf("[aipl] net_connect open failed to %d.%d.%d.%d:%d\r\n",
                a0, a1, a2, a3, port);
        close(dev);
        return v_int(-1);
    }
    kprintf("[aipl] net_connect ok fd=%d to=%d.%d.%d.%d:%d\r\n",
            dev, a0, a1, a2, a3, port);
    return v_int(dev);
#else
    (void)n_args; (void)args;
    kprintf("[aipl] net_connect: TCP not configured\r\n");
    return v_int(-1);
#endif
}

/* ---------- net_send ---------- */

value_t net_send(int n_args, value_t *args)
{
    int fd, len, wr;
    const char *s;

    if (n_args < 2) return v_int(-1);
    fd = (int)args[0].i;
    s  = (args[1].tag == V_STR && args[1].s != NULL) ? args[1].s : "";
    len = strlen(s);
    if (len == 0) {
        kprintf("[aipl] net_send fd=%d bytes=0 (empty)\r\n", fd);
        return v_int(0);
    }
    wr = write(fd, (void *)s, (uint)len);
    if (wr < 0) {
        kprintf("[aipl] net_send fd=%d write failed\r\n", fd);
        return v_int(-1);
    }
    kprintf("[aipl] net_send fd=%d bytes=%d\r\n", fd, wr);
    return v_int(wr);
}

/* ---------- net_recv ---------- */

value_t net_recv(int n_args, value_t *args)
{
    int fd, maxlen, got, i;
    char buf[256];

    if (n_args < 1) return v_int(-1);
    fd = (int)args[0].i;
    maxlen = (n_args >= 2 && args[1].tag == V_INT) ? (int)args[1].i
                                                   : (int)sizeof(buf) - 1;
    if (maxlen > (int)sizeof(buf) - 1) maxlen = sizeof(buf) - 1;
    if (maxlen < 1) maxlen = 1;

    got = read(fd, buf, (uint)maxlen);
    if (got < 0) {
        kprintf("[aipl] net_recv fd=%d read failed\r\n", fd);
        return v_int(-1);
    }
    /* Sanitise newlines for the marker line, keep null-terminator. */
    for (i = 0; i < got; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') buf[i] = '?';
    }
    buf[got] = 0;
    kprintf("[aipl] net_recv fd=%d bytes=%d data=%s\r\n", fd, got, buf);
    return v_int(got);
}

/* ---------- net_close ---------- */

value_t net_close(int n_args, value_t *args)
{
    int fd;
    if (n_args < 1) return v_int(0);
    fd = (int)args[0].i;
    close(fd);
    kprintf("[aipl] net_close fd=%d\r\n", fd);
    return v_int(0);
}
