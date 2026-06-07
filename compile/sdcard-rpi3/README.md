# Embedded Xinu on Raspberry Pi 3 — SD card setup (serial bring-up)

Built from `PLATFORM=arm-rpi3` (new platform port of the Pi1 `arm-rpi`):
peripheral base `0x3F000000`, Cortex-A53, multicore guard, `screenInit`
skipped (serial-first). Goal of this image: reach `xsh$` over the PL011
serial console on a real Pi 3.

This folder contains the two files **you** must add to the SD:
- `kernel.img`  — the Xinu kernel (objcopy -O binary of xinu.elf)
- `config.txt`  — Pi 3 boot config (32-bit, PL011→GPIO via disable-bt,
                  `init_uart_clock=3000000` to match the driver's baud)

You still need the **Broadcom firmware** on the SD (not in this repo):
`bootcode.bin`, `start.elf`, `fixup.dat`. Copy them from any Raspberry
Pi OS card's boot partition.

## Wiring the serial console
USB-TTL adapter ↔ Pi 3 GPIO header (3.3 V logic, do NOT connect the
adapter's 5 V):
- adapter GND → Pi pin 6 (GND)
- adapter RX  → Pi pin 8  (GPIO14 / TXD)
- adapter TX  → Pi pin 10 (GPIO15 / RXD)

Open the port at **115200 8N1** (e.g. `screen /dev/tty.usbserial-XXXX 115200`).

## Option A — fresh FAT32 card (cleanest)
```sh
# 1. Identify the microSD (NOT disk0 = internal!). Insert it, then:
diskutil list
# 2. Format as FAT32/MBR (DESTROYS the card). Replace diskN:
diskutil eraseDisk FAT32 XINU MBRFormat /dev/diskN
# 3. Copy firmware from a Raspberry Pi OS card's boot partition:
cp /Volumes/bootfs/{bootcode.bin,start.elf,fixup.dat} /Volumes/XINU/
#    (older Pi OS mounts as /Volumes/boot)
# 4. Copy our two files:
cp kernel.img config.txt /Volumes/XINU/
# 5. Eject, insert into Pi 3, power on, watch the serial console.
diskutil eject /Volumes/XINU
```

## Option B — reuse a working Raspberry Pi OS card (no format)
On the Pi OS boot partition (FAT32, firmware already there):
1. Back up its `config.txt`.
2. Copy our `kernel.img` and `config.txt` onto it (our `config.txt`
   sets `kernel=kernel.img`, so the stock `kernel7.img/kernel8.img`
   stays but is unused).
3. Eject and boot.

## Expected first boot & likely failure modes
Bare-metal ports rarely boot perfectly on the first try. Capture **all**
serial output and share it. Likely first-iteration issues:
- **silent console** → baud/clock (init_uart_clock) or disable-bt not applied
- **garbage characters** → UART clock mismatch (must be 3 MHz)
- **boots then hangs** → a peripheral still at the Pi1 base, or an early
  driver (timer/interrupt) needing a Pi3 fix
We iterate from whatever the serial shows.
