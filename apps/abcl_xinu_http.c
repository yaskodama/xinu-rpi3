/**
 * @file abcl_xinu_http.c
 *
 * Minimal HTTP/1.0 server running directly on Xinu's TCP stack
 * (smc91c111 → ipv4 → tcp) and served from a dedicated thread.
 *
 *   - Listens on TCP port 80 with TCP_PASSIVE
 *   - Accepts one connection at a time (single-shot per cycle)
 *   - Endpoints:
 *       /             → HTML dashboard with 500ms polling
 *       /api/state    → JSON snapshot: actors[id/class/fields[]] + uptime
 *       /api/actors   → text/plain "id class" lines
 *       /api/uptime   → text/plain seconds since boot
 *       *             → 404
 *   - Re-opens the listener after each connection
 *
 * Intended as the proof-of-life HTTP endpoint for the Xinu AIPL
 * Round 2 "Web from Xinu" milestone — host PC no longer needs a
 * UART1-RPC bridge to observe actor state.
 */
#include <ipv4.h>
#include <tcp.h>
#include <network.h>
#include <device.h>
#include <thread.h>
#include <clock.h>
#include <string.h>
#include <stdio.h>
#include <ether.h>

/* Value layout must match c_translator.ml's runtime_prelude_xinu
 * (kept in sync with abcl_xinu_str.c / abcl_xinu_chkpt.c).  We can
 * read object fields via abcl_object_field_get() which returns
 * value_t by output pointer. */
typedef enum { V_NIL, V_INT, V_FLOAT, V_STR, V_OBJ } vtag_t;
typedef struct {
    vtag_t      tag;
    long        i;
    double      f;
    const char *s;
    int         obj_id;
} value_t;

extern int  abcl_n_objects(void);
extern int  abcl_object_class_id(int obj_id);
extern int  abcl_object_field_count(void);
extern int  abcl_object_field_get(int obj_id, int field_idx, value_t *out);
extern const char* abcl_class_name(int class_id);

extern struct netaddr g_local_ip;
extern int g_net_up;

static int g_http_tid = -1;
static int g_http_port = 80;

#define HTTP_REQ_MAX   1024
#define HTTP_RESP_MAX  8192

static int read_request(int fd, char *buf, int max)
{
    int total = 0;
    int got;
    while (total < max - 1) {
        got = read(fd, buf + total, 1);
        if (got <= 0) {
            break;
        }
        total += got;
        if (total >= 4 &&
            buf[total-4] == '\r' && buf[total-3] == '\n' &&
            buf[total-2] == '\r' && buf[total-1] == '\n') {
            break;
        }
    }
    buf[total] = 0;
    return total;
}

/* ─── content builders ───────────────────────────────────────────── */

static const char DASHBOARD_HTML[] =
"<!doctype html><html lang=\"ja\"><meta charset=\"utf-8\">"
"<title>Xinu Direct HTTP — Diners</title>"
"<style>"
"body{font:13px/1.45 ui-monospace,Menlo,monospace;margin:0;"
"background:#0e1116;color:#d6deeb}"
"header{padding:8px 16px;background:#181d27;"
"border-bottom:1px solid #2c3242;"
"display:flex;align-items:baseline;gap:16px}"
"header h1{margin:0;font-size:16px;color:#82aaff}"
"header .meta{font-size:11px;color:#7fbcff}"
"main{display:grid;grid-template-columns:1fr 1fr;gap:1px;"
"background:#2c3242;min-height:calc(100vh - 42px)}"
"section{background:#0e1116;padding:10px 14px;overflow:auto}"
"section h2{margin:0 0 8px;font-size:12px;color:#c792ea;"
"text-transform:uppercase;letter-spacing:.05em}"
"table{width:100%;border-collapse:collapse;font-size:12px}"
"th,td{text-align:left;padding:4px 8px;border-bottom:1px solid #2c3242}"
"th{color:#82aaff;font-weight:normal}"
".pill{display:inline-block;padding:0 6px;border-radius:3px;"
"background:#1a2a3a;color:#82aaff;font-size:11px}"
".free{color:#b6f0b6}.held{color:#f78c6c}"
".ok{color:#b6f0b6}"
"</style>"
"<header>"
"<h1>Xinu Direct HTTP — Diners</h1>"
"<div class=\"meta\">smc91c111 → ipv4 → tcp → :80 "
"<span class=\"ok\">no host bridge</span> "
"<span id=\"up\"></span></div>"
"</header>"
"<main>"
"<section>"
"<h2>Xinu Actors (Forks)</h2>"
"<table id=\"forks\"><thead><tr>"
"<th style=\"width:3em\">id</th><th>class</th>"
"<th style=\"width:6em\">holder</th>"
"</tr></thead><tbody></tbody></table>"
"<h2 style=\"margin-top:18px\">Xinu Philosophers</h2>"
"<table id=\"philos\"><thead><tr>"
"<th style=\"width:3em\">id</th><th>my_id</th>"
"<th>state</th><th>meals</th><th>meal_idx</th>"
"</tr></thead><tbody></tbody></table>"
"</section>"
"<section>"
"<h2>All Actor Fields (raw)</h2>"
"<table id=\"all\"><thead><tr>"
"<th style=\"width:3em\">id</th><th>class</th><th>fields</th>"
"</tr></thead><tbody></tbody></table>"
"</section>"
"</main>"
"<script>"
"const $=(id)=>document.getElementById(id);"
"function holderText(h){return h===0?'<span class=\"free\">free</span>'"
":'<span class=\"held\">P'+h+'</span>';}"
"async function tick(){"
"try{const r=await fetch('/api/state');const s=await r.json();"
"$('up').textContent='uptime '+(s.uptime_ms/1000).toFixed(1)+'s, '"
"+'n_actors='+s.actors.length;"
"const forks=s.actors.filter(a=>a.class==='Fork');"
"$('forks').querySelector('tbody').innerHTML=forks.length?"
"forks.map(a=>'<tr><td><span class=\"pill\">'+a.id+'</span></td>'"
"+'<td>'+a.class+'</td><td>'+holderText(a.fields[0])+'</td></tr>').join(''):"
"'<tr><td colspan=3 style=opacity:0.6>no Fork actors</td></tr>';"
"const philos=s.actors.filter(a=>a.class==='Philosopher');"
"$('philos').querySelector('tbody').innerHTML=philos.length?"
"philos.map(a=>'<tr><td><span class=\"pill\">'+a.id+'</span></td>'"
"+'<td>'+a.fields[0]+'</td><td>'+a.fields[5]+'</td>'"
"+'<td>'+a.fields[4]+'/'+a.fields[3]+'</td>'"
"+'<td>'+a.fields[4]+'</td></tr>').join(''):"
"'<tr><td colspan=5 style=opacity:0.6>no Philosopher actors</td></tr>';"
"$('all').querySelector('tbody').innerHTML="
"s.actors.map(a=>'<tr><td><span class=\"pill\">'+a.id+'</span></td>'"
"+'<td>'+a.class+'</td><td>['+a.fields.join(',')+']</td></tr>').join('');"
"}catch(e){}"
"}"
"setInterval(tick,500);tick();"
"</script></html>";

static int build_api_state(char *out, int max)
{
    int n = abcl_n_objects();
    int n_fields = abcl_object_field_count();
    int i, j, len = 0;
    value_t v;

    /* Cap visible field count to keep responses small. */
    if (n_fields > 8) n_fields = 8;

    len += sprintf(out + len, "{\"uptime_ms\":%ld,\"actors\":[",
                   (long)clktime);
    for (i = 0; i < n && len < max - 256; i++) {
        const char *cn = abcl_class_name(abcl_object_class_id(i));
        if (i > 0) {
            out[len++] = ',';
        }
        len += sprintf(out + len,
                       "{\"id\":%d,\"class\":\"%s\",\"fields\":[",
                       i, cn ? cn : "?");
        for (j = 0; j < n_fields && len < max - 64; j++) {
            if (j > 0) out[len++] = ',';
            if (abcl_object_field_get(i, j, &v)) {
                if (v.tag == V_INT)       len += sprintf(out + len, "%ld", v.i);
                else if (v.tag == V_OBJ)  len += sprintf(out + len, "%d", v.obj_id);
                else if (v.tag == V_NIL)  len += sprintf(out + len, "null");
                else                      len += sprintf(out + len, "0");
            } else {
                len += sprintf(out + len, "0");
            }
        }
        len += sprintf(out + len, "]}");
    }
    len += sprintf(out + len, "]}");
    return len;
}

static int build_api_actors(char *out, int max)
{
    int n = abcl_n_objects();
    int i, len = 0;
    len += sprintf(out + len, "n_actors=%d\n", n);
    for (i = 0; i < n && len < max - 64; i++) {
        const char *cn = abcl_class_name(abcl_object_class_id(i));
        len += sprintf(out + len, "%d %s\n", i, cn ? cn : "?");
    }
    return len;
}

static int build_api_uptime(char *out, int max)
{
    return sprintf(out, "uptime_ms=%ld\n", (long)clktime);
}

static void write_resp(int fd, const char *status, const char *ctype,
                       const char *body, int blen)
{
    char hdr[256];
    int hlen;
    hlen = sprintf(hdr,
        "HTTP/1.0 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, ctype, blen);
    write(fd, hdr, hlen);
    if (blen > 0) {
        write(fd, body, blen);
    }
}

static void handle_one(int fd)
{
    static char req[HTTP_REQ_MAX];
    static char resp[HTTP_RESP_MAX];
    int rlen;
    int blen;
    char path[64];
    int i;

    rlen = read_request(fd, req, HTTP_REQ_MAX);
    if (rlen <= 0) {
        return;
    }

    path[0] = 0;
    if (rlen > 5 && req[0] == 'G' && req[1] == 'E' && req[2] == 'T'
        && req[3] == ' ') {
        for (i = 0; i < (int)sizeof(path) - 1 && req[4 + i] != ' '
             && req[4 + i] != '\r' && req[4 + i] != '\n'
             && req[4 + i] != 0; i++) {
            path[i] = req[4 + i];
        }
        path[i] = 0;
    }
    kprintf("[http] GET %s\r\n", path);

    if (path[0] == '/' && path[1] == 0) {
        write_resp(fd, "200 OK", "text/html; charset=utf-8",
                   DASHBOARD_HTML, (int)(sizeof(DASHBOARD_HTML) - 1));
    } else if (strncmp(path, "/api/state", 10) == 0) {
        blen = build_api_state(resp, HTTP_RESP_MAX);
        write_resp(fd, "200 OK", "application/json", resp, blen);
    } else if (strncmp(path, "/api/actors", 11) == 0) {
        blen = build_api_actors(resp, HTTP_RESP_MAX);
        write_resp(fd, "200 OK", "text/plain", resp, blen);
    } else if (strncmp(path, "/api/uptime", 11) == 0) {
        blen = build_api_uptime(resp, HTTP_RESP_MAX);
        write_resp(fd, "200 OK", "text/plain", resp, blen);
    } else {
        const char *body = "not found\n";
        write_resp(fd, "404 Not Found", "text/plain", body, 10);
    }
}

static thread http_main(int port)
{
    int fd;
    int rc;

    while (!g_net_up) {
        sleep(100);
    }
    kprintf("[http] starting on port %d\r\n", port);
    g_http_port = port;

    for (;;) {
        fd = tcpAlloc();
        if (fd == SYSERR) {
            kprintf("[http] tcpAlloc failed\r\n");
            sleep(500);
            continue;
        }
        /* open(dev, localip, remoteip, localpt, remotept, mode) */
        rc = open(fd, &g_local_ip, NULL, port, 0, TCP_PASSIVE);
        if (rc == SYSERR) {
            kprintf("[http] tcp passive open failed\r\n");
            close(fd);
            sleep(500);
            continue;
        }
        handle_one(fd);
        close(fd);
    }
}

int abcl_http_start(int port)
{
    if (g_http_tid >= 0) {
        return g_http_tid;
    }
    g_http_tid = create((void *)http_main, INITSTK, INITPRIO,
                        "abcl-http", 1, port);
    if (g_http_tid >= 0) {
        ready(g_http_tid, RESCHED_NO);
        kprintf("[http] spawn tid=%d port=%d\r\n", g_http_tid, port);
    } else {
        kprintf("[http] create failed\r\n");
    }
    return g_http_tid;
}
