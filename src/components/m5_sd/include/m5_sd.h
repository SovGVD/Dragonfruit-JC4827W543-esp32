#pragma once
#include <stdint.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

/* Shared SD mount API (name retained for existing consumers). On M5Paper,
 * display_init() owns SPI3 and SD shares it with CS4. On JC4827W543 this
 * component initializes the shared SD/XPT2046 SPI2 bus itself (CS10, CLK12,
 * MISO13, MOSI11). Unmount removes the card device, leaving the bus ready
 * for later config, install, or backup operations. No mount formats a card.
 */

/* FAT mount point shared by every SD user; build paths as M5_SD_MOUNT_POINT "/file". */
#define M5_SD_MOUNT_POINT "/sdcard"

/* Each emulated system keeps every file it owns in its own top-level folder:
 * /PalmOS (palm.rom, palm.cfg, install/, backup/, palm-card/, RAM checkpoints)
 * and /Cybiko (boot ROM, CyOS flash, saved RAM). FAT is case-insensitive. */
#define PALM_SD_DIR   M5_SD_MOUNT_POINT "/PalmOS"
#define CYBIKO_SD_DIR M5_SD_MOUNT_POINT "/Cybiko"

/* Initialize the JC4827W543 shared SD/XPT2046 SPI2 bus. Safe to call again;
 * does not require a card or display. JC4827W543 builds only. */
esp_err_t m5_sd_bus_init(void);

/* Mount the microSD at M5_SD_MOUNT_POINT. The FIRST sdspi transaction after the
 * IT8951 has been driving the shared bus intermittently CRC-fails until the
 * bus/card settle, so this retries a few times before giving up. On ESP_OK
 * *out_card holds the mounted card (hand it back to m5_sd_unmount); on failure
 * nothing is left mounted and the last error is returned. On JC4827W543 mounts
 * are reference counted: a virtual Palm card can keep the filesystem mounted
 * while checkpoint and backup callers acquire/release their own references.
 * Calls are serialized by boot / the CPU task, not from other tasks. */
esp_err_t m5_sd_mount(sdmmc_card_t **out_card);

/* Unmount the card from m5_sd_mount and remove the sdspi device, returning the
 * bus to the display. */
void m5_sd_unmount(sdmmc_card_t *card);

/* Capacity for Palm's 32-bit VFS API, capped at UINT32_MAX. */
esp_err_t m5_sd_space(uint32_t *used, uint32_t *total);
