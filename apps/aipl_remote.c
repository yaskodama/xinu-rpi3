/* apps/aipl_remote.c — AIPL の remote("host:port","actor") を Pi 3 で実現する。
 *
 * Pi 4 / Pi 5 は TCP が受け側しか無いのでフレームを自作したが、この板には
 * 本物の Xinu ネットワーク層（UDP デバイス）がある。素直にそれを使う。
 * 電文は 3 台で共通の ASCII 一行:
 *
 *   要求  Q <reqid> <actor> <method> <arg...>\n
 *   応答  R <reqid> <値>\n
 *
 * 受け口は UDP/9010 に常駐する番人プロセス。送り側は別のポートを取って
 * 出す（同じ局所ポートに 2 本開くと振り分けが曖昧になるため）。相手は
 * 要求の送信元ポートへ返すので、これで噛み合う。
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <device.h>
#include <network.h>
#include <udp.h>
#include <thread.h>
#include <ether.h>

#define AIPL_PORT 9010

extern int vm_remote_call(const char *path, const char *meth, const char *arg,
                          char *out, int cap);
extern syscall sleep(unsigned);

/* ---- 電文の組み立て・取り出し -------------------------------------------- */
static int put(char *d, int at, int cap, const char *s)
{ while (*s && at < cap - 1) d[at++] = *s++; d[at] = 0; return at; }

static int build_q(char *out, int cap, int id, const char *actor,
                   const char *meth, const char *arg)
{
    int at = 0;
    at = put(out, at, cap, "Q ");
    { char n[16]; sprintf(n, "%d", id); at = put(out, at, cap, n); }
    at = put(out, at, cap, " ");
    at = put(out, at, cap, actor);
    at = put(out, at, cap, " ");
    at = put(out, at, cap, meth);
    at = put(out, at, cap, " ");
    at = put(out, at, cap, arg ? arg : "");
    at = put(out, at, cap, "\n");
    return at;
}

/* "192.168.3.101" / "192.168.3.101:9010" を解く */
static int parse_hostport(const char *h, struct netaddr *ip, ushort *port)
{
    char host[32]; int i = 0, p = 0;
    *port = AIPL_PORT;
    while (h[i] && h[i] != ':' && i < (int)sizeof host - 1) { host[i] = h[i]; i++; }
    host[i] = 0;
    if (h[i] == ':') { i++; while (h[i] >= '0' && h[i] <= '9') { p = p*10 + (h[i]-'0'); i++; }
                       if (p > 0 && p < 65536) *port = (ushort)p; }
    return (SYSERR == dot2ipv4(host, ip)) ? -1 : 0;
}

/* ---- 計器 ----------------------------------------------------------------
   番人が黙っているとき、どこまで進んだのかを外から読む。
   「板が生きている」ことと「電文が通っている」ことは別なので、
   推測でコードを直す前にここを見る。GET /version が出す。 */
static long g_state = 0;   /* 0=未起動 1=udpAlloc失敗 2=open失敗 3=待ち受け中 */
static unsigned long g_n_read, g_n_short, g_n_notq, g_n_q, g_n_reply, g_n_tx_q;
static long g_last_n = -1, g_last_plen = -1, g_last_c0 = -1;
void aipl_remote_stats(long *o)
{ o[0]=g_state; o[1]=(long)g_n_read; o[2]=(long)g_n_short; o[3]=(long)g_n_notq;
  o[4]=(long)g_n_q; o[5]=(long)g_n_reply; o[6]=(long)g_n_tx_q;
  o[7]=g_last_n; o[8]=g_last_plen; o[9]=g_last_c0; }

/* ---- 送り側 -------------------------------------------------------------- */
static int g_next_id = 1;

static int remote_xfer(const char *hostport, const char *actor, const char *meth,
                       const char *arg, int timeout_ms, char *out, int cap)
{
    struct netaddr rip;
    ushort rport;
    int dev, id, n, waited = 0, rc = -1;
    char q[288];
    static char rbuf[512];

    if (out && cap > 0) out[0] = 0;
    if (parse_hostport(hostport, &rip, &rport) != 0) return -1;

    dev = udpAlloc();
    if (SYSERR == dev) return -1;
    /* 局所ポート 0 = 任意。9010 を取らないのは、受け口の番人と食い合うため。 */
    if (SYSERR == open(dev, &netiftab[0].ip, &rip, 0, rport)) {
        udptab[dev - UDP0].state = UDP_FREE; return -1;
    }

    id = g_next_id++;
    if (g_next_id > 1000000) g_next_id = 1;
    n = build_q(q, sizeof q, id, actor, meth, arg);
    if (SYSERR == write(dev, q, n)) goto out_close;
    g_n_tx_q++;

    if (timeout_ms <= 0) { rc = 0; goto out_close; }   /* 返事を待たない送信 */

    control(dev, UDP_CTRL_SETFLAG, UDP_FLAG_NOBLOCK, 0);
    while (waited < timeout_ms) {
        n = read(dev, rbuf, sizeof rbuf - 1);
        if (n > 0) {
            rbuf[n] = 0;
            { int i = 0; while (rbuf[i]) { if (rbuf[i]=='\n'||rbuf[i]=='\r') { rbuf[i]=0; break; } i++; } }
            if (rbuf[0] == 'R' && rbuf[1] == ' ') {
                int p = 2, gotid = 0;
                while (rbuf[p] >= '0' && rbuf[p] <= '9') { gotid = gotid*10 + (rbuf[p]-'0'); p++; }
                if (rbuf[p] == ' ') p++;
                if (gotid == id) {
                    int k = 0; while (rbuf[p] && k < cap - 1) out[k++] = rbuf[p++];
                    out[k] = 0; rc = 0; goto out_close;
                }
            }
            continue;                    /* 自分宛でない返事。読み飛ばす */
        }
        sleep(5); waited += 5;
    }
    rc = -2;                             /* 期限切れ */
out_close:
    close(dev);
    return rc;
}

int aipl_remote_send(const char *hostport, const char *actor,
                     const char *meth, const char *arg)
{ return remote_xfer(hostport, actor, meth, arg, 0, NULL, 0); }

int aipl_remote_call(const char *hostport, const char *actor, const char *meth,
                     const char *arg, int timeout_ms, char *out, int cap)
{ return remote_xfer(hostport, actor, meth, arg, timeout_ms > 0 ? timeout_ms : 2000, out, cap); }

/* ---- 受け口の番人 --------------------------------------------------------
 * UDP/9010 に常駐して、来た要求をこの板の公開アクターへ渡す。
 * PASSIVE で開くと、読んだ塊の先頭に送り主の擬似ヘッダが付いてくるので、
 * ARP も相手表も要らずに返せる。 */
thread aipl_remote_daemon(void)
{
    int dev, n, i;
    static char buf[1024];

    for (i = 0; i < 60; i++) {
        if (ethertab[0].state == ETH_STATE_UP) break;
        sleep(500);
    }
    dev = udpAlloc();
    if (SYSERR == dev) { g_state = 1; kprintf("[remote] udpAlloc failed\r\n"); return SYSERR; }
    if (SYSERR == open(dev, &netiftab[0].ip, NULL, AIPL_PORT, 0)) {
        g_state = 2;
        kprintf("[remote] open(:%d) failed\r\n", AIPL_PORT);
        udptab[dev - UDP0].state = UDP_FREE; return SYSERR;
    }
    control(dev, UDP_CTRL_SETFLAG, UDP_FLAG_PASSIVE, 0);
    g_state = 3;
    kprintf("[remote] AIPL remote listening on UDP %d\r\n", AIPL_PORT);

    for (;;) {
        n = read(dev, buf, sizeof buf - 1);
        g_n_read++; g_last_n = n;
        if (n <= (int)sizeof(struct udpPseudoHdr)) { g_n_short++; continue; }
        buf[n] = 0;

        { struct udpPseudoHdr *ph = (struct udpPseudoHdr *)buf;
          struct udpPkt *pkt = (struct udpPkt *)(buf + sizeof(struct udpPseudoHdr));
          struct netaddr src;
          /* ★ udpRecv が srcPort / dstPort / len を「受け取った時点で」ホスト順へ
                直してから積んでいる（network/udp/udpRecv.c）。ここで net2hs を
                かけると二度入れ替わって長さが化け、番人は黙って読み捨てる ――
                実機で Pi 3 だけが応答しなかったのはこれ。 */
          ushort srcpt = pkt->srcPort;
          char *p = (char *)pkt->data;
          int plen = (int)pkt->len - UDP_HDR_LEN;
          g_last_plen = plen;
          if (plen < 2) { g_n_short++; continue; }
          p[(plen < (int)(sizeof buf - sizeof(struct udpPseudoHdr) - UDP_HDR_LEN - 1))
            ? plen : 0] = 0;
          { int k = 0; while (p[k]) { if (p[k]=='\n'||p[k]=='\r') { p[k]=0; break; } k++; } }

          src.type = NETADDR_IPv4; src.len = IPv4_ADDR_LEN;
          memcpy(src.addr, ph->srcIp, IPv4_ADDR_LEN);

          g_last_c0 = (long)(unsigned char)p[0];
          if (!(p[0] == 'Q' && p[1] == ' ')) { g_n_notq++; continue; }
          g_n_q++;

          { int q = 2, id = 0, k;
            char actor[40], meth[40], val[192], reply[224];
            while (p[q] >= '0' && p[q] <= '9') { id = id*10 + (p[q]-'0'); q++; }
            if (p[q] == ' ') q++;
            k = 0; while (p[q] && p[q] != ' ' && k < (int)sizeof actor - 1) actor[k++] = p[q++];
            actor[k] = 0;
            if (p[q] == ' ') q++;
            k = 0; while (p[q] && p[q] != ' ' && k < (int)sizeof meth - 1) meth[k++] = p[q++];
            meth[k] = 0;
            if (p[q] == ' ') q++;

            /* 公開名は "/echo" のように '/' 始まりで登録されている。
               電文では '/' を書かせないので、両方の書き方を試す。 */
            { char withslash[42]; withslash[0] = '/';
              { int t = 0; while (actor[t] && t < 40) { withslash[t+1] = actor[t]; t++; }
                withslash[t+1] = 0; }
              if (!vm_remote_call(actor, meth, p + q, val, sizeof val)
               && !vm_remote_call(withslash, meth, p + q, val, sizeof val))
                  put(val, 0, sizeof val, "err"); }

            { int at = 0;
              at = put(reply, at, sizeof reply, "R ");
              { char nb[16]; sprintf(nb, "%d", id); at = put(reply, at, sizeof reply, nb); }
              at = put(reply, at, sizeof reply, " ");
              at = put(reply, at, sizeof reply, val);
              at = put(reply, at, sizeof reply, "\n");
              control(dev, UDP_CTRL_BIND, srcpt, (long)&src);
              if (SYSERR != write(dev, reply, at)) g_n_reply++;
              control(dev, UDP_CTRL_BIND, 0, (long)NULL); } }
        }
    }
    return OK;
}
