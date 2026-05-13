#!/usr/bin/env bash
#
# Window run of XINU under QEMU on macOS.
# LCD framebuffer pops up in a native Cocoa window (the WM screen),
# while the UART shell is multiplexed onto this terminal via -serial mon:stdio.
#
# Quit:
#   - close the QEMU window, or
#   - in this terminal:  Ctrl-A then x   (= QEMU monitor escape)
#
# Env overrides:
#   XINU_MEM       memory size, default 128M
#   XINU_PLATFORM  build target,  default arm-qemu

set -euo pipefail

cd "$(dirname "$0")"

MEM="${XINU_MEM:-128M}"
PLATFORM="${XINU_PLATFORM:-arm-qemu}"

if ! command -v qemu-system-arm >/dev/null 2>&1; then
    echo "qemu-system-arm not found.  Install with:  brew install qemu" >&2
    exit 1
fi

if [[ ! -f xinu.boot || Makefile -nt xinu.boot ]]; then
    echo "==> building $PLATFORM..."
    make PLATFORM="$PLATFORM"
fi

echo "==> XINU window mode ($PLATFORM, $MEM RAM)"
echo "==> LCD: QEMU window (drag corner to enlarge up to 4x)  |  shell: this terminal"
echo "==> Quit: close the window, or Ctrl-A then x here"

# -display cocoa,zoom-to-fit=on,zoom-interpolation=on
#   - lets the user drag the window corner to grow the LCD image freely.
#   - smooth-scales (interpolation=on) so 2x linear / 4x area is sharp.
# -serial mon:stdio
#   - route UART and the QEMU monitor onto stdio.  Ctrl-A x quits.
exec qemu-system-arm \
    -M versatilepb -cpu arm1176 -m "$MEM" \
    -kernel xinu.boot \
    -display cocoa,zoom-to-fit=on,zoom-interpolation=on \
    -serial mon:stdio
