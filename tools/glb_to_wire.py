#!/usr/bin/env python3
# glb_to_wire.py — extract a decimated wireframe (verts + edges) from a binary
# glTF (.glb) and emit a C header for the Xinu animation window.  Pure stdlib.
import sys, json, struct, math

def load_glb(path):
    d = open(path, "rb").read()
    magic, ver, length = struct.unpack_from("<4sII", d, 0)
    assert magic == b"glTF", "not a glb"
    off = 12
    js = None; bin_ = b""
    while off < length:
        clen, ctype = struct.unpack_from("<II", d, off); off += 8
        chunk = d[off:off+clen]; off += clen
        if ctype == 0x4E4F534A: js = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942: bin_ = chunk
    return js, bin_

CT = {5120:("b",1),5121:("B",1),5122:("h",2),5123:("H",2),5125:("I",4),5126:("f",4)}
NCOMP = {"SCALAR":1,"VEC2":2,"VEC3":3,"VEC4":4,"MAT4":16}

def read_accessor(g, bin_, idx):
    acc = g["accessors"][idx]
    bv = g["bufferViews"][acc["bufferView"]]
    base = bv.get("byteOffset",0) + acc.get("byteOffset",0)
    fmt, size = CT[acc["componentType"]]; n = NCOMP[acc["type"]]
    stride = bv.get("byteStride") or size*n
    out = []
    for i in range(acc["count"]):
        p = base + i*stride
        out.append(struct.unpack_from("<"+fmt*n, bin_, p))
    return out

# ---- matrix helpers (column-major like glTF) ----
def mat_ident(): return [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]
def mat_mul(a,b):
    r=[0.0]*16
    for c in range(4):
        for rr in range(4):
            s=0.0
            for k in range(4): s+=a[k*4+rr]*b[c*4+k]
            r[c*4+rr]=s
    return r
def trs_matrix(node):
    if "matrix" in node: return list(node["matrix"])
    t = node.get("translation",[0,0,0])
    q = node.get("rotation",[0,0,0,1])
    s = node.get("scale",[1,1,1])
    x,y,z,w = q
    # rotation matrix from quaternion (column-major)
    xx,yy,zz=x*x,y*y,z*z; xy,xz,yz=x*y,x*z,y*z; wx,wy,wz=w*x,w*y,w*z
    R=[1-2*(yy+zz), 2*(xy+wz),   2*(xz-wy),   0,
       2*(xy-wz),   1-2*(xx+zz), 2*(yz+wx),   0,
       2*(xz+wy),   2*(yz-wx),   1-2*(xx+yy), 0,
       0,0,0,1]
    S=[s[0],0,0,0, 0,s[1],0,0, 0,0,s[2],0, 0,0,0,1]
    M=mat_mul(R,S)
    M[12],M[13],M[14]=t[0],t[1],t[2]
    return M
def xform(m, v):
    x,y,z=v
    return (m[0]*x+m[4]*y+m[8]*z+m[12],
            m[1]*x+m[5]*y+m[9]*z+m[13],
            m[2]*x+m[6]*y+m[10]*z+m[14])

def main():
    path = sys.argv[1]; out = sys.argv[2]
    target_edges = int(sys.argv[3]) if len(sys.argv)>3 else 650
    g, bin_ = load_glb(path)
    tris = []          # list of (v0,v1,v2) world-space float tuples
    # traverse scene nodes applying world transforms
    scene = g.get("scene",0)
    roots = g["scenes"][scene]["nodes"]
    def walk(ni, parent):
        node = g["nodes"][ni]
        M = mat_mul(parent, trs_matrix(node))
        if "mesh" in node:
            mesh = g["meshes"][node["mesh"]]
            for prim in mesh["primitives"]:
                if "POSITION" not in prim.get("attributes",{}): continue
                pos = read_accessor(g, bin_, prim["attributes"]["POSITION"])
                wpos = [xform(M, p) for p in pos]
                if "indices" in prim:
                    idx = [i[0] for i in read_accessor(g, bin_, prim["indices"])]
                else:
                    idx = list(range(len(wpos)))
                for t in range(0, len(idx)-2, 3):
                    a,b,c = idx[t],idx[t+1],idx[t+2]
                    tris.append((wpos[a],wpos[b],wpos[c]))
        for ch in node.get("children",[]): walk(ch, M)
    for r in roots: walk(r, mat_ident())

    if not tris:
        print("no triangles found"); sys.exit(1)
    # bbox
    xs=[p[0] for t in tris for p in t]; ys=[p[1] for t in tris for p in t]; zs=[p[2] for t in tris for p in t]
    cx=(min(xs)+max(xs))/2; cy=(min(ys)+max(ys))/2; cz=(min(zs)+max(zs))/2
    height=max(ys)-min(ys);
    SCALE = 190.0/height if height>0 else 1.0   # normalise model height to ~190 units

    def norm(p): return ((p[0]-cx)*SCALE, (p[1]-cy)*SCALE, (p[2]-cz)*SCALE)

    # weld: snap to grid; increase grid until edge count <= target
    def build(grid):
        vmap={}; verts=[]; edges=set()
        def key(p):
            return (round(p[0]/grid), round(p[1]/grid), round(p[2]/grid))
        def vid(p):
            k=key(p)
            if k not in vmap:
                vmap[k]=len(verts); verts.append((k[0]*grid,k[1]*grid,k[2]*grid))
            return vmap[k]
        for (a,b,c) in tris:
            ia,ib,ic=vid(norm(a)),vid(norm(b)),vid(norm(c))
            for (u,v) in ((ia,ib),(ib,ic),(ic,ia)):
                if u!=v: edges.add((min(u,v),max(u,v)))
        return verts, sorted(edges)
    grid=2.0
    for _ in range(40):
        verts,edges=build(grid)
        if len(edges)<=target_edges: break
        grid*=1.18
    print(f"tris={len(tris)} -> verts={len(verts)} edges={len(edges)} grid={grid:.2f} scale={SCALE:.3f}")

    # emit C header
    with open(out,"w") as f:
        f.write("/* Auto-generated from MAKINA-7.glb by tools/glb_to_wire.py. Do not edit. */\n")
        f.write("#ifndef MAKINA7_MODEL_H\n#define MAKINA7_MODEL_H\n")
        f.write(f"#define MAKINA_NV {len(verts)}\n#define MAKINA_NE {len(edges)}\n")
        f.write("static const short MAKINA_V[MAKINA_NV][3] = {\n")
        for v in verts:
            f.write("  {%d,%d,%d},\n" % (int(round(v[0])),int(round(v[1])),int(round(v[2]))))
        f.write("};\n")
        f.write("static const unsigned short MAKINA_E[MAKINA_NE][2] = {\n")
        for e in edges:
            f.write("  {%d,%d},\n" % (e[0],e[1]))
        f.write("};\n#endif\n")
    print("wrote", out)

if __name__=='__main__':
    main()
