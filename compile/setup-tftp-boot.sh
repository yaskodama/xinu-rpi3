#!/bin/bash
# setup-tftp-boot.sh — one-time setup for Pi 3 network boot from this Mac.
#
# Pi 3 B+ default boot order is SD then network.  Once SD has no
# bootable kernel.img (deleted, blank SD, or corrupt), Pi 3 broadcasts
# a PXE DHCP request.  This script:
#   1. Populates /tftpboot with Pi 3 firmware files from the SD card
#      currently mounted at /Volumes/XINU.
#   2. Copies the freshly-built xinu.boot as kernel.img into /tftpboot.
#   3. Starts dnsmasq in proxy-DHCP mode (does not interfere with the
#      router's normal DHCP — only answers the PXE TFTP-info part).
#
# Run as: sudo ./setup-tftp-boot.sh
# Stop dnsmasq with Ctrl-C; the tftp serving stops with it.

set -e

if [ "$EUID" -ne 0 ]; then
  echo "Run with sudo."
  exit 1
fi

SDROOT=/Volumes/XINU
TFTPROOT=/tftpboot
DNSMASQ=/opt/homebrew/sbin/dnsmasq
CONF=/Users/kodamay/projects/xinu-raz/xinu/compile/dnsmasq-pi3-pxe.conf
KERNEL=/Users/kodamay/projects/xinu-raz/xinu/compile/xinu.boot

if [ ! -d "$SDROOT" ]; then
  echo "Pi 3 SD card not mounted at $SDROOT — insert SD in Mac first."
  exit 1
fi

mkdir -p "$TFTPROOT/overlays"
echo "[1] copying firmware from $SDROOT to $TFTPROOT ..."
cp -v "$SDROOT/bootcode.bin"     "$TFTPROOT/"
cp -v "$SDROOT/start.elf"        "$TFTPROOT/"
cp -v "$SDROOT/fixup.dat"        "$TFTPROOT/"
cp -v "$SDROOT/config.txt"       "$TFTPROOT/"
cp -v "$SDROOT/bcm2710-rpi-3-b-plus.dtb" "$TFTPROOT/"
cp -v "$SDROOT/overlays/"*.dtbo  "$TFTPROOT/overlays/" 2>/dev/null || true

echo "[2] copying xinu.boot as kernel.img ..."
cp -v "$KERNEL" "$TFTPROOT/kernel.img"

chmod -R a+r "$TFTPROOT"
chmod a+x "$TFTPROOT" "$TFTPROOT/overlays"

echo "[3] tftpboot ready:"
ls -la "$TFTPROOT/"

echo
echo "[4] starting dnsmasq (Ctrl-C to stop) ..."
echo "    Once Pi 3 reboots with SD's kernel.img absent or corrupt, it"
echo "    will PXE-DHCP, fetch bootcode.bin from this Mac, then chain-load"
echo "    start.elf / config.txt / kernel.img, all from $TFTPROOT."
echo
exec "$DNSMASQ" --no-daemon --conf-file="$CONF"
