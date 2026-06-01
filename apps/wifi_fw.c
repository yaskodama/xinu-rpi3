/**
 * @file wifi_fw.c
 *
 * Embedded Cypress/Broadcom BCM43455 SDIO firmware + NVRAM for the Pi 3 B+
 * on-board WiFi (Stage 3 firmware download).
 *
 * The blobs are NOT in git (nonfree license; .gitignore'd under wifi-fw/).
 * They were fetched from RPi-Distro/firmware-nonfree and staged at:
 *     wifi-fw/fw_43455.bin    = cypress/cyfmac43455-sdio-standard.bin (~609 KB)
 *     wifi-fw/nvram_43455.txt = brcm/brcmfmac43455-sdio.txt           (~2 KB)
 * They are linked directly into the kernel image via .incbin so wifi.c can
 * stream them into the chip's RAM over SDIO without a separate upload step.
 *
 * NOTE: the .incbin paths are ABSOLUTE because the assembler runs from the
 * compile/ directory; this is a local bring-up artifact (the blobs are local).
 */

asm(
    ".section .rodata\n"
    ".balign 4\n"
    ".globl wifi_fw_bin\n"
    "wifi_fw_bin:\n"
    "  .incbin \"/Users/kodamay/projects/xinu-raz/xinu/wifi-fw/fw_43455.bin\"\n"
    ".globl wifi_fw_bin_end\n"
    "wifi_fw_bin_end:\n"
    ".balign 4\n"
    ".globl wifi_nvram_txt\n"
    "wifi_nvram_txt:\n"
    "  .incbin \"/Users/kodamay/projects/xinu-raz/xinu/wifi-fw/nvram_43455.txt\"\n"
    ".globl wifi_nvram_txt_end\n"
    "wifi_nvram_txt_end:\n"
    ".balign 4\n"
    ".globl wifi_clm_blob\n"
    "wifi_clm_blob:\n"
    "  .incbin \"/Users/kodamay/projects/xinu-raz/xinu/wifi-fw/clm_43455.blob\"\n"
    ".globl wifi_clm_blob_end\n"
    "wifi_clm_blob_end:\n"
    ".balign 4\n"
);
