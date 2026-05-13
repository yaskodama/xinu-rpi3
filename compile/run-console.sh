#!/usr/bin/env bash
#
# Console-only run of XINU under QEMU on macOS.
# UART (= xsh$ shell) is multiplexed onto this terminal.
#
# Quit: Ctrl-A then x
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

echo "==> XINU console mode ($PLATFORM, $MEM RAM)"
echo "==> Quit: Ctrl-A then x"
exec qemu-system-arm \
    -M versatilepb -cpu arm1176 -m "$MEM" \
    -nographic \
    -kernel xinu.boot
