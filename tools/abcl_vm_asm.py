#!/usr/bin/env python3
"""abcl_vm_asm.py — assemble an AIPL "actor binary" (.avm) and (optionally) POST
it to a running Xinu kernel's /actor/loadvm endpoint, which loads it into the
dynamic class table, spawns the first class, and kicks it with "tick".

Usage:
  python3 abcl_vm_asm.py            # writes counter.avm
  python3 abcl_vm_asm.py <pi-ip> [port]   # also POST it to http://pi:port/actor/loadvm
"""
import struct, sys

OPS = dict(PUSHI=0x01, LDF=0x02, STF=0x03, LDA=0x04, SELF=0x05,
           ADD=0x10, SUB=0x11, MUL=0x12, DIV=0x13, MOD=0x14,
           LT=0x20, LE=0x21, GT=0x22, GE=0x23, EQ=0x24, NE=0x25,
           JMP=0x30, JZ=0x31, SEND=0x40, SPAWN=0x41, PRINT=0x42, RET=0x43)

class Asm:
    """Collects strings (the module string pool) and assembles methods."""
    def __init__(self):
        self.strs = []
    def sid(self, s):
        if s not in self.strs: self.strs.append(s)
        return self.strs.index(s)
    def code(self, prog):
        """prog = list of tuples; two-pass to resolve labels.
        ('label','L') / ('PUSHI',n) / ('LDF',f) / ('JZ','L') / ('SEND','m',nargs) / ('SPAWN',ci) / ('ADD',) ..."""
        # pass 1: offsets of labels
        off, labels = 0, {}
        def isz(ins):
            op = ins[0]
            if op == 'label': return 0
            if op in ('PUSHI',): return 5
            if op in ('LDF','STF','LDA'): return 2
            if op in ('JMP','JZ','SPAWN'): return 3
            if op == 'SEND': return 4
            return 1
        for ins in prog:
            if ins[0] == 'label': labels[ins[1]] = off
            else: off += isz(ins)
        # pass 2: emit
        b = bytearray()
        for ins in prog:
            op = ins[0]
            if op == 'label': continue
            b.append(OPS[op])
            if op == 'PUSHI': b += struct.pack('<i', ins[1])
            elif op in ('LDF','STF','LDA'): b.append(ins[1] & 0xff)
            elif op in ('JMP','JZ'): b += struct.pack('<H', labels[ins[1]])
            elif op == 'SPAWN': b += struct.pack('<H', ins[1])
            elif op == 'SEND': b += struct.pack('<H', self.sid(ins[1])); b.append(ins[2] & 0xff)
        return bytes(b)

def build_counter():
    a = Asm()
    # method tick(): count = count+1; print(count); if count<5: send self.tick()
    tick = a.code([
        ('LDF', 0), ('PUSHI', 1), ('ADD',), ('STF', 0),       # count = count + 1
        ('LDF', 0), ('PRINT',),                               # print(count)
        ('LDF', 0), ('PUSHI', 5), ('LT',), ('JZ', 'end'),     # if !(count<5) goto end
        ('SELF',), ('SEND', 'tick', 0),                       # send self.tick()
        ('label', 'end'), ('RET',),
    ])
    cls_name = a.sid("Counter"); tick_name = a.sid("tick")
    # module: header + strings + classes
    out = bytearray(b'AVM1')
    out += struct.pack('<H', len(a.strs))
    for s in a.strs:
        sb = s.encode(); out += struct.pack('<H', len(sb)) + sb
    out += struct.pack('<H', 1)                               # 1 class
    # class Counter: nameIdx, nfields=1, nmethods=1
    out += struct.pack('<HHH', cls_name, 1, 1)
    # method tick: nameIdx, nparams=0, codelen, code
    out += struct.pack('<HBH', tick_name, 0, len(tick)) + tick
    return bytes(out)

mod = build_counter()
open('counter.avm', 'wb').write(mod)
print(f"wrote counter.avm ({len(mod)} bytes)  — a self-incrementing Counter actor")
print("  tick(): count++; print(count); if count<5 send self.tick()")

if len(sys.argv) > 1:
    import urllib.request
    host = sys.argv[1]; port = sys.argv[2] if len(sys.argv) > 2 else "80"
    url = f"http://{host}:{port}/actor/loadvm"
    req = urllib.request.Request(url, data=mod, method='POST')
    try:
        print("POST", url, "->", urllib.request.urlopen(req, timeout=5).read().decode().strip())
    except Exception as e:
        print("POST failed:", e)
else:
    print("\nTo load+run it on a Pi (WiFi up, webactor port P):")
    print("  curl --data-binary @counter.avm http://<pi-ip>:<P>/actor/loadvm")
