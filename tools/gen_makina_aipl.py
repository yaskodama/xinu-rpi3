#!/usr/bin/env python3
# gen_makina_aipl.py — generate an AIPL (.abcl) program that draws a Blender-like
# line editor with the MAKINA-7 model turntable-rotating, by baking N rotation
# frames of the (decimated) wireframe into inline line() calls (the VM has no
# sin/cos and no arrays, so rotation is pre-unrolled).
import sys, math, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb_to_wire as G

GLB = sys.argv[1]
OUT = sys.argv[2]
NFRAMES = int(sys.argv[3]) if len(sys.argv) > 3 else 16
TARGET_E = int(sys.argv[4]) if len(sys.argv) > 4 else 120

# ---- load + decimate using glb_to_wire helpers ----
g, bin_ = G.load_glb(GLB)
tris = []
roots = g["scenes"][g.get("scene", 0)]["nodes"]
def walk(ni, parent):
    node = g["nodes"][ni]
    M = G.mat_mul(parent, G.trs_matrix(node))
    if "mesh" in node:
        for prim in g["meshes"][node["mesh"]]["primitives"]:
            if "POSITION" not in prim.get("attributes", {}): continue
            pos = G.read_accessor(g, bin_, prim["attributes"]["POSITION"])
            wp = [G.xform(M, p) for p in pos]
            idx = [i[0] for i in G.read_accessor(g, bin_, prim["indices"])] if "indices" in prim else list(range(len(wp)))
            for t in range(0, len(idx) - 2, 3):
                tris.append((wp[idx[t]], wp[idx[t+1]], wp[idx[t+2]]))
    for ch in node.get("children", []): walk(ch, M)
for r in roots: walk(r, G.mat_ident())

xs = [p[0] for t in tris for p in t]; ys = [p[1] for t in tris for p in t]; zs = [p[2] for t in tris for p in t]
cx0 = (min(xs)+max(xs))/2; cy0 = (min(ys)+max(ys))/2; cz0 = (min(zs)+max(zs))/2
H = max(ys)-min(ys); SCALE = 230.0/H
def norm(p): return ((p[0]-cx0)*SCALE, (p[1]-cy0)*SCALE, (p[2]-cz0)*SCALE)

def build(grid):
    vmap={}; verts=[]; edges=set()
    def vid(p):
        k=(round(p[0]/grid),round(p[1]/grid),round(p[2]/grid))
        if k not in vmap: vmap[k]=len(verts); verts.append((k[0]*grid,k[1]*grid,k[2]*grid))
        return vmap[k]
    for (a,b,c) in tris:
        ia,ib,ic=vid(norm(a)),vid(norm(b)),vid(norm(c))
        for u,v in ((ia,ib),(ib,ic),(ic,ia)):
            if u!=v: edges.add((min(u,v),max(u,v)))
    return verts, sorted(edges)
grid=3.0
for _ in range(40):
    verts,edges=build(grid)
    if len(edges)<=TARGET_E: break
    grid*=1.09
sys.stderr.write("verts=%d edges=%d frames=%d grid=%.2f\n"%(len(verts),len(edges),NFRAMES,grid))

# ---- model centred in the 480x400 line buffer (the HTML editor provides the
#      Blender chrome; AIPL draws only the model into the 3D viewport canvas) ----
VCX, VCY = 240, 205     # model centre on screen
# Blender-style 3D viewport scene that turntables WITH the model (the camera
# orbits the world): floor grid, world axes (X red, depth green, up blue), and
# the 3D cursor at the origin.  Colour codes are mapped to CSS in the editor.
FLOOR = -118
WORLD = []
for k in range(-5,6):                                   # floor grid
    WORLD.append(((-150,FLOOR,k*30),(150,FLOOR,k*30),1))
    WORLD.append(((k*30,FLOOR,-150),(k*30,FLOOR,150),1))
WORLD.append(((-165,0,0),(165,0,0),2))                  # X axis (red)
WORLD.append(((0,0,-165),(0,0,165),4))                  # depth axis (green)
WORLD.append(((0,FLOOR,0),(0,150,0),3))                 # up axis (blue)
WORLD.append(((-9,0,0),(9,0,0),5))                      # 3D cursor crosshair
WORLD.append(((0,-9,0),(0,9,0),5))
WORLD.append(((0,0,-9),(0,0,9),5))

def projpt(x,y,z,c,s):
    rx = x*c - z*s
    return (int(round(VCX+rx)), int(round(VCY - y)))

MODELC = 7
def emit(f):
    th = 2*math.pi*f/NFRAMES
    c,s = math.cos(th), math.sin(th)
    out=[]
    for (p1,p2,col) in WORLD:                           # world (behind model)
        a=projpt(p1[0],p1[1],p1[2],c,s); b=projpt(p2[0],p2[1],p2[2],c,s)
        out.append("      line(%d,%d,%d,%d,%d);"%(a[0],a[1],b[0],b[1],col))
    pts=[projpt(x,y,z,c,s) for (x,y,z) in verts]
    for (u,v) in edges:
        x1,y1=pts[u]; x2,y2=pts[v]
        out.append("      line(%d,%d,%d,%d,%d);"%(x1,y1,x2,y2,MODELC))
    return "\n".join(out)

# ---- editor chrome (line-art panels), drawn every frame ----
def rect(x,y,w,h,c): return "    line(%d,%d,%d,%d,%d); line(%d,%d,%d,%d,%d); line(%d,%d,%d,%d,%d); line(%d,%d,%d,%d,%d);"%(
    x,y,x+w,y,c, x+w,y,x+w,y+h,c, x+w,y+h,x,y+h,c, x,y+h,x,y,c)

CHROME=[]
CHROME.append("    // --- editor chrome (Blender-like, line art) ---")
CHROME.append(rect(2,2,474,354,8))                 # outer
CHROME.append(rect(2,2,474,14,8))                  # top menu/tab bar
CHROME.append(rect(60,3,70,12,11))                 # highlighted 'Animation' tab
CHROME.append(rect(4,18,322,300,8))                # 3D viewport
CHROME.append(rect(330,18,144,180,8))              # outliner panel
CHROME.append(rect(330,200,144,118,8))             # properties panel
CHROME.append(rect(4,320,470,34,8))                # timeline
# outliner item rows (short ticks = hierarchy)
for i in range(7):
    CHROME.append("    line(%d,%d,%d,%d,8);"%(338, 26+i*12, 338+90, 26+i*12))
# properties rows
for i in range(6):
    CHROME.append("    line(%d,%d,%d,%d,8);"%(338, 210+i*14, 338+120, 210+i*14))
# viewport ground grid
for i in range(5):
    yy=300-i*18
    CHROME.append("    line(%d,%d,%d,%d,8);"%(30+i*4, yy, 300-i*4, yy))
for i in range(-3,4):
    CHROME.append("    line(%d,%d,%d,%d,8);"%(165+i*30, 300, 165+i*10, 200))
# axis gizmo
CHROME.append("    line(24,300,40,300,4); line(24,300,24,284,2); line(24,300,16,308,1);")
# timeline ruler ticks
for i in range(0,11):
    x=14+i*44
    CHROME.append("    line(%d,%d,%d,%d,8);"%(x,322,x,328))
CHROME = "\n".join(CHROME)

with open(OUT,"w") as fo:
    fo.write("// MAKINA-7 in a Blender-like line editor, implemented in AIPL.\n")
    fo.write("// The VM has no sin/cos/arrays, so %d turntable frames of the\n"%NFRAMES)
    fo.write("// decimated wireframe (%d edges) are pre-baked as inline line()s.\n"%len(edges))
    fo.write("// Each rotation frame lives in its own method (d0..d%d) so no single\n"%(NFRAMES-1))
    fo.write("// method's bytecode exceeds the .avm 64KB codeLen limit.\n")
    fo.write("// Generated by tools/gen_makina_aipl.py from MAKINA-7.glb.\n\n")
    fo.write("class Main {\n  var r = 0;\n  method tick() { r = new Editor(); send r.run(); }\n}\n\n")
    fo.write("class Editor {\n  var f = 0; var ph = 0;\n")
    fo.write("  method run() { f = 0; ph = 0; send self.frame(); }\n")
    # frame(): clear + chrome + playhead + dispatch this frame's model method
    fo.write("  method frame() {\n")
    fo.write("    cls();\n")
    for k in range(NFRAMES):
        fo.write("    if (f == %d) { send self.d%d(); }\n"%(k,k))
    fo.write("    send self.tock();\n  }\n")
    # tock(): hold the frame, then advance + loop
    fo.write("  method tock() {\n")
    fo.write("    wait(150);\n")
    fo.write("    f = f + 1; if (f >= %d) { f = 0; }\n"%NFRAMES)
    fo.write("    send self.frame();\n  }\n")
    # one method per rotation frame (model wireframe)
    for k in range(NFRAMES):
        fo.write("  method d%d() {\n%s\n  }\n"%(k, emit(k)))
    fo.write("}\n")
print("wrote", OUT)
