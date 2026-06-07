/**
 * @file sd_block.h
 *
 * Minimal SDHCI block driver for BCM2837 EMMC on Raspberry Pi 3.
 * Read/write a single 512-byte sector by LBA.  No filesystem; just raw
 * blocks — the FAT layer (apps/fat.c) sits on top of this.
 *
 * The driver assumes the firmware bootloader already initialised the EMMC
 * controller to read kernel.img off the SD card.  We just reuse that
 * already-selected card without re-initialising clocks/power/CMD0/CMD7.
 */
#ifndef SD_BLOCK_H
#define SD_BLOCK_H

#define SD_BLOCK_SIZE 512

/**
 * Sanity-check the SD controller by reading LBA 0 and verifying the MBR
 * signature (0x55 0xAA at offset 510).  Returns 0 on success, -1 if the
 * read failed or the signature is wrong.
 */
int sd_init(void);

/**
 * Read one 512-byte sector at the given LBA into buf.  buf must be at
 * least SD_BLOCK_SIZE bytes and 4-byte aligned (we drain the data port
 * as 32-bit words).  Returns 0 on success, -1 on timeout or error.
 */
int sd_read_block(unsigned long lba, void *buf);

/**
 * Write one 512-byte sector at the given LBA from buf.  Same alignment
 * requirements as sd_read_block.  Returns 0 on success, -1 on error.
 */
int sd_write_block(unsigned long lba, const void *buf);

#endif /* SD_BLOCK_H */
