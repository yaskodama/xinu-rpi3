/* apps/browser.c —— 機内ブラウザ（Pi 3 版）
 *
 * xinu-rpi5 の system/browser.c と同じ役目を、Embedded Xinu の装置で果たす。
 * Pi 5 は ARP/DNS/TCP を自前で持つが、Pi 3 には Xinu の TCP 装置（能動オープン）と
 * UDP 装置があるので、それらを使う。整形（apps/html.c）と字形（apps/jpfont.c）は
 * 三板で同一ファイル（正典は xinu-rpi5）。
 *
 *   ・起動 10 秒後に airilab.app を取り（英語版）、60 秒ごとに更新する専用スレッド
 *   ・窓の中の [EN] で日英切替、リンクをクリックで辿る、上半分／下半分で頁送り
 *   ・GET /browse[?url=&lang=&raw=1]（webactor.c）で外から同じことができる
 *   ・User-Agent: XinuBrowser/1.0、X-Xinu-Board: Pi3 build …（airilab.app が記録する）
 *
 * メッシュ（無線）は Pi 3 では Xinu の網装置ではない（apps/wifi.c が生で扱う）ので、
 * xinu://mesh は近隣を一覧するだけで、その板のページは開けない。 */
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <device.h>
#include <network.h>
#include <ipv4.h>
#include <udp.h>
#include <tcp.h>
#include <thread.h>
#include <ether.h>
#include <gwm.h>

extern int kprintf(const char *, ...);
extern struct netif netiftab[];
extern const char *kernel_build_id(void);

/* ---- html.c ---- */
extern void html_layout(const char *h, int n, int width_px);
extern int  html_height(void);
extern void html_draw(int x0, int y0, int w, int h, int scroll_y, unsigned int bg);
extern int  html_text(char *dst, int cap);
extern int  html_i18n_apply(const char *h, int n, const char *dict, int dl, char *out, int cap);
extern void html_set_css(const char *css, int n);
extern int  html_find_stylesheet(const char *h, int n, char *out, int cap);
extern int  html_link_at(int x, int y, const char **href);

#define BR_RXCAP 65536
static char br_page[BR_RXCAP];           /* 直近の応答（ヘッダ込み） */
static int  br_page_len;
static const char *br_body; static int br_body_len;
static char br_page_html[65536];         /* いま表示しているページの HTML（日本語のまま） */
static int  br_page_html_len;
static char br_doc[65536];               /* 整形した文書 */
static int  br_doc_len;
static char br_text[16384];              /* 整形結果の文字（/browse の検算用） */
static int  br_text_len;
#define BR_DICTCAP 49152
static char br_dict[BR_DICTCAP];
static int  br_dict_len;
static char br_url[256] = "http://airilab.app/";
static char br_cur_url[256] = "http://airilab.app/";
static char br_css_url[256];
static char br_note[128] = "not fetched yet";
static int  br_status;
static int  br_lang = 0;                 /* 0=en 1=ja */
static int  br_layout_w = 674, br_scroll_px, br_view_h = 400;
static volatile int br_dirty = 1;        /* 窓を描き直す必要がある */
static char br_pending_url[256];
static int  br_pending_lang = -1;
static long br_st_fetch, br_st_fail, br_st_trunc, br_st_retry;
static const char *br_home      = "http://airilab.app/";
static const char *br_home_dict = "http://airilab.app/js/i18n.js";

static int  b_len(const char *s){ int n=0; while(s[n]) n++; return n; }
static int  b_eqn(const char *a, const char *b, int n)
{ for (int i=0;i<n;i++){ char x=a[i],y=b[i]; if(x>='A'&&x<='Z') x=(char)(x+32); if(y>='A'&&y<='Z') y=(char)(y+32); if(x!=y) return 0; } return 1; }
static void b_cpy(char *d, const char *s, int cap)
{ int i=0; while (s[i] && i < cap-1) { d[i]=s[i]; i++; } d[i]=0; }

const char *browser_url(void)  { return br_url; }
const char *browser_text(void) { return br_text; }
int  browser_text_len(void)    { return br_text_len; }
int  browser_status(void)      { return br_status; }
const char *browser_note(void) { return br_note; }
const char *browser_raw(void)  { return br_page; }
int  browser_raw_len(void)     { return br_page_len; }
int  browser_lang(void)        { return br_lang; }

/* ===== 名前を引く（UDP/53、ゲートウェイの DNS に聞く） ================== */
static unsigned short br_dns_id = 0x3100;
static int br_dns_query(const char *name, struct netaddr *out)
{
    struct netaddr dns = netiftab[0].gateway;      /* 家庭用ルータは DNS も兼ねる */
    unsigned char q[512]; int n = 0;
    br_dns_id++;
    q[n++] = (unsigned char)(br_dns_id >> 8); q[n++] = (unsigned char)(br_dns_id & 0xFF);
    q[n++] = 0x01; q[n++] = 0x00; q[n++] = 0; q[n++] = 1;
    q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    { int i = 0; while (name[i] && n < 480) {
        int st = i; while (name[i] && name[i] != '.') i++;
        q[n++] = (unsigned char)(i - st);
        for (int k = st; k < i; k++) q[n++] = (unsigned char)name[k];
        if (name[i] == '.') i++; }
      q[n++] = 0; }
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;

    int dev = udpAlloc();
    if (SYSERR == dev) return 0;
    if (SYSERR == open(dev, &netiftab[0].ip, &dns, 0, 53)) { udptab[dev - UDP0].state = UDP_FREE; return 0; }
    control(dev, UDP_CTRL_SETFLAG, UDP_FLAG_NOBLOCK, 0);
    int ok = 0;
    for (int t = 0; t < 3 && !ok; t++) {              /* UDP なので再送する */
        write(dev, q, n);
        for (int w = 0; w < 60 && !ok; w++) {
            static unsigned char r[512];
            int rn = read(dev, r, sizeof r);
            if (rn > 12 && r[0] == q[0] && r[1] == q[1]) {
                int an = (r[6] << 8) | r[7];
                int p = 12;
                while (p < rn && r[p]) { if (r[p] >= 0xC0) { p += 2; goto qend; } p += r[p] + 1; }
                p++;
            qend:
                p += 4;
                for (int a = 0; a < an && p + 12 <= rn; a++) {
                    if (r[p] >= 0xC0) p += 2; else { while (p < rn && r[p]) p += r[p] + 1; p++; }
                    int type = (r[p] << 8) | r[p+1];
                    int dl = (r[p+8] << 8) | r[p+9];
                    p += 10;
                    if (type == 1 && dl == 4 && p + 4 <= rn) {
                        out->type = NETADDR_IPv4; out->len = IPv4_ADDR_LEN;
                        for (int i = 0; i < 4; i++) out->addr[i] = r[p+i];
                        ok = 1; break;
                    }
                    p += dl;
                }
                if (!ok) break;                        /* 応答はあったが A が無い */
            } else sleep(50);
        }
    }
    close(dev);
    return ok;
}

/* ===== HTTP GET（Xinu の TCP 装置） ====================================== */
static long br_content_length(const char *h, int n)
{
    const char *k = "content-length:"; int kl = b_len(k);
    for (int i = 0; i + kl < n; i++) {
        if (!b_eqn(h + i, k, kl)) continue;
        int p = i + kl; while (p < n && h[p] == ' ') p++;
        long v = 0; int d = 0;
        while (p < n && h[p] >= '0' && h[p] <= '9') { v = v * 10 + (h[p] - '0'); p++; d = 1; }
        return d ? v : -1;
    }
    return -1;
}

/* 戻り値: 受信バイト数（<=0 は失敗、br_note に理由） */
static int browser_fetch1(const char *url)
{
    char host[128], path[192];
    const char *p = url;
    if (b_eqn(p, "http://", 7)) p += 7;
    else if (b_eqn(p, "https://", 8)) { b_cpy(br_note, "https は未対応（平文 http で開く）", sizeof br_note); br_status = -10; return -10; }
    { int i = 0; while (p[i] && p[i] != '/' && i < 127) { host[i] = p[i]; i++; } host[i] = 0; p += i; }
    if (*p == 0) b_cpy(path, "/", sizeof path); else b_cpy(path, p, sizeof path);
    b_cpy(br_url, url, sizeof br_url);

    struct netaddr dst; int port = 80;
    { char *c = host; while (*c && *c != ':') c++; if (*c == ':') { *c = 0; port = 0; c++; while (*c >= '0' && *c <= '9') port = port * 10 + (*c++ - '0'); if (!port) port = 80; } }
    if (SYSERR == dot2ipv4(host, &dst)) {
        static char cache_name[128]; static struct netaddr cache_ip; static int cache_ok = 0;
        if (br_dns_query(host, &dst)) { b_cpy(cache_name, host, sizeof cache_name); cache_ip = dst; cache_ok = 1; }
        else if (cache_ok && b_eqn(host, cache_name, b_len(host) + 1)) dst = cache_ip;   /* 前回の答えで進む */
        else { b_cpy(br_note, "名前を引けませんでした（DNS 応答なし）", sizeof br_note); br_status = -4; return -4; }
    }

    int dev = tcpAlloc();
    if (SYSERR == dev) { b_cpy(br_note, "TCP を確保できません", sizeof br_note); br_status = -1; return -1; }
    if (SYSERR == open(dev, &netiftab[0].ip, &dst, NULL, port, TCP_ACTIVE)) {
        close(dev);
        b_cpy(br_note, "接続できません（SYN に応答なし）", sizeof br_note); br_status = -3; return -3;
    }
    static char req[512]; int n = 0;
    const char *g = "GET ";           for (int i=0; g[i]; i++) req[n++] = g[i];
    for (int i = 0; path[i]; i++)     req[n++] = path[i];
    const char *h = " HTTP/1.1\r\nHost: "; for (int i=0; h[i]; i++) req[n++] = h[i];
    for (int i = 0; host[i]; i++)     req[n++] = host[i];
    const char *e = "\r\nUser-Agent: XinuBrowser/1.0\r\nX-Xinu-Board: Pi3 build ";
    for (int i = 0; e[i]; i++)        req[n++] = e[i];
    { const char *b = kernel_build_id(); for (int i = 0; b[i] && n < 440; i++) req[n++] = b[i]; }
    const char *e2 = "\r\nAccept: text/html\r\nConnection: close\r\n\r\n";
    for (int i = 0; e2[i]; i++)       req[n++] = e2[i];
    if (SYSERR == write(dev, req, n)) { close(dev); b_cpy(br_note, "送れません", sizeof br_note); br_status = -3; return -3; }

    /* ★ Xinu の tcpRead は「頼んだ長さが揃うまで」戻らない。相手の FIN が先に
       届いて残りが頼んだ長さより短いと、永久に待つ（実機で 40,960 バイトで止まった）。
       なのでヘッダは 1 バイトずつ読んで Content-Length を知り、本文は「残り」を
       超えない長さで読む。Content-Length が無ければ 1 バイトずつ閉じるまで。 */
    br_page_len = 0;
    { int hdr_done = 0;
      while (br_page_len < BR_RXCAP - 1 && !hdr_done) {
          int got = read(dev, br_page + br_page_len, 1);
          if (got <= 0) break;
          br_page_len += got;
          if (br_page_len >= 4 && br_page[br_page_len-4]=='\r' && br_page[br_page_len-3]=='\n' &&
              br_page[br_page_len-2]=='\r' && br_page[br_page_len-1]=='\n') hdr_done = 1;
      }
      long want = hdr_done ? br_content_length(br_page, br_page_len) : -1;
      if (hdr_done && want >= 0) {
          long remain = want;
          while (remain > 0 && br_page_len < BR_RXCAP - 1) {
              int room = BR_RXCAP - 1 - br_page_len;
              int ask = remain > 4096 ? 4096 : (int)remain; if (ask > room) ask = room;
              int got = read(dev, br_page + br_page_len, (uint)ask);
              if (got <= 0) break;
              br_page_len += got; remain -= got;
          }
      } else if (hdr_done) {
          for (;;) { if (br_page_len >= BR_RXCAP - 1) break;
                     int got = read(dev, br_page + br_page_len, 1); if (got <= 0) break; br_page_len += got; }
      } }
    close(dev);
    br_page[br_page_len] = 0;
    if (br_page_len <= 0) { b_cpy(br_note, "本文が空です", sizeof br_note); br_status = 0; return 0; }

    { const char *body = br_page; int bl = br_page_len;
      for (int i = 0; i + 3 < br_page_len; i++)
        if (br_page[i]=='\r'&&br_page[i+1]=='\n'&&br_page[i+2]=='\r'&&br_page[i+3]=='\n') { body = br_page + i + 4; bl = br_page_len - (i + 4); break; }
      long want = br_content_length(br_page, (int)(body - br_page));
      if (want >= 0 && (long)bl < want) {
          b_cpy(br_note, "本文が途中で切れました", sizeof br_note);
          br_st_trunc++; br_status = -5; return -5; }
      br_body = body; br_body_len = bl; }
    b_cpy(br_note, "ok", sizeof br_note);
    br_status = br_page_len;
    return br_page_len;
}
static int browser_fetch_raw(const char *url)
{
    br_st_fetch++;
    int r = browser_fetch1(url);
    if (r == -5 || r == -3) { br_st_retry++; r = browser_fetch1(url); }
    if (r <= 0) br_st_fail++;
    return r;
}

/* ===== 表示 ============================================================== */
extern int g_force_redraw;
extern void gwm_request_repaint(void);
static void br_present(const char *html, int n)
{
    if (n > (int)sizeof br_doc - 1) n = (int)sizeof br_doc - 1;
    if (html != br_doc) for (int i = 0; i < n; i++) br_doc[i] = html[i];
    br_doc[n] = 0; br_doc_len = n;
    html_layout(br_doc, br_doc_len, br_layout_w);
    br_text_len = html_text(br_text, sizeof br_text);
    br_scroll_px = 0;
    br_dirty = 1; gwm_request_repaint();
}
static void br_present_lang(void)
{
    if (br_page_html_len <= 0) return;
    if (br_lang == 1 || br_dict_len <= 0) { br_present(br_page_html, br_page_html_len); return; }
    static char en[65536];
    int el = html_i18n_apply(br_page_html, br_page_html_len, br_dict, br_dict_len, en, sizeof en);
    if (el <= 0) { br_present(br_page_html, br_page_html_len); return; }
    br_present(en, el);
}
void browser_set_lang(int ja)
{
    br_lang = ja ? 1 : 0;
    br_present_lang();
    b_cpy(br_note, br_lang ? "ok (ja)" : "ok (en)", sizeof br_note);
}

static void br_resolve_url(const char *base, const char *href, char *out, int cap)
{
    int o = 0;
    if (b_eqn(href, "http://", 7) || b_eqn(href, "https://", 8) || b_eqn(href, "xinu://", 7)) { b_cpy(out, href, cap); return; }
    const char *p = base; if (b_eqn(p, "http://", 7)) p += 7;
    const char *hs = p; while (*p && *p != '/') p++;
    const char *pre = "http://"; for (int i = 0; pre[i] && o < cap - 1; i++) out[o++] = pre[i];
    for (const char *q = hs; q < p && o < cap - 1; q++) out[o++] = *q;
    if (href[0] == '/') { for (int i = 0; href[i] && o < cap - 1; i++) out[o++] = href[i]; out[o] = 0; return; }
    const char *last = p; for (const char *q = p; *q; q++) if (*q == '/') last = q;
    for (const char *q = p; q <= last && o < cap - 1; q++) out[o++] = *q;
    if (o == 0 || out[o-1] != '/') { if (o < cap - 1) out[o++] = '/'; }
    for (int i = 0; href[i] && o < cap - 1; i++) out[o++] = href[i];
    out[o] = 0;
}

/* 組み込みページ xinu://mesh */
extern int  wifi_connected(void);
extern void wifi_manet_get(int *node, unsigned *rx, unsigned *htx, int *np, unsigned char *peers);
static int br_builtin_mesh(char *out, int cap)
{
    int o = 0;
    #define PUT(s) do { const char *q_ = (s); while (*q_ && o < cap - 1) out[o++] = *q_++; } while (0)
    #define PUTN(v) do { int v_ = (v); char nb_[12]; int k_ = 0; if (v_ == 0) nb_[k_++] = '0'; while (v_ > 0 && k_ < 11) { nb_[k_++] = (char)('0' + v_ % 10); v_ /= 10; } while (k_ > 0 && o < cap - 1) out[o++] = nb_[--k_]; } while (0)
    PUT("<html><body><h1>Xinu mesh</h1>");
    if (!wifi_connected()) PUT("<p>WiFi (IBSS) is not joined. Run <code>wifi adhoc ...</code> first.</p>");
    else {
        int node = 0, np = 0; unsigned rx = 0, htx = 0; unsigned char peers[32];
        wifi_manet_get(&node, &rx, &htx, &np, peers);
        PUT("<p>This board: node "); PUTN(node); PUT(" (10.0.0."); PUTN(node); PUT(")</p>");
        if (np <= 0) PUT("<p>No neighbours heard yet (HELLO every 2 s).</p>");
        else { PUT("<h2>Neighbours</h2><ul>");
               for (int i = 0; i < np && i < 32; i++) { PUT("<li>node "); PUTN(peers[i]); PUT(" - 10.0.0."); PUTN(peers[i]); PUT("</li>"); }
               PUT("</ul><p><i>Pi 3: opening mesh pages is not supported yet (the WiFi is not a Xinu network device).</i></p>"); }
    }
    PUT("<hr><p><a href=\"http://airilab.app/\">Home: airilab.app</a></p></body></html>");
    #undef PUT
    #undef PUTN
    out[o] = 0; return o;
}

/* ページを取り、外部 CSS も取り、控えて、いまの言語で表示する。 */
int browser_fetch_en(const char *page_url, const char *dict_url)
{
    if (b_eqn(page_url, "xinu://", 7)) {
        br_page_html_len = br_builtin_mesh(br_page_html, sizeof br_page_html);
        b_cpy(br_cur_url, page_url, sizeof br_cur_url);
        html_set_css("", 0); br_css_url[0] = 0;
        br_present(br_page_html, br_page_html_len);
        b_cpy(br_url, page_url, sizeof br_url); b_cpy(br_note, "ok (mesh)", sizeof br_note);
        br_status = br_text_len; return br_text_len;
    }
    if (br_dict_len <= 0) {
        int r = browser_fetch_raw(dict_url);
        if (r <= 0) { b_cpy(br_note, "i18n.js を取得できません", sizeof br_note); return -20; }
        br_dict_len = br_body_len < BR_DICTCAP-1 ? br_body_len : BR_DICTCAP-1;
        for (int i = 0; i < br_dict_len; i++) br_dict[i] = br_body[i];
        br_dict[br_dict_len] = 0;
    }
    int r = browser_fetch_raw(page_url);
    if (r <= 0) return r;
    { int plain = 0;
      { const char *k = "content-type:"; int kl = b_len(k); int hl = (int)(br_body - br_page);
        for (int i = 0; i + kl + 10 < hl; i++)
            if (b_eqn(br_page + i, k, kl)) { int q = i + kl; while (q < hl && br_page[q] == ' ') q++; plain = b_eqn(br_page + q, "text/plain", 10); break; } }
      int o = 0, cap = (int)sizeof br_page_html - 1;
      if (plain) { const char *w = "<html><body><pre>"; for (int i = 0; w[i] && o < cap; i++) br_page_html[o++] = w[i]; }
      for (int i = 0; i < br_body_len && o < cap; i++) {
          char c = br_body[i];
          if (plain && c == '<') { if (o + 4 <= cap) { br_page_html[o++]='&'; br_page_html[o++]='l'; br_page_html[o++]='t'; br_page_html[o++]=';'; } continue; }
          if (plain && c == '&') { if (o + 5 <= cap) { br_page_html[o++]='&'; br_page_html[o++]='a'; br_page_html[o++]='m'; br_page_html[o++]='p'; br_page_html[o++]=';'; } continue; }
          br_page_html[o++] = c;
      }
      if (plain) { const char *w = "</pre></body></html>"; for (int i = 0; w[i] && o < cap; i++) br_page_html[o++] = w[i]; }
      br_page_html[o] = 0; br_page_html_len = o; }
    b_cpy(br_cur_url, page_url, sizeof br_cur_url);
    { static char href[256], cssurl[256];
      if (html_find_stylesheet(br_page_html, br_page_html_len, href, sizeof href)) {
          br_resolve_url(page_url, href, cssurl, sizeof cssurl);
          if (!b_eqn(cssurl, br_css_url, b_len(cssurl) + 1)) {
              int c = browser_fetch_raw(cssurl);
              if (c > 0) { html_set_css(br_body, br_body_len); b_cpy(br_css_url, cssurl, sizeof br_css_url); }
              else html_set_css("", 0);
          }
      } else { html_set_css("", 0); br_css_url[0] = 0; } }
    br_present_lang();
    b_cpy(br_url, page_url, sizeof br_url);
    b_cpy(br_note, br_lang ? "ok (ja)" : "ok (en)", sizeof br_note);
    br_status = br_text_len;
    return br_text_len;
}

static int br_have_en = 0;
static int br_fetch_home(void)
{
    const char *u = br_have_en ? br_cur_url : br_home;
    int r = browser_fetch_en(u, br_home_dict);
    if (r <= 0 && !br_have_en) r = browser_fetch_en(u, br_home_dict);
    if (r > 0) { br_have_en = 1; return r; }
    { static char why[128]; b_cpy(why, br_note, sizeof why);
      b_cpy(br_url, br_cur_url, sizeof br_url);
      b_cpy(br_note, br_lang ? "ok (ja) / update failed: " : "ok (en) / update failed: ", sizeof br_note);
      int o = b_len(br_note); for (int i = 0; why[i] && o < (int)sizeof(br_note)-1; i++) br_note[o++] = why[i]; br_note[o] = 0;
      br_dirty = 1; gwm_request_repaint(); }
    br_status = br_text_len;
    return br_text_len;
}

/* HTTP（/browse?url=）とクリックからの要求。専用スレッドが取りに行く。 */
void browser_request_url(const char *url)
{
    b_cpy(br_pending_url, url, sizeof br_pending_url);
    b_cpy(br_note, "queued", sizeof br_note);
}
void browser_request_lang(int ja) { br_pending_lang = ja ? 1 : 0; }

/* ===== 専用スレッド ====================================================== */
thread browser_main(void)
{
    sleep(10000);                                  /* 網が上がるのを待つ */
    kprintf("[browser] fetching %s\r\n", br_home);
    br_fetch_home();
    kprintf("[browser] %s\r\n", br_note);
    int since = 0;
    for (;;) {
        sleep(500); since += 500;
        if (br_pending_lang >= 0) { browser_set_lang(br_pending_lang); br_pending_lang = -1; }
        if (br_pending_url[0]) {
            static char u[256]; b_cpy(u, br_pending_url, sizeof u); br_pending_url[0] = 0;
            if (b_eqn(u, "https://", 8)) { b_cpy(br_note, "https は未対応（このリンクは開けません）", sizeof br_note); br_dirty = 1; gwm_request_repaint(); continue; }
            int r = browser_fetch_en(u, br_home_dict);
            if (r <= 0) { static char why[128]; b_cpy(why, br_note, sizeof why);
                          b_cpy(br_note, "open failed: ", sizeof br_note);
                          int o = b_len(br_note); for (int i = 0; why[i] && o < (int)sizeof(br_note)-1; i++) br_note[o++] = why[i]; br_note[o] = 0;
                          br_dirty = 1; gwm_request_repaint(); }
            since = 0; continue;
        }
        if (since >= 60000) { since = 0; br_fetch_home(); }
    }
    return OK;
}

/* ===== 窓 ================================================================ */
extern void draw_string_at(int x, int y, const char *s, unsigned int fg, unsigned int bg);
extern void fill_rect(int x, int y, int w, int h, unsigned int c);
extern void video_set_clip(int x, int y, int w, int h);
extern void video_clear_clip(void);
extern int  video_viewport_x(void);
extern int  video_viewport_y(void);

void browser_scroll(int d)
{
    br_scroll_px += d;
    int maxs = html_height() - br_view_h; if (maxs < 0) maxs = 0;
    if (br_scroll_px > maxs) br_scroll_px = maxs;
    if (br_scroll_px < 0) br_scroll_px = 0;
    br_dirty = 1; gwm_request_repaint();
}

void browser_draw_window(window_t *w, unsigned int frame)
{
    (void)frame;
    if (!g_force_redraw && !br_dirty) return;
    br_dirty = 0;
    unsigned int bg = w->content_bg, hi = 0xFF80D0FFU, dim = 0xFF8090A8U;
    int xb = w->x + 8, yb = w->y + WM_TITLEBAR_H + 6;
    int cw = w->width - 26;
    /* 窓の外へはみ出さないよう切り取る（画面座標） */
    video_set_clip(w->x + 1 - video_viewport_x(), w->y + WM_TITLEBAR_H + 1 - video_viewport_y(), w->width - 2, w->height - WM_TITLEBAR_H - 2);
    fill_rect(w->x + 1, w->y + WM_TITLEBAR_H + 1, w->width - 2, w->height - WM_TITLEBAR_H - 2, bg);
    draw_string_at(xb, yb, br_url, hi, bg);
    { char st[96]; int o = 0; const char *n = br_note;
      st[o++] = ' '; st[o++] = ' ';
      for (int i = 0; n[i] && o < 90; i++) st[o++] = (unsigned char)n[i] < 0x80 ? n[i] : '.';
      st[o] = 0; draw_string_at(xb, yb + 10, st, dim, bg); }
    int vh = w->height - WM_TITLEBAR_H - 16 - 26; if (vh < 16) vh = 16;
    br_view_h = vh;
    if (br_layout_w != cw && br_doc_len > 0) { br_layout_w = cw; html_layout(br_doc, br_doc_len, cw); }
    if (br_doc_len > 0) html_draw(xb, yb + 26, cw, vh, br_scroll_px, bg);
    else draw_string_at(xb, yb + 26, "(no page loaded yet)", dim, bg);
    { int th = html_height(); if (th > vh && th > 0) {
        int bh = vh * vh / th; if (bh < 8) bh = 8;
        int by = yb + 26 + (vh - bh) * br_scroll_px / (th - vh > 0 ? th - vh : 1);
        fill_rect(w->x + w->width - 6, yb + 26, 3, vh, 0xFF1A2230U);
        fill_rect(w->x + w->width - 6, by, 3, bh, 0xFF60FFC0U); } }
    video_clear_clip();
}

/* 窓の中のクリック（窓の左上からの座標）。処理したら 1。 */
int browser_click(window_t *w, int lx, int ly)
{
    int top = WM_TITLEBAR_H + 6 + 26;
    if (ly >= WM_TITLEBAR_H && ly < top) { browser_request_url("xinu://mesh"); return 1; }
    if (ly >= top && lx >= 8 && lx < w->width - 12) {
        const char *href = 0;
        int k = html_link_at(lx - 8, ly - top + br_scroll_px, &href);
        if (k == 2) { br_pending_lang = !br_lang; return 1; }
        if (k == 1 && href) {
            if (href[0] == '#') { br_scroll_px = 0; br_dirty = 1; gwm_request_repaint(); return 1; }
            static char u[256]; br_resolve_url(br_cur_url, href, u, sizeof u);
            browser_request_url(u); br_dirty = 1; gwm_request_repaint(); return 1;
        }
    }
    int mid = top + (w->height - top) / 2;
    int page = br_view_h - 24; if (page < 40) page = 40;
    browser_scroll(ly < mid ? -page : page);
    return 1;
}

/* /browse の net= 行 */
int browser_netinfo(char *d, int cap)
{
    int o = 0;
    #define PS(s) do { const char *q=(s); while(*q && o<cap-1) d[o++]=*q++; } while(0)
    #define PL(x) do { long v=(x); char nb[16]; int k=0; if(v==0) nb[k++]='0'; while(v>0){ nb[k++]=(char)('0'+v%10); v/=10; } while(k>0 && o<cap-1) d[o++]=nb[--k]; } while(0)
    { unsigned char *ip = netiftab[0].ip.addr, *gw = netiftab[0].gateway.addr;
      PS("ip="); PL(ip[0]); PS("."); PL(ip[1]); PS("."); PL(ip[2]); PS("."); PL(ip[3]);
      PS(" gw="); PL(gw[0]); PS("."); PL(gw[1]); PS("."); PL(gw[2]); PS("."); PL(gw[3]); }
    PS(" fetch="); PL(br_st_fetch); PS(" fail="); PL(br_st_fail); PS(" trunc="); PL(br_st_trunc); PS(" retry="); PL(br_st_retry);
    #undef PS
    #undef PL
    d[o] = 0; return o;
}
