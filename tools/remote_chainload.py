#!/usr/bin/env python3
"""Remote kernel chainload for Xinu Pi 3 (RAM-only, no SD write -> no brick risk).

Uploads a freshly-built kernel image to the running Pi 3's webactor and boots it
in place — no power cycle, no SD swap.  Two HTTP steps:

  1. POST /upload      — stream the whole image into the 1.5 MB staging slot
  2. POST /chainload   — hardened RAM boot (masks IRQ+FIQ, tears down MMU/cache
                         atomically, copies the staged image to 0x8000 and jumps)

A bad image just needs a power cycle; the SD kernel.img is never touched.  The
new kernel comes up on HDMI immediately.  (USB keyboard/mouse and the LAN78xx
ethernet are NOT power-cycled, so the network may not survive the warm boot —
if you need to chainload again and the Pi is unreachable, power-cycle once.
For final deployment, burn xinu.boot to the SD kernel.img.)

  python3 tools/remote_chainload.py [host] [kernel.img]
  default host=192.168.3.50:8080  kernel=compile/xinu.boot
"""
import sys, time, urllib.request

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.3.50:8080"
img  = sys.argv[2] if len(sys.argv) > 2 else "compile/xinu.boot"
if "://" not in host:
    host = "http://" + host

data = open(img, "rb").read()
n = len(data)
print(f"[chainload] {img}: {n} bytes -> {host}")

# 1) Upload the whole image into the staging slot.
t0 = time.time()
req = urllib.request.Request(f"{host}/upload?dst=xinu.boot", data=data,
                             method="POST",
                             headers={"Content-Type": "application/octet-stream"})
r = urllib.request.urlopen(req, timeout=180).read().decode("ascii", "replace").strip()
print(f"[upload] {r}  ({time.time()-t0:.1f}s)")

# 2) Commit: boot the staged kernel.  The Pi replies 200 then jumps, so a
#    short read timeout / connection drop after the 200 is expected.
try:
    req = urllib.request.Request(f"{host}/chainload", data=b"", method="POST")
    r = urllib.request.urlopen(req, timeout=10).read().decode("ascii", "replace").strip()
    print(f"[chainload] {r}")
except Exception as e:
    print(f"[chainload] commit sent (connection dropped as the Pi boots: {e})")

print("[chainload] new kernel should be booting on HDMI now.")
