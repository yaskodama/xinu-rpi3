/*
 * abcl_xinu_gui.c
 * ABCL/c+ → Xinu 翻訳のための GUI 補助ランタイム。
 *
 * 提供する built-in (abcl 側からそのまま呼べる):
 *    value_t b_cos(int n, value_t* a);              // 度→cos*1024 (整数)
 *    value_t b_sin(int n, value_t* a);              // 度→sin*1024 (整数)
 *    value_t xinu_gui_set_line(int n, value_t* a);  // (idx, x1, y1, x2, y2 [,r,g,b])
 *    value_t xinu_gui_register_ticker(int n, value_t* a);  // (target_obj)
 *
 * wm.c (window manager) から呼ぶフック:
 *    void abcl_xinu_gui_render(void);     // 描画ループの最後で線を上描き
 *    void abcl_xinu_gui_tick_all(void);   // 16ms 周期で tick メッセージを配信
 */

#include <stddef.h>
#include <kernel.h>
#include <thread.h>
#include <semaphore.h>

/* abcl ランタイムが定義する value_t と完全一致させる */
typedef enum { V_NIL, V_INT, V_FLOAT, V_STR, V_OBJ } vtag_t;
typedef struct {
    vtag_t      tag;
    long        i;
    double      f;
    const char *s;
    int         obj_id;
} value_t;

/* abcl ランタイム側 (--xinu モードの生成 .c) で定義 */
extern void abcl_enqueue(int sender, int receiver, const char *method,
                         int n_args, value_t *args);

/* wm.c 側で公開する描画ラッパ */
extern void put_pixel_pub  (int x, int y, unsigned short color);
extern void fill_rect_pub  (int x, int y, int w, int h, unsigned short c);
extern void rect_outline_pub(int x, int y, int w, int h, unsigned short c);
extern void draw_string_pub (int x, int y, const char *s, unsigned short c);

/* 単純ヘルパ */
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

/* ===== 整数 sin/cos テーブル (度→値*1024) =====================
 * Bhaskara I 近似:
 *   sin(x[deg]) ≈ 4*x*(180-x) / (40500 - x*(180-x))   (0<=x<=180)
 *   x in [180,360) は符号反転
 */
static int sin_tab[360];
static int cos_tab[360];
static int trig_init_done = 0;

static void init_trig_tables(void)
{
    int i;
    for (i = 0; i < 360; i++) {
        int x = i;
        int sign = 1;
        if (x >= 180) { x -= 180; sign = -1; }
        long num = (long)4 * x * (180 - x);
        long den = (long)40500 - (long)x * (180 - x);
        long v = (num * 1024) / den;
        sin_tab[i] = (int)(sign * v);
    }
    for (i = 0; i < 360; i++) {
        cos_tab[i] = sin_tab[(i + 90) % 360];
    }
    trig_init_done = 1;
}

value_t b_cos(int n_args, value_t *args)
{
    long deg;
    if (!trig_init_done) init_trig_tables();
    if (n_args < 1) return v_int(0);
    deg = (args[0].tag == V_INT) ? args[0].i : (long)args[0].f;
    deg = ((deg % 360) + 360) % 360;
    return v_int(cos_tab[deg]);
}

value_t b_sin(int n_args, value_t *args)
{
    long deg;
    if (!trig_init_done) init_trig_tables();
    if (n_args < 1) return v_int(0);
    deg = (args[0].tag == V_INT) ? args[0].i : (long)args[0].f;
    deg = ((deg % 360) + 360) % 360;
    return v_int(sin_tab[deg]);
}

/* ===== 線分レジストリ ========================================= */
#define MAX_GLINES 8

typedef struct {
    int valid;
    int x1, y1, x2, y2;
    int r, g, b;
} gline_t;

static gline_t   g_lines[MAX_GLINES];
static semaphore lines_mu;
static int       lines_init_done = 0;

static void lines_ensure_init(void)
{
    if (!lines_init_done) {
        lines_mu = semcreate(1);
        lines_init_done = 1;
    }
}

value_t xinu_gui_set_line(int n, value_t *a)
{
    int idx;
    lines_ensure_init();
    if (n < 5) return v_int(0);
    idx = (int)a[0].i;
    if (idx < 0 || idx >= MAX_GLINES) return v_int(0);
    wait(lines_mu);
    g_lines[idx].valid = 1;
    g_lines[idx].x1 = (int)a[1].i;
    g_lines[idx].y1 = (int)a[2].i;
    g_lines[idx].x2 = (int)a[3].i;
    g_lines[idx].y2 = (int)a[4].i;
    if (n >= 8) {
        g_lines[idx].r = (int)a[5].i;
        g_lines[idx].g = (int)a[6].i;
        g_lines[idx].b = (int)a[7].i;
    } else {
        g_lines[idx].r = 200; g_lines[idx].g = 220; g_lines[idx].b = 255;
    }
    signal(lines_mu);
    return v_int(0);
}

/* ===== ticker レジストリ ====================================== */
#define MAX_TICKERS 16
static int g_tickers[MAX_TICKERS];
static int g_n_tickers = 0;

value_t xinu_gui_register_ticker(int n, value_t *a)
{
    int t;
    if (n < 1) return v_int(0);
    t = (a[0].tag == V_OBJ) ? a[0].obj_id : (int)a[0].i;
    if (g_n_tickers < MAX_TICKERS) g_tickers[g_n_tickers++] = t;
    return v_int(0);
}

/* 前方宣言 (描画関数本体は後で定義) */
static void draw_line(int x0, int y0, int x1, int y1, unsigned short c);
static int  iabs(int v);

/* ===== 哲学者問題 ============================================ */
#define MAX_PHILS  8
#define MAX_FORKS  8

typedef struct {
    int valid;
    int cx, cy, radius;
    int state;          /* 0=thinking, 1=hungry, 2=eating */
} gphil_t;

typedef struct {
    int valid;
    /* フォークを共有する 2 哲学者 (アクター上の隣接関係) */
    int adj_a, adj_b;
    /* スクリーン上の幾何的左右 (cx 比較で決定。dining_init で確定) */
    int leftside;       /* 画面左側にいる哲学者 (xl,yl 端) */
    int rightside;      /* 画面右側にいる哲学者 (xr,yr 端) */
    /* 描画用エンドポイント。leftside→rightside の方向で並んでいる */
    int xl, yl, xr, yr;
    int dir_deg;        /* leftside → rightside の方向 (度) */
    int held;
    int holder;
} gfork_t;

static gphil_t g_phils[MAX_PHILS];
static gfork_t g_forks[MAX_FORKS];

value_t xinu_gui_dining_init(int n, value_t *a)
{
    int N, i;
    int cx = 320, cy = 240;
    int R_phil = 130, R_fork = 95;
    if (!trig_init_done) init_trig_tables();
    if (n < 1) return v_int(0);
    N = (int)a[0].i;
    if (N > MAX_PHILS) N = MAX_PHILS;
    for (i = 0; i < N; i++) {
        long deg = -90 + (long)360 * i / N;
        deg = ((deg % 360) + 360) % 360;
        g_phils[i].valid  = 1;
        g_phils[i].cx     = cx + (int)(cos_tab[deg] * R_phil / 1024);
        g_phils[i].cy     = cy + (int)(sin_tab[deg] * R_phil / 1024);
        g_phils[i].radius = 24;
        g_phils[i].state  = 0;
    }
    /* fork i は phil_(i-1) と phil_i の間 (アクター上の隣接) */
    for (i = 0; i < N; i++) {
        int a = (i - 1 + N) % N;
        int b = i;
        int leftside, rightside;
        int lx, ly, rx, ry;
        int mx, my;
        int dx, dy;
        int dir_deg;
        int half = 18;
        long best_dot;
        int d;
        (void)R_fork;       /* もう使わない */
        /* 幾何的な左右を確定 (cx の小さい方が画面左) */
        if (g_phils[a].cx <= g_phils[b].cx) { leftside = a; rightside = b; }
        else                                 { leftside = b; rightside = a; }
        lx = g_phils[leftside].cx;  ly = g_phils[leftside].cy;
        rx = g_phils[rightside].cx; ry = g_phils[rightside].cy;
        mx = (lx + rx) / 2;
        my = (ly + ry) / 2;
        dx = rx - lx;
        dy = ry - ly;
        /* atan2 がないので cos/sin テーブルから最尤 deg を線形探索 */
        best_dot = -(1L << 30);
        dir_deg = 0;
        for (d = 0; d < 360; d++) {
            long dot = (long)cos_tab[d] * dx + (long)sin_tab[d] * dy;
            if (dot > best_dot) { best_dot = dot; dir_deg = d; }
        }
        g_forks[i].valid     = 1;
        g_forks[i].adj_a     = a;
        g_forks[i].adj_b     = b;
        g_forks[i].leftside  = leftside;
        g_forks[i].rightside = rightside;
        /* 中点を中心に dir_deg 方向に half ずつ伸ばした線分 */
        g_forks[i].xl = mx - (int)((long)cos_tab[dir_deg] * half / 1024);
        g_forks[i].yl = my - (int)((long)sin_tab[dir_deg] * half / 1024);
        g_forks[i].xr = mx + (int)((long)cos_tab[dir_deg] * half / 1024);
        g_forks[i].yr = my + (int)((long)sin_tab[dir_deg] * half / 1024);
        g_forks[i].dir_deg = dir_deg;
        g_forks[i].held    = 0;
        g_forks[i].holder  = -1;
    }
    return v_int(0);
}

value_t xinu_gui_set_phil(int n, value_t *a)
{
    int idx, st;
    if (n < 2) return v_int(0);
    idx = (int)a[0].i;
    st  = (int)a[1].i;
    if (idx < 0 || idx >= MAX_PHILS) return v_int(0);
    if (g_phils[idx].valid) g_phils[idx].state = st;
    return v_int(0);
}

value_t xinu_gui_set_fork_held(int n, value_t *a)
{
    int idx, holder;
    if (n < 2) return v_int(0);
    idx    = (int)a[0].i;
    holder = (int)a[1].i;
    if (idx < 0 || idx >= MAX_FORKS) return v_int(0);
    if (g_forks[idx].valid) { g_forks[idx].held = 1; g_forks[idx].holder = holder; }
    return v_int(0);
}

value_t xinu_gui_set_fork_free(int n, value_t *a)
{
    int idx;
    if (n < 1) return v_int(0);
    idx = (int)a[0].i;
    if (idx < 0 || idx >= MAX_FORKS) return v_int(0);
    if (g_forks[idx].valid) { g_forks[idx].held = 0; g_forks[idx].holder = -1; }
    return v_int(0);
}

/* 円塗りつぶし／輪郭 (整数 sqrt は線形探索でも十分小さい半径で OK) */
static void fill_circle(int cx, int cy, int rad, unsigned short c)
{
    int dy;
    for (dy = -rad; dy <= rad; dy++) {
        int dx_sq = rad*rad - dy*dy;
        int dx = 0;
        int xx;
        if (dx_sq < 0) continue;
        while ((dx + 1) * (dx + 1) <= dx_sq) dx++;
        for (xx = -dx; xx <= dx; xx++)
            put_pixel_pub(cx + xx, cy + dy, c);
    }
}

static void draw_circle_outline(int cx, int cy, int rad, unsigned short c)
{
    int x = rad, y = 0, err = 0;
    while (x >= y) {
        put_pixel_pub(cx + x, cy + y, c);
        put_pixel_pub(cx + y, cy + x, c);
        put_pixel_pub(cx - y, cy + x, c);
        put_pixel_pub(cx - x, cy + y, c);
        put_pixel_pub(cx - x, cy - y, c);
        put_pixel_pub(cx - y, cy - x, c);
        put_pixel_pub(cx + y, cy - x, c);
        put_pixel_pub(cx + x, cy - y, c);
        y += 1; err += 1 + 2*y;
        if (2*(err - x) + 1 > 0) { x -= 1; err += 1 - 2*x; }
    }
}

/* 三角形を塗りつぶす (鏃用) */
static void fill_triangle(int x0, int y0, int x1, int y1,
                          int x2, int y2, unsigned short c)
{
    int dx = x2 - x1;
    int dy = y2 - y1;
    int adx = iabs(dx);
    int ady = iabs(dy);
    int steps = (adx > ady) ? adx : ady;
    int i;
    if (steps == 0) {
        draw_line(x0, y0, x1, y1, c);
        return;
    }
    /* (x1,y1) → (x2,y2) の線分上を細かく刻み、各点から (x0,y0) へ線を引く */
    for (i = 0; i <= steps; i++) {
        int px = x1 + dx * i / steps;
        int py = y1 + dy * i / steps;
        draw_line(x0, y0, px, py, c);
    }
}

/* 矢印: 先端に大きめの三角の鏃 (塗りつぶし)。dir_deg は tail→head 方向 (度) */
static void draw_arrow(int xt, int yt, int xh, int yh, int dir_deg,
                       unsigned short c)
{
    int back_a = (dir_deg + 150) % 360;
    int back_b = (dir_deg + 210) % 360;
    int len_h  = 14;            /* 鏃の長さ (旧: 8) */
    int side_h = 8;             /* 鏃の半幅      */
    int perp_a = (dir_deg + 90)  % 360;   /* 軸と直角の方向 */
    int bx_axis = xh - (int)((long)cos_tab[dir_deg] * len_h / 1024);
    int by_axis = yh - (int)((long)sin_tab[dir_deg] * len_h / 1024);
    int ax = bx_axis + (int)((long)cos_tab[perp_a] * side_h / 1024);
    int ay = by_axis + (int)((long)sin_tab[perp_a] * side_h / 1024);
    int bx = bx_axis - (int)((long)cos_tab[perp_a] * side_h / 1024);
    int by = by_axis - (int)((long)sin_tab[perp_a] * side_h / 1024);
    (void)back_a; (void)back_b; /* 旧計算は捨て、上の精緻版を使う */

    /* 軸 (3px 厚)。鏃の根元 (bx_axis, by_axis) まで描けば十分 */
    draw_line(xt,   yt,   bx_axis,   by_axis,   c);
    draw_line(xt+1, yt,   bx_axis+1, by_axis,   c);
    draw_line(xt,   yt+1, bx_axis,   by_axis+1, c);
    /* 鏃 (塗りつぶし三角形) */
    fill_triangle(xh, yh, ax, ay, bx, by, c);
}

/* ===== 有限バッファ問題 ====================================== */
#define MAX_SLOTS    20
#define MAX_BACTORS  4
#define MAX_SLIDERS  4

typedef struct {
    int valid;
    int filled;
    int producer_id;
    int rx, ry, rw, rh;
} gslot_t;

typedef struct {
    int valid;
    int type;       /* 0=producer, 1=consumer */
    int state;      /* 0=idle, 1=working, 2=waiting */
    int cx, cy, radius;
} gact_t;

static int     g_buf_capacity = 0;
static int     g_buf_head     = 0;
static int     g_buf_tail     = 0;
static gslot_t g_slots[MAX_SLOTS];
static gact_t  g_producers[MAX_BACTORS];
static gact_t  g_consumers[MAX_BACTORS];
static int     g_n_producers  = 0;
static int     g_n_consumers  = 0;

/* producer 別の固定パレット */
static void producer_color(int pid, int *r, int *g, int *b)
{
    static const int pal[6][3] = {
        {220,  90,  90},   /* red    */
        { 90, 200, 130},   /* green  */
        { 90, 140, 230},   /* blue   */
        {220, 180,  90},   /* amber  */
        {180,  90, 220},   /* purple */
        { 90, 200, 220},   /* cyan   */
    };
    int i = ((pid % 6) + 6) % 6;
    *r = pal[i][0]; *g = pal[i][1]; *b = pal[i][2];
}

value_t xinu_gui_buf_setup(int n, value_t *a)
{
    int cap, npr, nco, i;
    int slot_w, slot_h, total_w, start_x, slot_y;
    int top_y, bot_y, mid_x_l, mid_x_r;
    if (n < 3) return v_int(0);
    cap = (int)a[0].i;
    npr = (int)a[1].i;
    nco = (int)a[2].i;
    if (cap > MAX_SLOTS)   cap = MAX_SLOTS;
    if (npr > MAX_BACTORS) npr = MAX_BACTORS;
    if (nco > MAX_BACTORS) nco = MAX_BACTORS;
    g_buf_capacity = cap;
    g_buf_head = 0; g_buf_tail = 0;
    g_n_producers = npr;
    g_n_consumers = nco;

    /* レイアウト: 左 producer | 中央 buffer | 右 consumer */
    top_y     = 80;
    bot_y     = 320;     /* 下にスライダ＋ボタン用余白 */
    mid_x_l   = 90;
    mid_x_r   = 550;
    /* スロット幅は中央に収まる範囲で自動算出 */
    slot_w = (mid_x_r - mid_x_l) / cap;
    if (slot_w > 30) slot_w = 30;
    if (slot_w < 14) slot_w = 14;
    slot_h = slot_w + 8;
    if (slot_h > 36) slot_h = 36;
    total_w = slot_w * cap;
    start_x = (mid_x_l + mid_x_r - total_w) / 2;
    slot_y  = (top_y + bot_y) / 2 - slot_h / 2;
    for (i = 0; i < cap; i++) {
        g_slots[i].valid       = 1;
        g_slots[i].filled      = 0;
        g_slots[i].producer_id = -1;
        g_slots[i].rx = start_x + i * slot_w;
        g_slots[i].ry = slot_y;
        g_slots[i].rw = slot_w - 2;
        g_slots[i].rh = slot_h - 2;
    }
    /* producer 縦並び (左) */
    for (i = 0; i < npr; i++) {
        int yy;
        if (npr <= 1) yy = (top_y + bot_y) / 2;
        else          yy = top_y + i * (bot_y - top_y) / (npr - 1);
        g_producers[i].valid  = 1;
        g_producers[i].type   = 0;
        g_producers[i].state  = 0;
        g_producers[i].cx     = 40;
        g_producers[i].cy     = yy;
        g_producers[i].radius = 22;
    }
    /* consumer 縦並び (右) */
    for (i = 0; i < nco; i++) {
        int yy;
        if (nco <= 1) yy = (top_y + bot_y) / 2;
        else          yy = top_y + i * (bot_y - top_y) / (nco - 1);
        g_consumers[i].valid  = 1;
        g_consumers[i].type   = 1;
        g_consumers[i].state  = 0;
        g_consumers[i].cx     = 600;
        g_consumers[i].cy     = yy;
        g_consumers[i].radius = 22;
    }
    return v_int(0);
}

value_t xinu_gui_buf_put(int n, value_t *a)
{
    int pid, slot;
    if (n < 1 || g_buf_capacity <= 0) return v_int(0);
    pid = (int)a[0].i;
    slot = g_buf_tail % g_buf_capacity;
    g_slots[slot].filled      = 1;
    g_slots[slot].producer_id = pid;
    g_buf_tail = (g_buf_tail + 1) % g_buf_capacity;
    return v_int(0);
}

value_t xinu_gui_buf_take(int n, value_t *a)
{
    int slot;
    (void)n; (void)a;
    if (g_buf_capacity <= 0) return v_int(0);
    slot = g_buf_head % g_buf_capacity;
    g_slots[slot].filled      = 0;
    g_slots[slot].producer_id = -1;
    g_buf_head = (g_buf_head + 1) % g_buf_capacity;
    return v_int(0);
}

value_t xinu_gui_set_actor(int n, value_t *a)
{
    int idx, type, state;
    if (n < 3) return v_int(0);
    idx   = (int)a[0].i;
    type  = (int)a[1].i;
    state = (int)a[2].i;
    if (idx < 0 || idx >= MAX_BACTORS) return v_int(0);
    if      (type == 0 && g_producers[idx].valid) g_producers[idx].state = state;
    else if (type == 1 && g_consumers[idx].valid) g_consumers[idx].state = state;
    return v_int(0);
}

/* ===== スライダー ============================================ */
typedef struct {
    int  valid;
    int  x, y, w, h;
    int  track_id;
    int  min_val, max_val, current_val;
    int  color_r, color_g, color_b;
    char label[8];
} gslider_t;

static gslider_t g_sliders[MAX_SLIDERS];
static int       g_n_sliders   = 0;
static int       g_drag_slider = -1;

value_t xinu_gui_add_slider(int n, value_t *a)
{
    gslider_t *s;
    int i;
    const char *lab;
    if (n < 8) return v_int(0);
    if (g_n_sliders >= MAX_SLIDERS) return v_int(0);
    s = &g_sliders[g_n_sliders++];
    s->valid       = 1;
    s->track_id    = (int)a[0].i;
    s->x           = (int)a[1].i;
    s->y           = (int)a[2].i;
    s->w           = (int)a[3].i;
    s->h           = (int)a[4].i;
    s->min_val     = (int)a[5].i;
    s->max_val     = (int)a[6].i;
    s->current_val = (int)a[7].i;
    if (n >= 11) {
        s->color_r = (int)a[8].i;
        s->color_g = (int)a[9].i;
        s->color_b = (int)a[10].i;
    } else {
        s->color_r = 200; s->color_g = 200; s->color_b = 220;
    }
    if (n >= 12) {
        lab = (a[11].tag == V_STR && a[11].s) ? a[11].s : "";
        for (i = 0; i < (int)sizeof s->label - 1 && lab[i]; i++)
            s->label[i] = lab[i];
        s->label[i] = '\0';
    } else {
        s->label[0] = '\0';
    }
    return v_int(0);
}

value_t xinu_gui_slider_value(int n, value_t *a)
{
    int track_id, i;
    if (n < 1) return v_int(0);
    track_id = (int)a[0].i;
    for (i = 0; i < g_n_sliders; i++) {
        if (g_sliders[i].valid && g_sliders[i].track_id == track_id)
            return v_int(g_sliders[i].current_val);
    }
    return v_int(0);
}

static void slider_update_from_x(int idx, int mx)
{
    gslider_t *s;
    int rx, range;
    if (idx < 0 || idx >= g_n_sliders) return;
    s = &g_sliders[idx];
    if (!s->valid) return;
    rx = mx - s->x;
    if (rx < 0) rx = 0;
    if (rx > s->w) rx = s->w;
    range = s->max_val - s->min_val;
    s->current_val = s->min_val +
        (rx * range + s->w / 2) / (s->w > 0 ? s->w : 1);
}

/* ===== ボタン =============================================== */
#define MAX_BUTTONS 6
typedef struct {
    int  valid;
    int  x, y, w, h;
    char label[16];
    int  target;
    char method[16];
    int  color_r, color_g, color_b;
    int  pressed_frames;   /* クリック後のフラッシュ */
} gbutton_t;

static gbutton_t g_buttons[MAX_BUTTONS];
static int       g_n_buttons = 0;

static void copy_str(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

value_t xinu_gui_add_button(int n, value_t *a)
{
    gbutton_t *b;
    if (n < 7) return v_int(0);
    if (g_n_buttons >= MAX_BUTTONS) return v_int(0);
    b = &g_buttons[g_n_buttons++];
    b->valid = 1;
    copy_str(b->label,  (a[0].tag == V_STR && a[0].s) ? a[0].s : "", sizeof b->label);
    b->x = (int)a[1].i; b->y = (int)a[2].i;
    b->w = (int)a[3].i; b->h = (int)a[4].i;
    b->target = (a[5].tag == V_OBJ) ? a[5].obj_id : (int)a[5].i;
    copy_str(b->method, (a[6].tag == V_STR && a[6].s) ? a[6].s : "", sizeof b->method);
    if (b->label[0] == 'S' && b->label[1] == 't' && b->label[2] == 'a') {
        b->color_r =  60; b->color_g = 170; b->color_b =  90;   /* Start */
    } else if (b->label[0] == 'S' && b->label[1] == 't' && b->label[2] == 'o') {
        b->color_r = 200; b->color_g =  80; b->color_b =  80;   /* Stop  */
    } else {
        b->color_r = 120; b->color_g = 120; b->color_b = 140;
    }
    b->pressed_frames = 0;
    return v_int(0);
}

/* wm から呼ばれる: クリックがスライダー/ボタンに当たれば 1 を返す */
int abcl_xinu_gui_handle_click(int mx, int my)
{
    int i;
    /* スライダー優先 (掴んでドラッグ開始) */
    for (i = 0; i < g_n_sliders; i++) {
        gslider_t *s = &g_sliders[i];
        if (!s->valid) continue;
        if (mx >= s->x - 6 && mx < s->x + s->w + 6 &&
            my >= s->y - 6 && my < s->y + s->h + 6) {
            g_drag_slider = i;
            slider_update_from_x(i, mx);
            return 1;
        }
    }
    /* ボタン */
    for (i = 0; i < g_n_buttons; i++) {
        gbutton_t *b = &g_buttons[i];
        if (!b->valid) continue;
        if (mx >= b->x && mx < b->x + b->w &&
            my >= b->y && my < b->y + b->h) {
            b->pressed_frames = 6;
            abcl_enqueue(-1, b->target, b->method, 0, NULL);
            return 1;
        }
    }
    return 0;
}

/* マウス移動時に呼ぶ。スライダーをドラッグ中なら値を更新 */
void abcl_xinu_gui_handle_drag(int mx, int my)
{
    (void)my;
    if (g_drag_slider >= 0) slider_update_from_x(g_drag_slider, mx);
}

/* ボタンリリース時 */
void abcl_xinu_gui_handle_release(void)
{
    g_drag_slider = -1;
}

/* ===== 線描画 (Bresenham) と BGR565 変換 ======================
   wm.c の RGB() マクロと同じく PL110 のレイアウト (R を低ビット側) */
static unsigned short rgb565(int r, int g, int b)
{
    return (unsigned short)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}

static int iabs(int v) { return v < 0 ? -v : v; }

static void draw_line(int x0, int y0, int x1, int y1, unsigned short c)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = iabs(dx);
    int ady = iabs(dy);
    int sx  = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    int sy  = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    int err;
    if (adx > ady) {
        err = adx / 2;
        while (x0 != x1) {
            put_pixel_pub(x0, y0, c);
            err -= ady;
            if (err < 0) { y0 += sy; err += adx; }
            x0 += sx;
        }
    } else {
        err = ady / 2;
        while (y0 != y1) {
            put_pixel_pub(x0, y0, c);
            err -= adx;
            if (err < 0) { x0 += sx; err += ady; }
            y0 += sy;
        }
    }
    put_pixel_pub(x0, y0, c);
}

/* wm から呼ぶ: 描画 (3px 厚で視認性アップ) */
void abcl_xinu_gui_render(void)
{
    int i;
    lines_ensure_init();
    wait(lines_mu);
    for (i = 0; i < MAX_GLINES; i++) {
        if (g_lines[i].valid) {
            unsigned short c = rgb565(g_lines[i].r, g_lines[i].g, g_lines[i].b);
            int x1 = g_lines[i].x1, y1 = g_lines[i].y1;
            int x2 = g_lines[i].x2, y2 = g_lines[i].y2;
            draw_line(x1,   y1,   x2,   y2,   c);
            draw_line(x1+1, y1,   x2+1, y2,   c);
            draw_line(x1,   y1+1, x2,   y2+1, c);
            draw_line(x1-1, y1,   x2-1, y2,   c);
            draw_line(x1,   y1-1, x2,   y2-1, c);
        }
    }
    signal(lines_mu);

    /* フォーク描画
       - エンドポイントは dining_init で「両隣の哲学者を結ぶ線」上の中点
         ±half として確定済み (= 五角形の辺に沿う傾き)
       - 空            : 灰色の平坦/斜め線分
       - 右側 (画面右) の哲学者が保持: 黄色の右向き (→ leftside→rightside) 矢印
       - 左側 (画面左) の哲学者が保持: 黄色の左向き (← rightside→leftside) 矢印 */
    for (i = 0; i < MAX_FORKS; i++) {
        gfork_t *f = &g_forks[i];
        unsigned short cc;
        if (!f->valid) continue;

        if (!f->held) {
            cc = rgb565(120, 120, 130);
            /* 3px 厚: 線本体 + ±1 オフセット */
            draw_line(f->xl,   f->yl,   f->xr,   f->yr,   cc);
            draw_line(f->xl+1, f->yl,   f->xr+1, f->yr,   cc);
            draw_line(f->xl,   f->yl+1, f->xr,   f->yr+1, cc);
        } else {
            cc = rgb565(240, 200, 80);
            if (f->holder == f->rightside) {
                /* → 右向き矢印: leftside 端から rightside 端へ */
                draw_arrow(f->xl, f->yl, f->xr, f->yr, f->dir_deg, cc);
            } else if (f->holder == f->leftside) {
                /* ← 左向き矢印: rightside 端から leftside 端へ */
                int rev = (f->dir_deg + 180) % 360;
                draw_arrow(f->xr, f->yr, f->xl, f->yl, rev, cc);
            } else {
                draw_line(f->xl, f->yl, f->xr, f->yr, cc);
            }
        }
    }
    /* 哲学者 (円) */
    for (i = 0; i < MAX_PHILS; i++) {
        gphil_t *p = &g_phils[i];
        int cr = 80, cg = 130, cb = 230;
        unsigned short body, outl;
        if (!p->valid) continue;
        switch (p->state) {
        case 0: cr =  80; cg = 130; cb = 230; break;  /* thinking 青 */
        case 1: cr = 240; cg = 200; cb =  80; break;  /* hungry   黄 */
        case 2: cr =  80; cg = 200; cb = 120; break;  /* eating   緑 */
        }
        body = rgb565(cr, cg, cb);
        outl = rgb565(240, 240, 240);
        fill_circle(p->cx, p->cy, p->radius, body);
        draw_circle_outline(p->cx, p->cy, p->radius, outl);
    }

    /* 有限バッファ問題のスロット */
    if (g_buf_capacity > 0) {
        for (i = 0; i < g_buf_capacity; i++) {
            gslot_t *slot = &g_slots[i];
            unsigned short bg, ol;
            if (!slot->valid) continue;
            bg = rgb565(35, 38, 50);
            fill_rect_pub(slot->rx, slot->ry, slot->rw, slot->rh, bg);
            if (slot->filled) {
                int sr, sg, sb;
                producer_color(slot->producer_id, &sr, &sg, &sb);
                fill_rect_pub(slot->rx + 3, slot->ry + 3,
                              slot->rw - 6, slot->rh - 6,
                              rgb565(sr, sg, sb));
            }
            ol = rgb565(180, 180, 200);
            rect_outline_pub(slot->rx, slot->ry, slot->rw, slot->rh, ol);
        }
    }
    /* producer */
    for (i = 0; i < g_n_producers; i++) {
        gact_t *p = &g_producers[i];
        int cr, cg, cb;
        unsigned short body, outl;
        if (!p->valid) continue;
        producer_color(i, &cr, &cg, &cb);
        if (p->state == 0) { cr = cr / 3; cg = cg / 3; cb = cb / 3; }
        else if (p->state == 2) { cr = 240; cg = 200; cb = 80; }
        body = rgb565(cr, cg, cb);
        outl = rgb565(240, 240, 240);
        fill_circle(p->cx, p->cy, p->radius, body);
        draw_circle_outline(p->cx, p->cy, p->radius, outl);
    }
    /* consumer */
    for (i = 0; i < g_n_consumers; i++) {
        gact_t *p = &g_consumers[i];
        int cr = 80, cg = 200, cb = 130;
        unsigned short body, outl;
        if (!p->valid) continue;
        if (p->state == 0)      { cr = 60;  cg = 80;  cb = 80;  }
        else if (p->state == 1) { cr = 90;  cg = 220; cb = 140; }
        else if (p->state == 2) { cr = 240; cg = 200; cb = 80;  }
        body = rgb565(cr, cg, cb);
        outl = rgb565(240, 240, 240);
        fill_circle(p->cx, p->cy, p->radius, body);
        draw_circle_outline(p->cx, p->cy, p->radius, outl);
    }

    /* スライダー */
    for (i = 0; i < g_n_sliders; i++) {
        gslider_t *s = &g_sliders[i];
        unsigned short tr, fl, th_c, ol;
        int range, fw, tx;
        if (!s->valid) continue;
        tr = rgb565(50, 55, 70);
        fl = rgb565(s->color_r, s->color_g, s->color_b);
        th_c = rgb565(245, 245, 245);
        ol = rgb565(200, 200, 220);
        fill_rect_pub(s->x, s->y, s->w, s->h, tr);
        range = s->max_val - s->min_val;
        fw = (range > 0)
             ? s->w * (s->current_val - s->min_val) / range : 0;
        if (fw < 0) fw = 0; if (fw > s->w) fw = s->w;
        fill_rect_pub(s->x, s->y, fw, s->h, fl);
        tx = s->x + fw;
        fill_rect_pub(tx - 4, s->y - 5, 8, s->h + 10, th_c);
        rect_outline_pub(s->x, s->y, s->w, s->h, ol);
        if (s->label[0])
            draw_string_pub(s->x - 50, s->y - 1, s->label, rgb565(230, 230, 240));
    }

    /* ボタン */
    for (i = 0; i < g_n_buttons; i++) {
        gbutton_t *b = &g_buttons[i];
        int cr, cg, cb;
        unsigned short bg, outline_c;
        int slen, char_w, text_w, tx, ty;
        const char *p;
        if (!b->valid) continue;
        cr = b->color_r; cg = b->color_g; cb = b->color_b;
        if (b->pressed_frames > 0) {
            cr = (cr + 50 < 255) ? cr + 50 : 255;
            cg = (cg + 50 < 255) ? cg + 50 : 255;
            cb = (cb + 50 < 255) ? cb + 50 : 255;
            b->pressed_frames--;
        }
        bg = rgb565(cr, cg, cb);
        outline_c = rgb565(240, 240, 240);
        fill_rect_pub(b->x, b->y, b->w, b->h, bg);
        rect_outline_pub(b->x, b->y, b->w, b->h, outline_c);
        /* ラベルを中央寄せ。8x2 = 16px char (FONT_SCALE=2) */
        slen = 0;
        for (p = b->label; *p; p++) slen++;
        char_w = 16;
        text_w = slen * char_w;
        tx = b->x + (b->w - text_w) / 2;
        ty = b->y + (b->h - char_w) / 2;
        draw_string_pub(tx, ty, b->label, rgb565(255, 255, 255));
    }
}

/* wm から呼ぶ: 全 ticker に tick() を配信 */
void abcl_xinu_gui_tick_all(void)
{
    int i;
    for (i = 0; i < g_n_tickers; i++) {
        abcl_enqueue(-1, g_tickers[i], "tick", 0, NULL);
    }
}

/* ============================================================
 *  G1: framebuffer drawing primitives (callable from AIPL).
 *
 *  AIPL externs (all return v_int(1) on success, v_int(0) on bad arg):
 *    fb_pixel(x, y, color24)
 *    fb_line(x0, y0, x1, y1, color24)
 *    fb_circle(cx, cy, r, color24)
 *    fb_triangle(x0, y0, x1, y1, x2, y2, color24)
 *    fb_rrect(x, y, w, h, radius, color24)
 *    fb_dashed_line(x0, y0, x1, y1, color24, dash)
 *    fb_set_aa(0|1)     -> returns new AA flag
 *    fb_get_aa()        -> returns current AA flag
 *
 *  Color: a single AIPL int packed as 0xRRGGBB.  Quantized to PL110's
 *  BGR565 layout on the actual framebuffer write.  Serial trace
 *  carries the full 24-bit value so smokes can verify.
 *
 *  AA: a per-runtime flag.  When AA=1 the line drawer also paints a
 *  1-pixel parallel feather in a 50%-intensity copy of the colour.
 *  This is not full Wu anti-aliasing (PL110 has no alpha blending),
 *  but it is observably softer than the crisp Bresenham fallback and
 *  proves the toggle is wired end-to-end.
 * ============================================================ */
/* kprintf is declared by kernel.h (syscall = int) — don't re-declare. */

static int g_fb_aa = 0;

static unsigned short rgb565_from24(long c24) {
    return rgb565((int)((c24 >> 16) & 0xFF),
                  (int)((c24 >>  8) & 0xFF),
                  (int)( c24        & 0xFF));
}

static unsigned short rgb565_dim50(long c24) {
    return rgb565((int)(((c24 >> 16) & 0xFF) >> 1),
                  (int)(((c24 >>  8) & 0xFF) >> 1),
                  (int)(( c24        & 0xFF) >> 1));
}

static void fb_line_internal(int x0, int y0, int x1, int y1, long c24) {
    draw_line(x0, y0, x1, y1, rgb565_from24(c24));
    if (g_fb_aa) {
        unsigned short cd = rgb565_dim50(c24);
        draw_line(x0,     y0 + 1, x1,     y1 + 1, cd);
        draw_line(x0 + 1, y0,     x1 + 1, y1,     cd);
    }
}

value_t fb_pixel(int n, value_t *a) {
    int x, y; long c24;
    if (n < 3) return v_int(0);
    x   = (int)a[0].i;
    y   = (int)a[1].i;
    c24 = a[2].i;
    put_pixel_pub(x, y, rgb565_from24(c24));
    kprintf("[aipl] fb_pixel x=%d y=%d color=0x%06x\r\n",
            x, y, (unsigned)(c24 & 0xFFFFFFu));
    return v_int(1);
}

value_t fb_line(int n, value_t *a) {
    int x0, y0, x1, y1; long c24;
    if (n < 5) return v_int(0);
    x0  = (int)a[0].i; y0 = (int)a[1].i;
    x1  = (int)a[2].i; y1 = (int)a[3].i;
    c24 = a[4].i;
    fb_line_internal(x0, y0, x1, y1, c24);
    kprintf("[aipl] fb_line x0=%d y0=%d x1=%d y1=%d color=0x%06x aa=%d\r\n",
            x0, y0, x1, y1, (unsigned)(c24 & 0xFFFFFFu), g_fb_aa);
    return v_int(1);
}

value_t fb_circle(int n, value_t *a) {
    int cx, cy, r;
    long c24;
    unsigned short c;
    int x, y, err;
    if (n < 4) return v_int(0);
    cx  = (int)a[0].i; cy = (int)a[1].i; r = (int)a[2].i;
    c24 = a[3].i;
    c   = rgb565_from24(c24);
    /* Midpoint circle algorithm — paint all 8 octants. */
    x = r; y = 0; err = 1 - r;
    while (x >= y) {
        put_pixel_pub(cx + x, cy + y, c);
        put_pixel_pub(cx + y, cy + x, c);
        put_pixel_pub(cx - y, cy + x, c);
        put_pixel_pub(cx - x, cy + y, c);
        put_pixel_pub(cx - x, cy - y, c);
        put_pixel_pub(cx - y, cy - x, c);
        put_pixel_pub(cx + y, cy - x, c);
        put_pixel_pub(cx + x, cy - y, c);
        if (g_fb_aa) {
            unsigned short cd = rgb565_dim50(c24);
            put_pixel_pub(cx + x + 1, cy + y,     cd);
            put_pixel_pub(cx + y,     cy + x + 1, cd);
            put_pixel_pub(cx - y,     cy + x + 1, cd);
            put_pixel_pub(cx - x - 1, cy + y,     cd);
            put_pixel_pub(cx - x - 1, cy - y,     cd);
            put_pixel_pub(cx - y,     cy - x - 1, cd);
            put_pixel_pub(cx + y,     cy - x - 1, cd);
            put_pixel_pub(cx + x + 1, cy - y,     cd);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
    kprintf("[aipl] fb_circle cx=%d cy=%d r=%d color=0x%06x aa=%d\r\n",
            cx, cy, r, (unsigned)(c24 & 0xFFFFFFu), g_fb_aa);
    return v_int(1);
}

value_t fb_triangle(int n, value_t *a) {
    int x0, y0, x1, y1, x2, y2;
    long c24;
    if (n < 7) return v_int(0);
    x0 = (int)a[0].i; y0 = (int)a[1].i;
    x1 = (int)a[2].i; y1 = (int)a[3].i;
    x2 = (int)a[4].i; y2 = (int)a[5].i;
    c24 = a[6].i;
    fb_line_internal(x0, y0, x1, y1, c24);
    fb_line_internal(x1, y1, x2, y2, c24);
    fb_line_internal(x2, y2, x0, y0, c24);
    kprintf("[aipl] fb_triangle p0=(%d,%d) p1=(%d,%d) p2=(%d,%d) "
            "color=0x%06x aa=%d\r\n",
            x0, y0, x1, y1, x2, y2,
            (unsigned)(c24 & 0xFFFFFFu), g_fb_aa);
    return v_int(1);
}

value_t fb_rrect(int n, value_t *a) {
    int x, y, w, h, r;
    long c24;
    unsigned short c;
    int px, py, err;
    if (n < 6) return v_int(0);
    x = (int)a[0].i; y = (int)a[1].i;
    w = (int)a[2].i; h = (int)a[3].i;
    r = (int)a[4].i;
    c24 = a[5].i;
    if (r < 0)       r = 0;
    if (r > w / 2)   r = w / 2;
    if (r > h / 2)   r = h / 2;
    /* Four straight edges. */
    fb_line_internal(x + r,     y,           x + w - r - 1, y,             c24);
    fb_line_internal(x + r,     y + h - 1,   x + w - r - 1, y + h - 1,     c24);
    fb_line_internal(x,         y + r,       x,             y + h - r - 1, c24);
    fb_line_internal(x + w - 1, y + r,       x + w - 1,     y + h - r - 1, c24);
    /* Four corner arcs via midpoint quarter-circle. */
    c = rgb565_from24(c24);
    px = r; py = 0; err = 1 - r;
    while (px >= py) {
        /* top-right corner (origin cx=x+w-1-r, cy=y+r) */
        put_pixel_pub(x + w - 1 - r + px, y + r - py,             c);
        put_pixel_pub(x + w - 1 - r + py, y + r - px,             c);
        /* top-left  (origin cx=x+r, cy=y+r) */
        put_pixel_pub(x + r - px,         y + r - py,             c);
        put_pixel_pub(x + r - py,         y + r - px,             c);
        /* bottom-right (origin cx=x+w-1-r, cy=y+h-1-r) */
        put_pixel_pub(x + w - 1 - r + px, y + h - 1 - r + py,     c);
        put_pixel_pub(x + w - 1 - r + py, y + h - 1 - r + px,     c);
        /* bottom-left  (origin cx=x+r, cy=y+h-1-r) */
        put_pixel_pub(x + r - px,         y + h - 1 - r + py,     c);
        put_pixel_pub(x + r - py,         y + h - 1 - r + px,     c);
        py++;
        if (err < 0) err += 2 * py + 1;
        else { px--; err += 2 * (py - px) + 1; }
    }
    kprintf("[aipl] fb_rrect x=%d y=%d w=%d h=%d r=%d color=0x%06x aa=%d\r\n",
            x, y, w, h, r, (unsigned)(c24 & 0xFFFFFFu), g_fb_aa);
    return v_int(1);
}

value_t fb_dashed_line(int n, value_t *a) {
    int x0, y0, x1, y1;
    long c24;
    int dash;
    unsigned short c;
    int dx, dy, sx, sy, err, e2, step;
    if (n < 6) return v_int(0);
    x0 = (int)a[0].i; y0 = (int)a[1].i;
    x1 = (int)a[2].i; y1 = (int)a[3].i;
    c24 = a[4].i;
    dash = (int)a[5].i;
    if (dash < 1) dash = 4;
    c  = rgb565_from24(c24);
    dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx + dy; step = 0;
    for (;;) {
        if ((step / dash) % 2 == 0) put_pixel_pub(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        step++;
    }
    kprintf("[aipl] fb_dashed_line color=0x%06x dash=%d\r\n",
            (unsigned)(c24 & 0xFFFFFFu), dash);
    return v_int(1);
}

value_t fb_set_aa(int n, value_t *a) {
    int on;
    if (n < 1) return v_int(g_fb_aa);
    on = a[0].i ? 1 : 0;
    g_fb_aa = on;
    kprintf("[aipl] fb_aa=%d\r\n", g_fb_aa);
    return v_int(g_fb_aa);
}

value_t fb_get_aa(int n, value_t *a) {
    (void)n; (void)a;
    return v_int(g_fb_aa);
}

/* fb_fill_rect — P4 needs a filled-rect primitive for the bar-chart.
   Lives in G1 surface since it is a fundamental drawing primitive. */
value_t fb_fill_rect(int n, value_t *a) {
    int x, y, w, h;
    long c24;
    if (n < 5) return v_int(0);
    x = (int)a[0].i; y = (int)a[1].i;
    w = (int)a[2].i; h = (int)a[3].i;
    c24 = a[4].i;
    fill_rect_pub(x, y, w, h, rgb565_from24(c24));
    kprintf("[aipl] fb_fill_rect x=%d y=%d w=%d h=%d color=0x%06x\r\n",
            x, y, w, h, (unsigned)(c24 & 0xFFFFFFu));
    return v_int(1);
}

/* ============================================================
 *  P4: SchedulingVisualizer.
 *
 *  Background Xinu thread that samples thrtab[] every `period`
 *  milliseconds, classifies each thread into one of four buckets —
 *  CURR / READY / SLEEP / RECV — emits a CSV line to the serial
 *  console, and (if framebuffer drawing is enabled) renders a bar
 *  chart in the upper-right corner using the G1 fb_fill_rect
 *  primitive.
 *
 *  AIPL externs:
 *    sched_viz_start(period_ms)   ->  1 ok / 0 already running
 *    sched_viz_stop()             ->  0
 *    sched_viz_sample()           ->  combined int (ready<<24|curr<<16|sleep<<8|recv)
 *
 *  CSV format: `csv,t=<ms>,ready=N,curr=N,sleep=N,recv=N`
 * ============================================================ */
#include <clock.h>
#include <thread.h>

#define P4_BAR_X        470
#define P4_BAR_Y        70
#define P4_BAR_W        18
#define P4_BAR_H_MAX    96
#define P4_BAR_GAP      4
#define P4_SCALE_DEN    8       /* assume up to ~8 threads per bucket */

static volatile int g_viz_running = 0;
static volatile int g_viz_period  = 500;
static volatile int g_viz_samples = 0;

static void p4_count_states(int *ready, int *curr, int *slp, int *rcv) {
    int i;
    *ready = *curr = *slp = *rcv = 0;
    for (i = 0; i < NTHREAD; i++) {
        switch (thrtab[i].state) {
            case THRCURR:   (*curr)++;   break;
            case THRREADY:  (*ready)++;  break;
            case THRSLEEP:  (*slp)++;    break;
            case THRRECV:
            case THRWAIT:
            case THRTMOUT:  (*rcv)++;    break;
            default: break;
        }
    }
}

static void p4_draw_bars(int ready, int curr, int slp, int rcv) {
    unsigned short bg     = rgb565(20, 30, 60);
    unsigned short c_red  = rgb565(50, 200, 50);
    unsigned short c_cur  = rgb565(255, 100, 100);
    unsigned short c_slp  = rgb565(100, 130, 220);
    unsigned short c_rcv  = rgb565(220, 200, 80);
    int total_w = 4 * P4_BAR_W + 3 * P4_BAR_GAP;
    int hr, hc, hs, hv;
    int pitch = P4_BAR_W + P4_BAR_GAP;
    /* Clear the chart area before drawing. */
    fill_rect_pub(P4_BAR_X - 2, P4_BAR_Y - 2,
                  total_w + 4, P4_BAR_H_MAX + 4, bg);
    hr = (ready * P4_BAR_H_MAX) / P4_SCALE_DEN; if (hr > P4_BAR_H_MAX) hr = P4_BAR_H_MAX;
    hc = (curr  * P4_BAR_H_MAX) / P4_SCALE_DEN; if (hc > P4_BAR_H_MAX) hc = P4_BAR_H_MAX;
    hs = (slp   * P4_BAR_H_MAX) / P4_SCALE_DEN; if (hs > P4_BAR_H_MAX) hs = P4_BAR_H_MAX;
    hv = (rcv   * P4_BAR_H_MAX) / P4_SCALE_DEN; if (hv > P4_BAR_H_MAX) hv = P4_BAR_H_MAX;
    fill_rect_pub(P4_BAR_X + 0 * pitch, P4_BAR_Y + P4_BAR_H_MAX - hr, P4_BAR_W, hr, c_red);
    fill_rect_pub(P4_BAR_X + 1 * pitch, P4_BAR_Y + P4_BAR_H_MAX - hc, P4_BAR_W, hc, c_cur);
    fill_rect_pub(P4_BAR_X + 2 * pitch, P4_BAR_Y + P4_BAR_H_MAX - hs, P4_BAR_W, hs, c_slp);
    fill_rect_pub(P4_BAR_X + 3 * pitch, P4_BAR_Y + P4_BAR_H_MAX - hv, P4_BAR_W, hv, c_rcv);
    kprintf("[aipl] p4_bar ready=%d curr=%d sleep=%d recv=%d "
            "h=(%d,%d,%d,%d)\r\n",
            ready, curr, slp, rcv, hr, hc, hs, hv);
}

static thread p4_sched_viz_main(void) {
    int ready, curr, slp, rcv;
    while (g_viz_running) {
        p4_count_states(&ready, &curr, &slp, &rcv);
        kprintf("csv,t=%d,ready=%d,curr=%d,sleep=%d,recv=%d\r\n",
                (int)clkticks, ready, curr, slp, rcv);
        p4_draw_bars(ready, curr, slp, rcv);
        g_viz_samples++;
        sleep(g_viz_period);
    }
    return OK;
}

value_t sched_viz_start(int n, value_t *a) {
    int period;
    tid_typ tid;
    if (g_viz_running) return v_int(0);
    period = (n >= 1) ? (int)a[0].i : 500;
    if (period < 50) period = 50;
    g_viz_period  = period;
    g_viz_samples = 0;
    g_viz_running = 1;
    tid = create((void*)p4_sched_viz_main, 4096, INITPRIO,
                 "aipl-viz", 0);
    if (tid != SYSERR) {
        ready(tid, RESCHED_NO);
        kprintf("[aipl] sched-viz started period=%d\r\n", period);
        return v_int(1);
    }
    g_viz_running = 0;
    return v_int(0);
}

value_t sched_viz_stop(int n, value_t *a) {
    (void)n; (void)a;
    g_viz_running = 0;
    kprintf("[aipl] sched-viz stopped samples=%d\r\n", g_viz_samples);
    return v_int(0);
}

value_t sched_viz_sample(int n, value_t *a) {
    int ready, curr, slp, rcv;
    (void)n; (void)a;
    p4_count_states(&ready, &curr, &slp, &rcv);
    /* Pack into a single int — caller can decode if needed. */
    return v_int(((long)ready << 24) | ((long)curr << 16) |
                 ((long)slp   <<  8) |  (long)rcv);
}

/* ============================================================
 *  G2: bitmap font text rendering (callable from AIPL).
 *
 *  Reuses the WM's 8x8 font (FONT_SCALE=2 → 16x16 px cell) via
 *  draw_string_pub / draw_char_pub.  Background is transparent —
 *  callers can pre-fill_rect for an opaque box.
 *
 *  AIPL externs (return 1 on success, 0 on bad arg):
 *    fb_text(x, y, str, color24)
 *    fb_char(x, y, ch_code, color24)        — ch_code is ASCII int
 *    fb_text_size(str)                       -> width_px = len * 16
 *
 *  CHAR_W / CHAR_H are 16 px on the WM (8 * FONT_SCALE).  G2 inherits
 *  that — clients lay out grids on 16-px boundaries.
 * ============================================================ */
extern void draw_char_pub(int x, int y, char c, unsigned short fg);

value_t fb_text(int n, value_t *a) {
    int x, y;
    const char *s;
    long c24;
    int len;
    if (n < 4) return v_int(0);
    x   = (int)a[0].i;
    y   = (int)a[1].i;
    s   = (a[2].tag == V_STR && a[2].s != NULL) ? a[2].s : "";
    c24 = a[3].i;
    draw_string_pub(x, y, s, rgb565_from24(c24));
    len = 0; while (s[len]) len++;
    kprintf("[aipl] fb_text x=%d y=%d len=%d color=0x%06x str=%s\r\n",
            x, y, len, (unsigned)(c24 & 0xFFFFFFu), s);
    return v_int(1);
}

value_t fb_char(int n, value_t *a) {
    int x, y, ch;
    long c24;
    if (n < 4) return v_int(0);
    x   = (int)a[0].i;
    y   = (int)a[1].i;
    ch  = (int)a[2].i & 0xFF;
    c24 = a[3].i;
    draw_char_pub(x, y, (char)ch, rgb565_from24(c24));
    kprintf("[aipl] fb_char x=%d y=%d ch=0x%02x color=0x%06x\r\n",
            x, y, ch, (unsigned)(c24 & 0xFFFFFFu));
    return v_int(1);
}

value_t fb_text_size(int n, value_t *a) {
    const char *s;
    int len;
    if (n < 1) return v_int(0);
    s   = (a[0].tag == V_STR && a[0].s != NULL) ? a[0].s : "";
    len = 0; while (s[len]) len++;
    /* width = chars * (8 * FONT_SCALE)  — WM's CHAR_W = 16. */
    return v_int((long)len * 16);
}

/* ============================================================
 *  G3: PL050 mouse → AIPL actor message dispatch.
 *
 *  Three subscriber tables (click / move / release).  Each entry is
 *  (target actor obj_id, method name).  When the WM's main loop
 *  observes a mouse edge, it calls the corresponding dispatcher,
 *  which iterates the table and enqueues `method(x, y, buttons)`
 *  on every registered actor.
 *
 *  AIPL externs (all return 1 on success, 0 on bad arg / full table):
 *    mouse_on_click(self, "method")
 *    mouse_on_move(self, "method")
 *    mouse_on_release(self, "method")
 *    mouse_unsubscribe(self)         — drops `self` from all 3 tables
 *    mouse_inject(kind, x, y, btn)   — synthesise an event; kind:
 *                                       0=click, 1=move, 2=release
 *    mouse_state()                   -> packed int (x<<20)|(y<<8)|btn
 *
 *  Serial marker: `[aipl] mouse_event kind=click|move|release x=… y=…
 *  btn=… subs=…` — emitted once per dispatched event so smokes can
 *  count without rendering the framebuffer.
 * ============================================================ */
#define MAX_MOUSE_SUBS 8

typedef struct {
    int  valid;
    int  target;            /* actor obj_id */
    char method[32];
} mouse_sub_t;

static mouse_sub_t g_click_subs  [MAX_MOUSE_SUBS];
static mouse_sub_t g_move_subs   [MAX_MOUSE_SUBS];
static mouse_sub_t g_release_subs[MAX_MOUSE_SUBS];

static volatile int g_mouse_x   = 0;
static volatile int g_mouse_y   = 0;
static volatile int g_mouse_btn = 0;

static int mouse_sub_add(mouse_sub_t *tbl, int target, const char *method) {
    int i;
    for (i = 0; i < MAX_MOUSE_SUBS; i++) {
        if (!tbl[i].valid) {
            tbl[i].valid  = 1;
            tbl[i].target = target;
            copy_str(tbl[i].method, method ? method : "", sizeof tbl[i].method);
            return 1;
        }
    }
    return 0;
}

static int mouse_sub_remove(mouse_sub_t *tbl, int target) {
    int i, n = 0;
    for (i = 0; i < MAX_MOUSE_SUBS; i++) {
        if (tbl[i].valid && tbl[i].target == target) {
            tbl[i].valid = 0;
            n++;
        }
    }
    return n;
}

static int mouse_dispatch(mouse_sub_t *tbl, const char *kind,
                          int x, int y, int btn) {
    int i, fired = 0;
    value_t args[3];
    args[0] = v_int(x);
    args[1] = v_int(y);
    args[2] = v_int(btn);
    for (i = 0; i < MAX_MOUSE_SUBS; i++) {
        if (tbl[i].valid) {
            abcl_enqueue(-1, tbl[i].target, tbl[i].method, 3, args);
            fired++;
        }
    }
    kprintf("[aipl] mouse_event kind=%s x=%d y=%d btn=%d subs=%d\r\n",
            kind, x, y, btn, fired);
    return fired;
}

/* Public entry points — called from wm.c's main loop. */
void abcl_mouse_dispatch_click(int x, int y, int btn) {
    g_mouse_x = x; g_mouse_y = y; g_mouse_btn = btn;
    mouse_dispatch(g_click_subs, "click", x, y, btn);
}

void abcl_mouse_dispatch_move(int x, int y, int btn) {
    g_mouse_x = x; g_mouse_y = y; g_mouse_btn = btn;
    mouse_dispatch(g_move_subs, "move", x, y, btn);
}

void abcl_mouse_dispatch_release(int x, int y, int btn) {
    g_mouse_x = x; g_mouse_y = y; g_mouse_btn = btn;
    mouse_dispatch(g_release_subs, "release", x, y, btn);
}

static int unwrap_actor_id(value_t v) {
    return (v.tag == V_OBJ) ? v.obj_id : (int)v.i;
}

value_t mouse_on_click(int n, value_t *a) {
    int target;
    const char *m;
    if (n < 2) return v_int(0);
    target = unwrap_actor_id(a[0]);
    m      = (a[1].tag == V_STR && a[1].s) ? a[1].s : "";
    kprintf("[aipl] mouse_on_click target=%d method=%s\r\n", target, m);
    return v_int(mouse_sub_add(g_click_subs, target, m));
}

value_t mouse_on_move(int n, value_t *a) {
    int target;
    const char *m;
    if (n < 2) return v_int(0);
    target = unwrap_actor_id(a[0]);
    m      = (a[1].tag == V_STR && a[1].s) ? a[1].s : "";
    kprintf("[aipl] mouse_on_move target=%d method=%s\r\n", target, m);
    return v_int(mouse_sub_add(g_move_subs, target, m));
}

value_t mouse_on_release(int n, value_t *a) {
    int target;
    const char *m;
    if (n < 2) return v_int(0);
    target = unwrap_actor_id(a[0]);
    m      = (a[1].tag == V_STR && a[1].s) ? a[1].s : "";
    kprintf("[aipl] mouse_on_release target=%d method=%s\r\n", target, m);
    return v_int(mouse_sub_add(g_release_subs, target, m));
}

value_t mouse_unsubscribe(int n, value_t *a) {
    int target, removed;
    if (n < 1) return v_int(0);
    target  = unwrap_actor_id(a[0]);
    removed = 0;
    removed += mouse_sub_remove(g_click_subs,   target);
    removed += mouse_sub_remove(g_move_subs,    target);
    removed += mouse_sub_remove(g_release_subs, target);
    kprintf("[aipl] mouse_unsubscribe target=%d removed=%d\r\n",
            target, removed);
    return v_int(removed);
}

value_t mouse_inject(int n, value_t *a) {
    int kind, x, y, btn;
    if (n < 4) return v_int(0);
    kind = (int)a[0].i;
    x    = (int)a[1].i;
    y    = (int)a[2].i;
    btn  = (int)a[3].i;
    switch (kind) {
        case 0: abcl_mouse_dispatch_click  (x, y, btn); break;
        case 1: abcl_mouse_dispatch_move   (x, y, btn); break;
        case 2: abcl_mouse_dispatch_release(x, y, btn); break;
        default: return v_int(0);
    }
    return v_int(1);
}

value_t mouse_state(int n, value_t *a) {
    (void)n; (void)a;
    /* (x & 0xFFF) << 20 | (y & 0xFFF) << 8 | (btn & 0xFF)
       Layout chosen so a single AIPL int print is readable. */
    return v_int(((long)(g_mouse_x & 0xFFF) << 20) |
                 ((long)(g_mouse_y & 0xFFF) <<  8) |
                  (long)(g_mouse_btn & 0xFF));
}

/* ============================================================
 *  G5: image bitmap blit (callable from AIPL).
 *
 *  Source pixel format = 0xRRGGBB long (one AIPL int per pixel),
 *  packed row-major.  Quantised to BGR565 on write — identical to the
 *  G1 colour pipeline so existing colours round-trip.
 *
 *  AIPL externs:
 *    fb_image(x, y, w, h, arr)         — blit w*h pixels from int array
 *    fb_image_solid(x, y, w, h, c24)   — no array; flat colour blit
 *
 *  Array source lives in the F4 pool (abcl_xinu_str.c).  Cap is 256
 *  pixels per array → max 16×16 image; larger images need multiple
 *  blits or a future F4 capacity bump.  The blit clips against the
 *  array length, so passing a too-short array is safe — the marker
 *  reports actual painted count so the smoke can verify.
 * ============================================================ */
extern int abcl_array_view(int id, long **out_data, int *out_len);

value_t fb_image(int n, value_t *a) {
    int x, y, w, h;
    int arr_id, arr_len = 0;
    long *arr_data = NULL;
    int row, col, painted = 0, clipped = 0;
    if (n < 5) return v_int(0);
    x = (int)a[0].i; y = (int)a[1].i;
    w = (int)a[2].i; h = (int)a[3].i;
    arr_id = (a[4].tag == V_OBJ) ? a[4].obj_id : (int)a[4].i;
    if (!abcl_array_view(arr_id, &arr_data, &arr_len)) {
        kprintf("[aipl] fb_image bad arr=%d\r\n", arr_id);
        return v_int(0);
    }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    for (row = 0; row < h; row++) {
        for (col = 0; col < w; col++) {
            int idx = row * w + col;
            if (idx >= arr_len) { clipped++; continue; }
            put_pixel_pub(x + col, y + row, rgb565_from24(arr_data[idx]));
            painted++;
        }
    }
    kprintf("[aipl] fb_image x=%d y=%d w=%d h=%d arr=%d "
            "painted=%d clipped=%d\r\n",
            x, y, w, h, arr_id, painted, clipped);
    return v_int(painted);
}

value_t fb_image_solid(int n, value_t *a) {
    int x, y, w, h;
    long c24;
    if (n < 5) return v_int(0);
    x = (int)a[0].i; y = (int)a[1].i;
    w = (int)a[2].i; h = (int)a[3].i;
    c24 = a[4].i;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    fill_rect_pub(x, y, w, h, rgb565_from24(c24));
    kprintf("[aipl] fb_image_solid x=%d y=%d w=%d h=%d color=0x%06x\r\n",
            x, y, w, h, (unsigned)(c24 & 0xFFFFFFu));
    return v_int(w * h);
}

/* ============================================================
 *  F1: distributed checkpoint — local-node snapshot/restore.
 *
 *  Phase 1 of distrib: in-process snapshots tagged by string name.
 *  Each snapshot copies the full value_t fields[MAX_FIELDS] of an
 *  actor along with its class_id, so a later restore puts the actor
 *  back in the same logical state regardless of intervening sends.
 *
 *  The N3 (multi-Pi cluster) phase will hand snapshot blobs over the
 *  network — bytes-on-the-wire format is intentionally the value_t
 *  array for now; promoting it to a portable serialisation is the
 *  next step.
 *
 *  AIPL externs:
 *    checkpoint_save(self, "tag")     -> slot_id, or -1 on full table
 *    checkpoint_load(self, "tag")     -> 1 ok / 0 not found
 *    checkpoint_list()                -> count of live snapshots
 *    checkpoint_clear()               -> count cleared
 *
 *  Serial marker: `[aipl] chkpt op=save|load|clear actor=N tag=… …`
 * ============================================================ */
#define MAX_CHKPT 8

/* Field count exposed by the runtime; cached on first save so we
 * don't have to ask per-slot. */
extern int abcl_object_field_count(void);
extern int abcl_object_field_get(int obj_id, int field_idx, value_t *out);
extern int abcl_object_field_set(int obj_id, int field_idx, value_t v);
extern int abcl_object_class_id(int obj_id);

typedef struct {
    int     valid;
    int     actor_id;
    int     class_id;
    int     n_fields;
    char    tag[32];
    value_t fields[16];     /* must be ≥ MAX_FIELDS in c_translator.ml */
} chkpt_slot_t;

static chkpt_slot_t g_chkpt[MAX_CHKPT];

static int chkpt_find(int actor_id, const char *tag) {
    int i;
    for (i = 0; i < MAX_CHKPT; i++) {
        if (g_chkpt[i].valid && g_chkpt[i].actor_id == actor_id) {
            int j;
            for (j = 0; tag[j] && g_chkpt[i].tag[j]; j++) {
                if (tag[j] != g_chkpt[i].tag[j]) goto next;
            }
            if (tag[j] == g_chkpt[i].tag[j]) return i;
        }
        next: ;
    }
    return -1;
}

static int chkpt_alloc(int actor_id, const char *tag) {
    int existing = chkpt_find(actor_id, tag);
    int i;
    if (existing >= 0) return existing;     /* overwrite same tag */
    for (i = 0; i < MAX_CHKPT; i++) {
        if (!g_chkpt[i].valid) return i;
    }
    return -1;
}

value_t checkpoint_save(int n, value_t *a) {
    int target, slot, n_fields, j, copied;
    const char *tag;
    if (n < 2) return v_int(-1);
    target = (a[0].tag == V_OBJ) ? a[0].obj_id : (int)a[0].i;
    tag    = (a[1].tag == V_STR && a[1].s) ? a[1].s : "";
    n_fields = abcl_object_field_count();
    if (n_fields > 16) n_fields = 16;
    slot = chkpt_alloc(target, tag);
    if (slot < 0) {
        kprintf("[aipl] chkpt op=save actor=%d tag=%s status=table-full\r\n",
                target, tag);
        return v_int(-1);
    }
    copied = 0;
    for (j = 0; j < n_fields; j++) {
        if (abcl_object_field_get(target, j, &g_chkpt[slot].fields[j])) copied++;
    }
    if (copied == 0) {
        kprintf("[aipl] chkpt op=save actor=%d tag=%s status=bad-actor\r\n",
                target, tag);
        return v_int(-1);
    }
    g_chkpt[slot].valid    = 1;
    g_chkpt[slot].actor_id = target;
    g_chkpt[slot].class_id = abcl_object_class_id(target);
    g_chkpt[slot].n_fields = copied;
    {
        int k;
        for (k = 0; k < 31 && tag[k]; k++) g_chkpt[slot].tag[k] = tag[k];
        g_chkpt[slot].tag[k] = '\0';
    }
    kprintf("[aipl] chkpt op=save actor=%d tag=%s slot=%d "
            "class=%d fields=%d\r\n",
            target, tag, slot, g_chkpt[slot].class_id, copied);
    return v_int(slot);
}

value_t checkpoint_load(int n, value_t *a) {
    int target, slot, j, restored;
    const char *tag;
    if (n < 2) return v_int(0);
    target = (a[0].tag == V_OBJ) ? a[0].obj_id : (int)a[0].i;
    tag    = (a[1].tag == V_STR && a[1].s) ? a[1].s : "";
    slot   = chkpt_find(target, tag);
    if (slot < 0) {
        kprintf("[aipl] chkpt op=load actor=%d tag=%s status=not-found\r\n",
                target, tag);
        return v_int(0);
    }
    restored = 0;
    for (j = 0; j < g_chkpt[slot].n_fields; j++) {
        if (abcl_object_field_set(target, j, g_chkpt[slot].fields[j])) restored++;
    }
    kprintf("[aipl] chkpt op=load actor=%d tag=%s slot=%d fields=%d\r\n",
            target, tag, slot, restored);
    return v_int(restored > 0 ? 1 : 0);
}

value_t checkpoint_list(int n, value_t *a) {
    int i, count = 0;
    (void)n; (void)a;
    for (i = 0; i < MAX_CHKPT; i++) if (g_chkpt[i].valid) count++;
    kprintf("[aipl] chkpt op=list count=%d\r\n", count);
    return v_int(count);
}

value_t checkpoint_clear(int n, value_t *a) {
    int i, cleared = 0;
    (void)n; (void)a;
    for (i = 0; i < MAX_CHKPT; i++) {
        if (g_chkpt[i].valid) { g_chkpt[i].valid = 0; cleared++; }
    }
    kprintf("[aipl] chkpt op=clear count=%d\r\n", cleared);
    return v_int(cleared);
}

/* ============================================================
 *  F2: Last-Writer-Wins cell semantics.
 *
 *  Direct port of the browser-abcl spreadsheet's LWW store.  Cells
 *  are keyed by an int; each cell carries (value, ts).  A write
 *  commits iff its `ts` is strictly greater than the stored ts; ties
 *  are rejected.  This matches the spreadsheet's convergence
 *  guarantee — independent writers eventually agree on the cell with
 *  the highest ts.
 *
 *  AIPL externs:
 *    lww_write(key, value, ts)   -> 1 accepted / 0 rejected (older)
 *    lww_read(key)               -> value (0 if absent)
 *    lww_ts(key)                 -> ts (0 if absent)
 *    lww_size()                  -> live cell count
 *    lww_clear()                 -> cleared cell count
 *    lww_tick()                  -> next Lamport tick (monotonic)
 *
 *  Serial marker: `[aipl] lww op=write|reject|clear key=K value=V ts=T`
 *  — only the writes log; reads stay quiet so the trace remains
 *  legible at high throughput.
 * ============================================================ */
#define MAX_LWW_CELLS 32

typedef struct {
    int   valid;
    int   key;
    long  value;
    long  ts;
} lww_cell_t;

static lww_cell_t g_lww[MAX_LWW_CELLS];
static long       g_lamport = 0;

static int lww_find(int key) {
    int i;
    for (i = 0; i < MAX_LWW_CELLS; i++) {
        if (g_lww[i].valid && g_lww[i].key == key) return i;
    }
    return -1;
}

static int lww_alloc(int key) {
    int existing = lww_find(key);
    int i;
    if (existing >= 0) return existing;
    for (i = 0; i < MAX_LWW_CELLS; i++) {
        if (!g_lww[i].valid) return i;
    }
    return -1;
}

value_t lww_write(int n, value_t *a) {
    int key, slot;
    long value, ts;
    if (n < 3) return v_int(0);
    key   = (int)a[0].i;
    value = a[1].i;
    ts    = a[2].i;
    slot  = lww_find(key);
    if (slot >= 0 && ts <= g_lww[slot].ts) {
        kprintf("[aipl] lww op=reject key=%d value=%ld ts=%ld "
                "stored_ts=%ld\r\n",
                key, value, ts, g_lww[slot].ts);
        return v_int(0);
    }
    if (slot < 0) {
        slot = lww_alloc(key);
        if (slot < 0) {
            kprintf("[aipl] lww op=reject key=%d ts=%ld status=table-full\r\n",
                    key, ts);
            return v_int(0);
        }
    }
    g_lww[slot].valid = 1;
    g_lww[slot].key   = key;
    g_lww[slot].value = value;
    g_lww[slot].ts    = ts;
    /* Advance the global Lamport clock to the highest observed ts so a
       subsequent lww_tick() never collides with this writer. */
    if (ts > g_lamport) g_lamport = ts;
    kprintf("[aipl] lww op=write key=%d value=%ld ts=%ld slot=%d\r\n",
            key, value, ts, slot);
    return v_int(1);
}

value_t lww_read(int n, value_t *a) {
    int key, slot;
    if (n < 1) return v_int(0);
    key  = (int)a[0].i;
    slot = lww_find(key);
    if (slot < 0) return v_int(0);
    return v_int(g_lww[slot].value);
}

value_t lww_ts(int n, value_t *a) {
    int key, slot;
    if (n < 1) return v_int(0);
    key  = (int)a[0].i;
    slot = lww_find(key);
    if (slot < 0) return v_int(0);
    return v_int(g_lww[slot].ts);
}

value_t lww_size(int n, value_t *a) {
    int i, count = 0;
    (void)n; (void)a;
    for (i = 0; i < MAX_LWW_CELLS; i++) if (g_lww[i].valid) count++;
    return v_int(count);
}

value_t lww_clear(int n, value_t *a) {
    int i, cleared = 0;
    (void)n; (void)a;
    for (i = 0; i < MAX_LWW_CELLS; i++) {
        if (g_lww[i].valid) { g_lww[i].valid = 0; cleared++; }
    }
    kprintf("[aipl] lww op=clear count=%d\r\n", cleared);
    return v_int(cleared);
}

value_t lww_tick(int n, value_t *a) {
    (void)n; (void)a;
    g_lamport = g_lamport + 1;
    return v_int(g_lamport);
}

/* ============================================================
 *  S3 DeadlineHints — AIPL surface for the kernel-side
 *  setdeadline() syscall (system/setdeadline.c).
 *
 *  Usage from AIPL:
 *      set_deadline(actor_ref, ms)
 *  where actor_ref is a TAny (typically `self` or `var x = new C()`)
 *  and ms is the relative deadline in milliseconds (= clock ticks,
 *  since CLKTICKS_PER_SEC=1000).
 *
 *  Returns 1 on success, 0 if the obj_id has no live tid, -1 on
 *  malformed args.  After dispatch the kernel auto-clears the
 *  deadline; call set_deadline again before the next urgent slice.
 * ============================================================ */
extern int abcl_object_tid(int obj_id);
extern int setdeadline(int tid, int ticks_from_now);

value_t set_deadline(int n, value_t *a) {
    int obj_id;
    int ticks;
    int tid;
    int rc;
    if (n < 2) return v_int(-1);
    obj_id = (a[0].tag == V_OBJ) ? a[0].obj_id : (int)a[0].i;
    ticks  = (int)a[1].i;
    tid    = abcl_object_tid(obj_id);
    if (tid < 0) {
        kprintf("[aipl] set_deadline bad obj_id=%d\r\n", obj_id);
        return v_int(0);
    }
    rc = setdeadline((tid_typ)tid, ticks);
    kprintf("[aipl] set_deadline obj=%d tid=%d ms=%d rc=%d\r\n",
            obj_id, tid, ticks, rc);
    return v_int(rc == OK ? 1 : 0);
}
