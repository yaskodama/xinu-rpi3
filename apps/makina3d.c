/* apps/makina3d.c — native software 3-D viewer ("kernel Blender") for MAKINA-7.
 * Integer fixed-point (kernel is soft-float): rotate the mesh by yaw/pitch,
 * back-face cull, flat-shade by rotated normal vs a head-on light, depth-bucket
 * (painter's) and rasterise filled 24-bit triangles into an OFFSCREEN buffer,
 * then blit once (flicker-free).  Rotate with the mouse (drag) OR the on-screen
 * buttons (Y-/Y+/P-/P+/SPIN). */
#ifdef _XINU_PLATFORM_ARM_RPI3_
#include <gwm.h>
#include <gvideo.h>
#include <makina_mesh.h>

extern void fill_rect(int, int, int, int, unsigned int);
extern void gv_blit(int, int, int, int, const unsigned int *, int);
extern void draw_string_at(int, int, const char *, unsigned int, unsigned int);
extern int  g_force_redraw;

static const short SINT[256] = {
  0,101,201,301,401,501,601,700,799,897,995,1092,1189,1285,1380,1474,
  1567,1660,1751,1842,1931,2019,2106,2191,2276,2359,2440,2520,2598,2675,2751,2824,
  2896,2967,3035,3102,3166,3229,3290,3349,3406,3461,3513,3564,3612,3659,3703,3745,
  3784,3822,3857,3889,3920,3948,3973,3996,4017,4036,4052,4065,4076,4085,4091,4095,
  4096,4095,4091,4085,4076,4065,4052,4036,4017,3996,3973,3948,3920,3889,3857,3822,
  3784,3745,3703,3659,3612,3564,3513,3461,3406,3349,3290,3229,3166,3102,3035,2967,
  2896,2824,2751,2675,2598,2520,2440,2359,2276,2191,2106,2019,1931,1842,1751,1660,
  1567,1474,1380,1285,1189,1092,995,897,799,700,601,501,401,301,201,101,
  0,-101,-201,-301,-401,-501,-601,-700,-799,-897,-995,-1092,-1189,-1285,-1380,-1474,
  -1567,-1660,-1751,-1842,-1931,-2019,-2106,-2191,-2276,-2359,-2440,-2520,-2598,-2675,-2751,-2824,
  -2896,-2967,-3035,-3102,-3166,-3229,-3290,-3349,-3406,-3461,-3513,-3564,-3612,-3659,-3703,-3745,
  -3784,-3822,-3857,-3889,-3920,-3948,-3973,-3996,-4017,-4036,-4052,-4065,-4076,-4085,-4091,-4095,
  -4096,-4095,-4091,-4085,-4076,-4065,-4052,-4036,-4017,-3996,-3973,-3948,-3920,-3889,-3857,-3822,
  -3784,-3745,-3703,-3659,-3612,-3564,-3513,-3461,-3406,-3349,-3290,-3229,-3166,-3102,-3035,-2967,
  -2896,-2824,-2751,-2675,-2598,-2520,-2440,-2359,-2276,-2191,-2106,-2019,-1931,-1842,-1751,-1660,
  -1567,-1474,-1380,-1285,-1189,-1092,-995,-897,-799,-700,-601,-501,-401,-301,-201,-101,
};


static int g_yaw = 32, g_pitch = 8, g_dirty = 1, g_spin = 0;
static int mk_lx, mk_ly, mk_drag;

/* Active mesh: defaults to the kernel-baked MAKINA-7, but a mesh sent over the
 * network (POST /actor/loadmesh, "MK3D" blob) replaces it at run time. */
#define MK_VMAX 24000           /* upload vertex cap   (baked MK_NV fits) */
#define MK_TMAX 48000           /* upload triangle cap (baked MK_NT fits) */
static short          up_v[MK_VMAX][3];
static unsigned short up_t[MK_TMAX][3];
static unsigned char  up_c[MK_TMAX][3];
static const short          (*g_v)[3] = mk_v;
static const unsigned short (*g_t)[3] = mk_t;
static const unsigned char  (*g_c)[3] = mk_c;
static int g_nv = MK_NV, g_nt = MK_NT;

static int rx[MK_VMAX], ry[MK_VMAX], rz[MK_VMAX], px[MK_VMAX], py[MK_VMAX];
#define NB 256
static int bhead[NB], tnext[MK_TMAX];

/* ---- Skeletal rig ("MKR1") for the articulated walk.  Bone-local verts live
 * in up_v[]; per frame we forward-kinematics them into pose[] using the gait. */
#define MKR_NB 16
typedef struct { int parent, px, py, pz, v0, nv, t0, nt; } mkbone_t;
static mkbone_t rb[MKR_NB];
static int rb_n = 0, rig_loaded = 0, rig_nv = 0, rig_nt = 0;
static short pose[MK_VMAX][3];
static void mkr_pose(int p);

/* offscreen back buffer */
#define MKBW 760
#define MKBH 640
static unsigned int mk_bb[MKBW * MKBH];

/* clipped Bresenham line into the back buffer (wireframe mode) */
static void mk_line(int x0,int y0,int x1,int y1,int bw,int bh,unsigned int col){
    int dx=x1-x0, dy=y1-y0; dx=dx<0?-dx:dx; dy=dy<0?-dy:dy;
    int sx=x0<x1?1:-1, sy=y0<y1?1:-1, err=dx-dy;
    for(;;){
        if(x0>=0&&x0<bw&&y0>=0&&y0<bh) mk_bb[y0*MKBW+x0]=col;
        if(x0==x1&&y0==y1) break;
        int e2=2*err; if(e2>-dy){err-=dy;x0+=sx;} if(e2<dx){err+=dx;y0+=sy;}
    }
}

#define MK_TB 26            /* toolbar height */
#define MK_NBTN 5
static const char *mk_btn[MK_NBTN] = { "Y-", "Y+", "P-", "P+", "SPIN" };

void makina_orbit(int dx, int dy) { g_yaw = (g_yaw - dx) & 255; g_pitch = (g_pitch + dy) & 255; g_dirty = 1; }
int  makina_drag_active(void) { return mk_drag; }
void makina_drag_set(int on, int x, int y) { mk_drag = on; mk_lx = x; mk_ly = y; }
void makina_drag_move(int x, int y) { makina_orbit(x - mk_lx, y - mk_ly); mk_lx = x; mk_ly = y; }

static int isqrt32(int v){ if(v<=0)return 0; int r=0,b=1<<15; while(b>v)b>>=2;
    while(b){ if(v>=r+b){v-=r+b;r=(r>>1)+b;}else r>>=1; b>>=2;} return r; }

static window_t mk_win; static int mk_open = 0;

static void mk_btn_rect(int i, int *bx, int *by, int *bw, int *bh){
    int x = mk_win.x + 6, y = mk_win.y + WM_TITLEBAR_H + 4;
    int k, w;
    for (k = 0; ; k++) { w = (mk_btn[k][2] ? 5 : 3) * FONT_WIDTH + 10;
        if (k == i) { *bx = x; *by = y; *bw = w; *bh = MK_TB - 8; return; }
        x += w + 5; }
}
/* desktop-coord hit test: returns button index or -1 */
int makina_button_hit(int sx, int sy){
    if (!mk_open) return -1;
    int i, bx, by, bw, bh;
    for (i = 0; i < MK_NBTN; i++){ mk_btn_rect(i,&bx,&by,&bw,&bh);
        if (sx>=bx && sx<bx+bw && sy>=by && sy<by+bh) return i; }
    return -1;
}
void makina_button_action(int i){
    if (i==0) makina_orbit( 12,0);
    else if (i==1) makina_orbit(-12,0);
    else if (i==2) makina_orbit(0,-12);
    else if (i==3) makina_orbit(0, 12);
    else if (i==4) { g_spin = !g_spin; g_dirty = 1; }
}
/* 1 if (sx,sy) is inside the 3-D viewport (below the toolbar) */
int makina_viewport_hit(int sx, int sy){
    if (!mk_open) return 0;
    int vx = mk_win.x+4, vy = mk_win.y+WM_TITLEBAR_H+4+MK_TB;
    int vw = mk_win.width-8, vh = mk_win.height-WM_TITLEBAR_H-7-MK_TB;
    return sx>=vx && sx<vx+vw && sy>=vy && sy<vy+vh;
}

/* Shared renderer core.  World pre-transform (for the walk actor): rotate the
 * body about Y by `head`, then translate it to (wx,wy,wz) in model units — so
 * the character can stand at a point on a circular path while facing its
 * heading.  Then the camera (yaw,pitch) views it.  roadR>0 draws the circular
 * ground path as a dotted ellipse.  Then shade + blit below a tb-high toolbar. */
static void mk_render_core(window_t *self,
                           const short (*V)[3], const unsigned short (*T)[3],
                           const unsigned char (*C)[3], int NV, int NT,
                           int yaw, int pitch,
                           int head, int wx, int wy, int wz, int roadR,
                           int xoff, int yoff, int wire, int tb){
    int gx = self->x+4, gy = self->y+WM_TITLEBAR_H+4;
    int cw = self->width-8;
    int vy0 = gy + tb;
    int vh  = self->height-WM_TITLEBAR_H-7-tb;
    int bw = cw, bh = vh; if (bw>MKBW) bw=MKBW; if (bh>MKBH) bh=MKBH;
    /* clear back buffer to content_bg */
    unsigned int bg = self->content_bg; int i,t;
    for (i=0;i<bw*bh;i++) mk_bb[i]=bg;
    int cx = bw/2, cy = bh/2;       /* local centre */
    int cyaw=SINT[(yaw+64)&255], syaw=SINT[yaw&255];
    int cpit=SINT[(pitch+64)&255], spit=SINT[pitch&255];
    int chd =SINT[(head+64)&255],  shd =SINT[head&255];
    /* circular road as a dotted ellipse on the floor (y=0 world plane) */
    if (roadR > 0) {
        int a;
        for (a=0;a<256;a+=2){
            int rwx=(roadR*SINT[(a+64)&255])>>12, rwz=(roadR*SINT[a&255])>>12;
            int x1=(rwx*cyaw - rwz*syaw)>>12, z1=(rwx*syaw + rwz*cyaw)>>12;
            int y2=(- z1*spit)>>12;          /* hy=0 */
            int sxp=cx+xoff+(x1>>2), syp=cy-yoff-(y2>>2), dx,dy;
            for (dy=-1;dy<=1;dy++) for (dx=-1;dx<=1;dx++){
                int xx=sxp+dx, yy=syp+dy;
                if (xx>=0&&xx<bw&&yy>=0&&yy<bh) mk_bb[yy*MKBW+xx]=0xFF3A4250U;
            }
        }
    }
    for (i=0;i<NV;i++){
        int x=V[i][0],y=V[i][1],z=V[i][2];
        /* body heading rotation about Y, then world translation */
        int hx=(x*chd - z*shd)>>12, hz=(x*shd + z*chd)>>12, hy=y;
        hx+=wx; hz+=wz; hy+=wy;
        /* camera yaw about Y, pitch about X */
        int x1=(hx*cyaw - hz*syaw)>>12, z1=(hx*syaw + hz*cyaw)>>12;
        int y2=(hy*cpit - z1*spit)>>12, z2=(hy*spit + z1*cpit)>>12;
        rx[i]=x1; ry[i]=y2; rz[i]=z2;
        px[i]=cx+xoff+(x1>>2); py[i]=cy-yoff-(y2>>2);
    }
    if (wire){                       /* wireframe: front-face edges only (light) */
        for (t=0;t<NT;t++){
            int a=T[t][0],b=T[t][1],c=T[t][2];
            long area=(long)(px[b]-px[a])*(py[c]-py[a]) - (long)(px[c]-px[a])*(py[b]-py[a]);
            if (area<=0) continue;
            /* wireframe lines: bright (brighten the part colour toward white) */
            unsigned int wr=160+((unsigned)C[t][0]*95)/255;
            unsigned int wg=160+((unsigned)C[t][1]*95)/255;
            unsigned int wb=160+((unsigned)C[t][2]*95)/255;
            unsigned int col=0xFF000000u|(wr<<16)|(wg<<8)|wb;
            mk_line(px[a],py[a],px[b],py[b],bw,bh,col);
            mk_line(px[b],py[b],px[c],py[c],bw,bh,col);
            mk_line(px[c],py[c],px[a],py[a],bw,bh,col);
        }
        gv_blit(gx, vy0, bw, bh, mk_bb, MKBW);
        return;
    }
    for (i=0;i<NB;i++) bhead[i]=-1;
    for (t=0;t<NT;t++){
        int a=T[t][0],b=T[t][1],c=T[t][2];
        long area=(long)(px[b]-px[a])*(py[c]-py[a]) - (long)(px[c]-px[a])*(py[b]-py[a]);
        if (area<=0) continue;
        int zc=(rz[a]+rz[b]+rz[c])/3, bi=(zc+1024)>>3; if(bi<0)bi=0; if(bi>=NB)bi=NB-1;
        tnext[t]=bhead[bi]; bhead[bi]=t;
    }
    for (int bi=0;bi<NB;bi++)
    for (t=bhead[bi];t>=0;t=tnext[t]){
        int a=T[t][0],b=T[t][1],c=T[t][2];
        int ux=rx[b]-rx[a],uy=ry[b]-ry[a],uz=rz[b]-rz[a];
        int vx=rx[c]-rx[a],vy=ry[c]-ry[a],vz=rz[c]-rz[a];
        int nz=(ux*vy-uy*vx)>>8, nx=(uy*vz-uz*vy)>>8, ny=(uz*vx-ux*vz)>>8;
        int nl=isqrt32(nx*nx+ny*ny+nz*nz); if(nl<1)nl=1;
        int br=70+(185*(nz>0?nz:-nz))/nl; if(br>255)br=255;
        unsigned int rr=(C[t][0]*br)>>8, gg=(C[t][1]*br)>>8, bb2=(C[t][2]*br)>>8;
        unsigned int col=0xFF000000u|(rr<<16)|(gg<<8)|bb2;
        int x0=px[a],y0=py[a],x1=px[b],y1=py[b],x2=px[c],y2=py[c];
        if(y1<y0){int s=x0;x0=x1;x1=s;s=y0;y0=y1;y1=s;}
        if(y2<y0){int s=x0;x0=x2;x2=s;s=y0;y0=y2;y2=s;}
        if(y2<y1){int s=x1;x1=x2;x2=s;s=y1;y1=y2;y2=s;}
        if(y2==y0) continue;
        for(int yy=y0;yy<=y2;yy++){
            if(yy<0||yy>=bh) continue;
            int xa=x0+(int)((long)(x2-x0)*(yy-y0)/(y2-y0)), xb;
            if(yy<y1 && y1!=y0) xb=x0+(int)((long)(x1-x0)*(yy-y0)/(y1-y0));
            else if(y2!=y1)     xb=x1+(int)((long)(x2-x1)*(yy-y1)/(y2-y1));
            else                xb=x1;
            if(xa>xb){int s=xa;xa=xb;xb=s;}
            if(xa<0)xa=0; if(xb>=bw)xb=bw-1;
            unsigned int *row=mk_bb+yy*MKBW;
            for(int xx=xa;xx<=xb;xx++) row[xx]=col;
        }
    }
    gv_blit(gx, vy0, bw, bh, mk_bb, MKBW);
}

static void mk_render(window_t *self){
    int gx = self->x+4, gy = self->y+WM_TITLEBAR_H+4, cw = self->width-8, i;
    mk_render_core(self, g_v, g_t, g_c, g_nv, g_nt, g_yaw, g_pitch, 0, 0, 0, 0, 0, 0, 0, 0, MK_TB);
    /* toolbar (drawn directly; static, doesn't flicker) */
    fill_rect(gx, gy, cw, MK_TB-2, 0xFF1A2030U);
    int bx,by,bbw,bh2;
    for (i=0;i<MK_NBTN;i++){ mk_btn_rect(i,&bx,&by,&bbw,&bh2);
        unsigned int f = (i==4 && g_spin) ? 0xFF2E8B40U : 0xFF38506EU;
        fill_rect(bx,by,bbw,bh2,f);
        draw_string_at(bx+5,by+(bh2-FONT_HEIGHT)/2,mk_btn[i],0xFFFFFFFFU,f);
    }
}

static void makina_draw(window_t *self, unsigned int frame){
    (void)frame;
    if (g_spin) { g_yaw = (g_yaw+2)&255; g_dirty = 1; }   /* auto-rotate */
    if (!g_force_redraw && !g_dirty) return;
    g_dirty = 0;
    mk_render(self);
}

/* ----- Walk actor: a second window that animates the same character with a
 * procedural walk cycle (stroll across the floor + vertical bob at 2x step
 * frequency + body sway + a slow turn).  Redraws every frame. ----- */
static window_t mkw_win; static int mkw_open = 0;
static int mkw_phase = 0, mkw_walk = 1;     /* mkw_walk: 0 = paused */
static int mkw_wire = 1;                    /* 1 = wireframe, 0 = solid */
static int mkw_steps = 0, mkw_done = 0;     /* 2-step-then-freeze sequence */

#define MKW_R    700          /* circular path radius (model units)        */
#define MKW_CAMY 32           /* camera yaw (fixed; body turns as it walks) */
#define MKW_CAMP 20           /* camera pitch ~28deg down -> path is ellipse*/
static void makina_walk_draw(window_t *self, unsigned int frame){
    (void)frame;
    /* Walk exactly 2 steps then freeze; clear the whole screen at each step so
     * the stride reads as discrete and the final 2-step pose stays on screen. */
    int full_clear = 0;
    if (mkw_walk && !mkw_done){
        int seg0 = mkw_phase >> 7;            /* /128 = one footfall per half-period */
        mkw_phase += 5;
        if ((mkw_phase >> 7) != seg0){        /* a step just completed */
            mkw_steps++;
            full_clear = 1;
            if (mkw_steps >= 2){ mkw_done = 1; mkw_walk = 0; }
        }
    }
    int ph = mkw_phase & 255;                 /* gait phase (legs/arms swing) */
    int a  = (mkw_phase >> 2) & 255;          /* modest forward travel on the ring */
    /* world position on the circle + heading along the tangent (face travel) */
    int wx = (MKW_R * SINT[(a+64)&255]) >> 12;          /* R*cos(a) */
    int wz = (MKW_R * SINT[a&255])      >> 12;          /* R*sin(a) */
    int head = (-a) & 255;                              /* rotation.y = -a   */
    /* choose the mesh: rigged (articulated FK) if a rig was sent, else the
     * single body mesh (whole-body circular stroll). */
    const short          (*V)[3]; const unsigned short (*T)[3];
    const unsigned char  (*C)[3]; int NV, NT, midy, wy;
    if (rig_loaded){
        mkr_pose(ph);
        V=(const short(*)[3])pose; T=(const unsigned short(*)[3])up_t;
        C=(const unsigned char(*)[3])up_c; NV=rig_nv; NT=rig_nt;
        midy = 900;                                     /* rig feet at y=0    */
        wy = 0;                                         /* bob comes from gait */
    } else {
        V=g_v; T=g_t; C=g_c; NV=g_nv; NT=g_nt;
        int step=(a*9)&255, bob=SINT[step]; if(bob<0)bob=-bob;
        midy = 0; wy = (bob*70)>>12;
    }
    /* camera follows the walker: cancel the projected mid-body so it stays put */
    int cyaw=SINT[(MKW_CAMY+64)&255], syaw=SINT[MKW_CAMY&255];
    int cpit=SINT[(MKW_CAMP+64)&255], spit=SINT[MKW_CAMP&255];
    int X1=(wx*cyaw - wz*syaw)>>12, Z1=(wx*syaw + wz*cyaw)>>12;
    int Y2=(midy*cpit - Z1*spit)>>12;
    int xoff = -(X1>>2), yoff = -(Y2>>2);
    if (full_clear){                          /* clear the whole screen at each step */
        extern unsigned int video_screen_width(void), video_screen_height(void);
        fill_rect(0, 0, (int)video_screen_width(), (int)video_screen_height(), 0xFF000000U);
        g_force_redraw = 1;                   /* let the WM repaint other windows */
    }
    mk_render_core(self, V, T, C, NV, NT, MKW_CAMY, MKW_CAMP, head, wx, wy, wz, MKW_R, xoff, yoff, mkw_wire, MK_TB);
    /* mini toolbar: WALK/STOP + WIRE/SOLID toggles */
    int gx = self->x+4, gy = self->y+WM_TITLEBAR_H+4, cw = self->width-8;
    fill_rect(gx, gy, cw, MK_TB-2, 0xFF1A2030U);
    unsigned int f = mkw_walk ? 0xFF2E8B40U : 0xFF38506EU;
    fill_rect(gx+6, gy+4, 6*FONT_WIDTH+10, MK_TB-8, f);
    draw_string_at(gx+11, gy+4+(MK_TB-8-FONT_HEIGHT)/2, mkw_walk?"WALK":"STOP", 0xFFFFFFFFU, f);
    int bx2 = gx+6 + 6*FONT_WIDTH+10 + 6;
    unsigned int f2 = mkw_wire ? 0xFF2E8B40U : 0xFF38506EU;
    fill_rect(bx2, gy+4, 6*FONT_WIDTH+10, MK_TB-8, f2);
    draw_string_at(bx2+11, gy+4+(MK_TB-8-FONT_HEIGHT)/2, mkw_wire?"WIRE":"SOLID", 0xFFFFFFFFU, f2);
}

int makina_walk_open(void){
    if (mkw_open) return 0;
    mkw_win.x=470; mkw_win.y=120; mkw_win.width=520; mkw_win.height=560;
    { const char *t="MAKINA-7 Walk"; int i; for(i=0;i<WM_TITLE_MAX&&t[i];i++)mkw_win.title[i]=t[i]; mkw_win.title[i]=0; }
    mkw_win.chrome_color=0xFF7ED0A0U; mkw_win.title_bg=0xFF182838U;
    mkw_win.title_fg=0xFFFFFFFFU; mkw_win.content_bg=0xFF0B1610U;
    mkw_win.draw_content=makina_walk_draw;
    { extern void wm_add(window_t *); wm_add(&mkw_win); }
    mkw_open=1;
    return 0;
}
/* desktop-coord hit test for the walk window's WALK/STOP + WIRE/SOLID toggles */
int makina_walk_toggle_hit(int sx, int sy){
    if (!mkw_open) return 0;
    int gx = mkw_win.x+4+6, gy = mkw_win.y+WM_TITLEBAR_H+4+4;
    int w = 6*FONT_WIDTH+10, h = MK_TB-8;
    if (sx>=gx && sx<gx+w && sy>=gy && sy<gy+h) {
        if (mkw_done) { mkw_done=0; mkw_steps=0; mkw_phase=0; mkw_walk=1; }  /* replay 2 steps */
        else mkw_walk = !mkw_walk;
        return 1;
    }
    int bx2 = gx + w + 6;
    if (sx>=bx2 && sx<bx2+w && sy>=gy && sy<gy+h) { mkw_wire = !mkw_wire; return 1; }
    return 0;
}

void *makina_win_ptr(void){ return &mk_win; }

/* On-screen download progress bar, drawn directly to the framebuffer while a
 * mesh streams in over the network (called per chunk from webactor).  Centred
 * where the MAKINA window opens, so the window redraw after load covers it. */
void makina_progress(int recv, int total){
    if (total <= 0) return;
    int pct = (int)((long)recv * 100 / total);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    int bx = 310, by = 350, bw = 320, bh = 44;
    fill_rect(bx-2, by-2, bw+4, bh+4, 0xFF0A0E18U);
    fill_rect(bx, by, bw, bh, 0xFF182233U);
    draw_string_at(bx+8, by+5, "Receiving mesh", 0xFFFFFFFFU, 0xFF182233U);
    /* percent text, right-aligned-ish */
    char ps[6]; int m=0;
    if (pct >= 100) { ps[m++]='1'; ps[m++]='0'; ps[m++]='0'; }
    else { if (pct >= 10) ps[m++]='0'+pct/10; ps[m++]='0'+pct%10; }
    ps[m++]='%'; ps[m]=0;
    draw_string_at(bx+bw-8-m*FONT_WIDTH, by+5, ps, 0xFF9FE0FFU, 0xFF182233U);
    /* track + fill */
    fill_rect(bx+8, by+24, bw-16, 12, 0xFF26384EU);
    int innw = (bw-16) * pct / 100;
    if (innw > 0) fill_rect(bx+8, by+24, innw, 12, 0xFF3FA0E0U);
}

/* Receive a mesh sent over the network and make it the active model.
 * Blob format (little-endian, packed):
 *   char  magic[4] = "MK3D"
 *   u32   nv               number of vertices
 *   u32   nt               number of triangles
 *   i16   v[nv][3]         vertex positions (model scaled so height ~2000)
 *   u16   t[nt][3]         triangle vertex indices
 *   u8    c[nt][3]         per-triangle RGB
 * Returns the triangle count on success, -1 on a malformed/oversized blob. */
static unsigned rd_u32(const unsigned char *p){
    return (unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);
}
int makina_window_open(void);
int makina_walk_open(void);
int makina_load_mesh(const unsigned char *d, int len){
    int i;
    if (len < 12 || d[0]!='M'||d[1]!='K'||d[2]!='3'||d[3]!='D') return -1;
    int nv = (int)rd_u32(d+4), nt = (int)rd_u32(d+8);
    if (nv<=0 || nt<=0 || nv>MK_VMAX || nt>MK_TMAX) return -1;
    int need = 12 + nv*6 + nt*6 + nt*3;
    if (len < need) return -1;
    const unsigned char *pv = d+12;
    const unsigned char *pt = pv + nv*6;
    const unsigned char *pc = pt + nt*6;
    for (i=0;i<nv;i++){
        const unsigned char *q = pv + i*6;
        up_v[i][0]=(short)(q[0]|(q[1]<<8));
        up_v[i][1]=(short)(q[2]|(q[3]<<8));
        up_v[i][2]=(short)(q[4]|(q[5]<<8));
    }
    for (i=0;i<nt;i++){
        const unsigned char *q = pt + i*6;
        up_t[i][0]=(unsigned short)(q[0]|(q[1]<<8));
        up_t[i][1]=(unsigned short)(q[2]|(q[3]<<8));
        up_t[i][2]=(unsigned short)(q[4]|(q[5]<<8));
    }
    for (i=0;i<nt;i++){
        const unsigned char *q = pc + i*3;
        up_c[i][0]=q[0]; up_c[i][1]=q[1]; up_c[i][2]=q[2];
    }
    g_v=(const short (*)[3])up_v; g_t=(const unsigned short (*)[3])up_t;
    g_c=(const unsigned char (*)[3])up_c; g_nv=nv; g_nt=nt;
    makina_window_open();          /* interactive viewer only (no walk window) */
    g_spin=0;                      /* default: no auto-spin (static until SPIN) */
    g_dirty=1; g_force_redraw=1;
    return nt;
}

static unsigned rd_u32s(const unsigned char *p){
    return (unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);
}
/* Receive a skeletal rig ("MKR1"): bone table + bone-local verts + tris + cols.
 * Layout (LE): magic, u32 nbones,nv,nt; per bone {i32 parent; i16 px,py,pz;
 * u32 v0,nv,t0,nt}; then nv*(i16x3) verts, nt*(u16x3) idx, nt*(u8x3) cols. */
int makina_load_rig(const unsigned char *d, int len){
    int i;
    if (len < 16 || d[0]!='M'||d[1]!='K'||d[2]!='R'||d[3]!='1') return -1;
    int nb=(int)rd_u32s(d+4), nv=(int)rd_u32s(d+8), nt=(int)rd_u32s(d+12);
    if (nb<1||nb>MKR_NB||nv<1||nv>MK_VMAX||nt<1||nt>MK_TMAX) return -1;
    int need = 16 + nb*26 + nv*6 + nt*6 + nt*3;
    if (len < need) return -1;
    const unsigned char *p=d+16;
    for (i=0;i<nb;i++){
        const unsigned char *q=p+i*26;
        rb[i].parent=(int)rd_u32s(q);
        rb[i].px=(short)(q[4]|(q[5]<<8)); rb[i].py=(short)(q[6]|(q[7]<<8)); rb[i].pz=(short)(q[8]|(q[9]<<8));
        rb[i].v0=(int)rd_u32s(q+10); rb[i].nv=(int)rd_u32s(q+14);
        rb[i].t0=(int)rd_u32s(q+18); rb[i].nt=(int)rd_u32s(q+22);
    }
    const unsigned char *pv=p+nb*26, *pt=pv+nv*6, *pc=pt+nt*6;
    for (i=0;i<nv;i++){ const unsigned char *q=pv+i*6;
        up_v[i][0]=(short)(q[0]|(q[1]<<8)); up_v[i][1]=(short)(q[2]|(q[3]<<8)); up_v[i][2]=(short)(q[4]|(q[5]<<8)); }
    for (i=0;i<nt;i++){ const unsigned char *q=pt+i*6;
        up_t[i][0]=(unsigned short)(q[0]|(q[1]<<8)); up_t[i][1]=(unsigned short)(q[2]|(q[3]<<8)); up_t[i][2]=(unsigned short)(q[4]|(q[5]<<8)); }
    for (i=0;i<nt;i++){ const unsigned char *q=pc+i*3; up_c[i][0]=q[0]; up_c[i][1]=q[1]; up_c[i][2]=q[2]; }
    rb_n=nb; rig_nv=nv; rig_nt=nt; rig_loaded=1;
    makina_walk_open();            /* the rigged character walks here */
    g_force_redraw=1;
    return nt;
}

/* 3x3 (fixed-point x4096) helpers for forward kinematics */
static void mat3(const int *A, const int *B, int *O){      /* O = A*B */
    int r,c,k; for(r=0;r<3;r++)for(c=0;c<3;c++){ long s=0;
        for(k=0;k<3;k++) s+=(long)A[r*3+k]*B[k*3+c]; O[r*3+c]=(int)(s>>12); } }
static void mvec3(const int *A, const int *v, int *o){     /* o = A*v */
    int r; for(r=0;r<3;r++){ long s=(long)A[r*3]*v[0]+(long)A[r*3+1]*v[1]+(long)A[r*3+2]*v[2]; o[r]=(int)(s>>12); } }

/* gait angle (SINT units) about X for bone index, at phase p (0..255) */
static int bone_gait(int b, int p){
    int pL=p, pR=(p+128)&255;
    int cL=SINT[(pL+64)&255], sL=SINT[pL&255];
    int cR=SINT[(pR+64)&255], sR=SINT[pR&255];
    switch(b){
      case 1: return -((22*sL)>>12);                          /* thighL hip   */
      case 2: return  (43*(cL>0?cL:0))>>12;                   /* shinL  knee  */
      case 3: return ((13*sL)>>12)-2;                         /* footL  ankle */
      case 4: return -((22*sR)>>12);                          /* thighR       */
      case 5: return  (43*(cR>0?cR:0))>>12;                   /* shinR        */
      case 6: return ((13*sR)>>12)-2;                         /* footR        */
      case 7: return  (22*sR)>>12;                            /* uarmL (opp)  */
      case 8: return -12-((22*(cR>0?cR:0))>>12);              /* farmL elbow  */
      case 9: return  (22*sL)>>12;                            /* uarmR        */
      case 10:return -12-((22*(cL>0?cL:0))>>12);              /* farmR elbow  */
      default:return 0;                                       /* body         */
    }
}
/* forward-kinematics the rig into pose[] for gait phase p */
static void mkr_pose(int p){
    static int wr[MKR_NB][9], wt[MKR_NB][3];
    int b,i;
    for (b=0;b<rb_n;b++){
        int th=bone_gait(b,p);
        int c=SINT[(th+64)&255], s=SINT[th&255];
        int Rl[9]={4096,0,0, 0,c,-s, 0,s,c};        /* rotate about X */
        if (rb[b].parent<0){
            for(i=0;i<9;i++) wr[b][i]=Rl[i];
            wt[b][0]=wt[b][1]=wt[b][2]=0;
        } else {
            int pp=rb[b].parent;
            mat3(wr[pp],Rl,wr[b]);
            int lp[3]={rb[b].px,rb[b].py,rb[b].pz}, rp[3];
            mvec3(wr[pp],lp,rp);
            wt[b][0]=rp[0]+wt[pp][0]; wt[b][1]=rp[1]+wt[pp][1]; wt[b][2]=rp[2]+wt[pp][2];
        }
        int v0=rb[b].v0, ve=v0+rb[b].nv;
        for (i=v0;i<ve;i++){
            int v[3]={up_v[i][0],up_v[i][1],up_v[i][2]}, o[3];
            mvec3(wr[b],v,o);
            pose[i][0]=(short)(o[0]+wt[b][0]);
            pose[i][1]=(short)(o[1]+wt[b][1]);
            pose[i][2]=(short)(o[2]+wt[b][2]);
        }
    }
}

int makina_window_open(void){
    if (mk_open) return 0;
    mk_win.x=120; mk_win.y=60; mk_win.width=700; mk_win.height=620;
    { const char *t="MAKINA-7 3D"; int i; for(i=0;i<WM_TITLE_MAX&&t[i];i++)mk_win.title[i]=t[i]; mk_win.title[i]=0; }
    mk_win.chrome_color=0xFFB68AEEU; mk_win.title_bg=0xFF202838U;
    mk_win.title_fg=0xFFFFFFFFU; mk_win.content_bg=0xFF0B0E16U;
    mk_win.draw_content=makina_draw;
    { extern void wm_add(window_t *); wm_add(&mk_win); }
    mk_open=1; g_dirty=1;
    return 0;
}
#endif
