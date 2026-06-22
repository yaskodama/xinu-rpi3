#!/usr/bin/env python3
"""avm_view.py — render an AIPL actor binary (.avm) on the Mac, exactly the way
the Xinu kernel's VM would draw it, so you can preview an actor *before* sending
it to the Pi.

It is a faithful soft-VM: it parses the AVM1 module, spawns the first class and
kicks it with "tick" (just like /actor/loadvm on the kernel), then executes the
bytecode — interpreting the gfx opcodes CLS(0x46) / LINE(0x45) / TRI(0x47) — and
emits the drawn frame as a standalone SVG/HTML that it opens in the browser.

  python3 tools/avm_view.py [file.avm]      # default actors/MakinaXinuHi.avm

Opcodes + module layout mirror apps/abcl_program.c (abcl_vm_load / dispatch).
"""
import struct, sys, subprocess, os

PATH = sys.argv[1] if len(sys.argv) > 1 else "actors/MakinaXinuHi.avm"

# 16-colour palette — apps/gwm.c bas_palette() (ARGB -> #RRGGBB)
PAL = [0x000000,0x3060FF,0x30D040,0x30D0D0, 0xE03030,0xE040E0,0xE0E040,0xF0F0F0,
       0x808080,0x80A0FF,0x80FF80,0x80FFFF, 0xFF8080,0xFF80FF,0xFFFF80,0xFFFFFF]
def hexcol(c):
    if c & 0x1000000: return f"#{c & 0xFFFFFF:06x}"   # bit24 = true 0xRRGGBB colour
    v = PAL[c & 15]; return f"#{v:06x}"

# ---- parse the AVM1 module (matches abcl_vm_load) ----------------------------
d = open(PATH, "rb").read()
if d[:4] != b"AVM1": sys.exit(f"not an AVM1 module: {PATH}")
p = 4
ns = struct.unpack_from("<H", d, p)[0]; p += 2
strs = []
for _ in range(ns):
    l = struct.unpack_from("<H", d, p)[0]; p += 2
    strs.append(d[p:p+l].decode("latin1")); p += l
nc = struct.unpack_from("<H", d, p)[0]; p += 2
classes = []
for _ in range(nc):
    name, nf, nm = struct.unpack_from("<HHH", d, p); p += 6
    methods = {}
    for _ in range(nm):
        mname, npar = struct.unpack_from("<HB", d, p); p += 3
        clen = struct.unpack_from("<H", d, p)[0]; p += 2
        methods[strs[mname]] = (d[p:p+clen]); p += clen
    classes.append({"name": strs[name], "nfields": nf, "methods": methods})

print(f"[avm] {PATH}: {len(d)} B, {nc} classes: " +
      ", ".join(f"{c['name']}({'/'.join(c['methods'])})" for c in classes))

# ---- soft-VM -----------------------------------------------------------------
MAX_FIELDS = 16
objects = []                      # each actor: {"class": ci, "fields": [int]*MAX_FIELDS}
def spawn(ci):
    objects.append({"class": ci, "fields": [0]*MAX_FIELDS})
    return len(objects) - 1

draw = []                         # current frame: ('line'|'tri', coords..., col)
def u16(b, i): return b[i] | (b[i+1] << 8)
def i32(b, i): return struct.unpack_from("<i", b, i)[0]

MSGCAP = 30000                      # bounded so an animating actor (re-sends tick) stops
queue = []

def run(self_id, sender, method, args):
    ci = objects[self_id]["class"]; cl = classes[ci]
    code = cl["methods"].get(method)
    if code is None: return
    fields = objects[self_id]["fields"]
    stk = []; pc = 0; clen = len(code); guard = 0
    def pop(): return stk.pop() if stk else 0
    while pc < clen:
        guard += 1
        if guard > 8_000_000: break
        op = code[pc]; pc += 1
        if   op == 0x01: stk.append(i32(code, pc)); pc += 4
        elif op == 0x02: f = code[pc]; pc += 1; stk.append(fields[f] if f < MAX_FIELDS else 0)
        elif op == 0x03: f = code[pc]; pc += 1; v = pop();  fields.__setitem__(f, v) if f < MAX_FIELDS else None
        elif op == 0x04: a = code[pc]; pc += 1; stk.append(args[a] if a < len(args) else 0)
        elif op == 0x05: stk.append(self_id)
        elif op == 0x06: stk.append(sender)
        elif op == 0x07: pop()
        elif op == 0x08: stk.append(stk[-1] if stk else 0)
        elif op == 0x10: b=pop(); a=pop(); stk.append(a+b)
        elif op == 0x11: b=pop(); a=pop(); stk.append(a-b)
        elif op == 0x12: b=pop(); a=pop(); stk.append(a*b)
        elif op == 0x13: b=pop(); a=pop(); stk.append(int(a/b) if b else 0)
        elif op == 0x14: b=pop(); a=pop(); stk.append(a%b if b else 0)
        elif op == 0x20: b=pop(); a=pop(); stk.append(1 if a<b else 0)
        elif op == 0x21: b=pop(); a=pop(); stk.append(1 if a<=b else 0)
        elif op == 0x22: b=pop(); a=pop(); stk.append(1 if a>b else 0)
        elif op == 0x23: b=pop(); a=pop(); stk.append(1 if a>=b else 0)
        elif op == 0x24: b=pop(); a=pop(); stk.append(1 if a==b else 0)
        elif op == 0x25: b=pop(); a=pop(); stk.append(1 if a!=b else 0)
        elif op == 0x30: pc = u16(code, pc)
        elif op == 0x31: t = u16(code, pc); pc += 2; pc = t if pop() == 0 else pc
        elif op == 0x40:
            mn = u16(code, pc); pc += 2; na = code[pc]; pc += 1
            va = [pop() for _ in range(na)][::-1]; recv = pop()
            queue.append((self_id, recv, strs[mn], va))
        elif op == 0x41: ci2 = u16(code, pc); pc += 2; stk.append(spawn(ci2))
        elif op == 0x42: pop()
        elif op == 0x43: break
        elif op == 0x44:
            pc += 2; na = code[pc]; pc += 1
            for _ in range(na): pop()
        elif op == 0x45:
            col=pop(); y2=pop(); x2=pop(); y1=pop(); x1=pop()
            draw.append(("line", x1, y1, x2, y2, col))
        elif op == 0x46: draw.clear()
        elif op == 0x47:
            col=pop(); y3=pop(); x3=pop(); y2=pop(); x2=pop(); y1=pop(); x1=pop()
            draw.append(("tri", x1, y1, x2, y2, x3, y3, col))
        else: break

# spawn first class + kick "tick", then drain the message queue (bounded)
queue.append((-1, spawn(0), "tick", []))
msgs = 0
while queue and msgs < MSGCAP:
    sender, recv, method, args = queue.pop(0); msgs += 1
    if 0 <= recv < len(objects): run(recv, sender, method, args)

print(f"[avm] executed {msgs} messages, {len(objects)} actors, {len(draw)} draw ops in final frame")
if not draw: sys.exit("[avm] nothing was drawn (no CLS/LINE/TRI executed)")

# ---- bounding box ------------------------------------------------------------
xs, ys = [], []
for o in draw:
    if o[0] == "line": _, x1,y1,x2,y2,_ = o; xs += [x1,x2]; ys += [y1,y2]
    else:              _, x1,y1,x2,y2,x3,y3,_ = o; xs += [x1,x2,x3]; ys += [y1,y2,y3]
minx,maxx,miny,maxy = min(xs),max(xs),min(ys),max(ys)
M = 20
W = (maxx-minx) + 2*M; H = (maxy-miny) + 2*M
def tx(x): return x - minx + M
def ty(y): return y - miny + M

# ---- colour ------------------------------------------------------------------
# The MAKINA wireframe is monochrome (every edge uses palette index 7 = white).
# When the whole frame is a single colour, render it in *full colour* by mapping
# each primitive's height to a hue (a vivid top-to-bottom spectrum).  When the
# actor actually uses several palette colours (e.g. coloured axes), keep them.
def hsv(h, s, v):                       # h in [0,360), s,v in [0,1] -> #rrggbb
    import math
    i = int(h/60) % 6; f = h/60 - int(h/60)
    p = v*(1-s); q = v*(1-s*f); t = v*(1-s*(1-f))
    r,g,b = [(v,t,p),(q,v,p),(p,v,t),(p,q,v),(t,p,v),(v,p,q)][i]
    return f"#{int(r*255):02x}{int(g*255):02x}{int(b*255):02x}"
used = set(o[-1] & 15 for o in draw)
RAINBOW = (len(used) <= 1) and not any(o[-1] & 0x1000000 for o in draw)
yspan = (maxy - miny) or 1
def hue_for(y): return 330.0 * (1.0 - (y - miny) / yspan)   # top=red .. bottom=violet
def col_line(x1,y1,x2,y2,c):
    return hsv(hue_for((y1+y2)/2), 0.95, 1.0) if RAINBOW else hexcol(c)
def col_tri(y1,y2,y3,c):
    return hsv(hue_for((y1+y2+y3)/3), 0.85, 1.0) if RAINBOW else hexcol(c)

# ---- SVG (vector -> scale up freely for resolution) --------------------------
SCALE = 3                                # on-screen pixel multiplier
SW = 0.7                                 # stroke width in viewBox units
svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W*SCALE}" height="{H*SCALE}" '
       f'viewBox="0 0 {W} {H}" shape-rendering="geometricPrecision">',
       f'<rect width="{W}" height="{H}" fill="#000000"/>',
       '<g stroke-linecap="round">']
ntri = nline = 0
for o in draw:
    if o[0] == "tri":
        _, x1,y1,x2,y2,x3,y3,col = o; ntri += 1
        svg.append(f'<polygon points="{tx(x1)},{ty(y1)} {tx(x2)},{ty(y2)} '
                   f'{tx(x3)},{ty(y3)}" fill="{col_tri(y1,y2,y3,col)}"/>')
    else:
        _, x1,y1,x2,y2,col = o; nline += 1
        svg.append(f'<line x1="{tx(x1)}" y1="{ty(y1)}" x2="{tx(x2)}" y2="{ty(y2)}" '
                   f'stroke="{col_line(x1,y1,x2,y2,col)}" stroke-width="{SW}"/>')
svg.append('</g></svg>')

name = os.path.splitext(os.path.basename(PATH))[0]

# ---- locate the AIPL (.abcl) source for this actor ---------------------------
# Accept an explicit path as the 2nd arg, else search the usual spots by name.
import html
src_text, src_path = None, None
if len(sys.argv) > 2:
    cands = [sys.argv[2]]
else:
    avmdir = os.path.dirname(os.path.abspath(PATH))
    cands = [
        os.path.join(avmdir, name + ".abcl"),
        os.path.join(avmdir, "..", "abclc", name + ".abcl"),
        os.path.join(avmdir, "..", "..", "abclc", name + ".abcl"),
        os.path.expanduser(f"~/ocaml-app/abclcp-project/abclc/{name}.abcl"),
    ]
for c in cands:
    if c and os.path.isfile(c):
        src_path = os.path.abspath(c)
        src_text = open(src_path, encoding="utf-8", errors="replace").read()
        break

if src_text is not None:
    nsrc = src_text.count("\n") + 1
    src_panel = (
        f'<div class=hd>AIPL source &mdash; {html.escape(os.path.basename(src_path))} '
        f'({nsrc} lines, {len(src_text)} B)</div>'
        f'<pre id=src>{html.escape(src_text)}</pre>')
    print(f"[avm] AIPL source: {src_path} ({nsrc} lines)")
else:
    src_panel = ('<div class=hd>AIPL source</div>'
                 f'<pre id=src>(no .abcl source found for "{name}")</pre>')
    print(f"[avm] no .abcl source found for {name}")

out = f"/tmp/{name}_preview.html"
open(out, "w").write(
    "<!doctype html><html><head><meta charset=utf-8>"
    f"<title>{name} preview</title><style>"
    "*{box-sizing:border-box}"
    "body{margin:0;background:#111;color:#ccc;font:13px/1.3 monospace}"
    ".bar{padding:8px 12px;background:#0a0a0a;border-bottom:1px solid #333}"
    ".wrap{display:flex;height:calc(100vh - 38px)}"
    ".col{overflow:auto;height:100%}"
    ".render{flex:0 0 auto;padding:12px;background:#000;text-align:center}"
    ".source{flex:1 1 auto;border-left:1px solid #333}"
    ".hd{position:sticky;top:0;padding:6px 12px;background:#181818;"
    "border-bottom:1px solid #333;color:#9cf}"
    "pre{margin:0;padding:8px 12px;white-space:pre;color:#cfe8ff}"
    "svg{background:#000}</style></head><body>"
    f"<div class=bar>{name}.avm &mdash; {ntri} triangles, {nline} lines &nbsp;"
    f"({'full-colour (height spectrum)' if RAINBOW else 'palette colour'}, "
    f"{SCALE}&times; = {W*SCALE}&times;{H*SCALE}px, soft-VM render)</div>"
    "<div class=wrap>"
    f"<div class='col render'>{''.join(svg)}</div>"
    f"<div class='col source'>{src_panel}</div>"
    "</div></body></html>")
print(f"[avm] wrote {out}  ({ntri} triangles, {nline} lines, {W}x{H})")
subprocess.run(["open", out])
