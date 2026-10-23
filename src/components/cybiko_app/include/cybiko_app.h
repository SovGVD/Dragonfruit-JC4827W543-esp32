#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Cybiko Xtreme mode of the firmware: everything it reads or writes lives in
 * CYBIKO_SD_DIR (/Cybiko on the SD card). */

/* Find the boot ROM and CyOS flash image in /Cybiko (SD must be mounted).
 * Accepted names, first match wins:
 *   boot ROM (32 KB):  bootrom.bin, cyrom150.bin, xtreme-rom.bin
 *   flash   (512 KB):  flash.bin,   cyos_v1508.bin, xtreme-flash.bin
 * Either output may be NULL. Returns true when both files exist. */
bool cybiko_app_find_files(char *rom_path, char *flash_path, size_t cap);
/* The original flash dump (the names above without flash.bin). */
bool cybiko_app_find_dump(char *flash_path, size_t cap);

/* A CyOS update image in /Cybiko: a file named *.boot of 64-512 KB, such as
 * the Russian ccyphRU1.5.08r.boot. It is installed into the flash on start
 * (see cybiko_flash_stage_boot), the result is saved as flash.bin and the
 * file is renamed to *.boot.done. */
#define CYBIKO_UPDATE_MIN (64 * 1024)
#define CYBIKO_UPDATE_MAX (512 * 1024)
bool cybiko_app_find_update(char *path, size_t cap);

/* Load the images, start the emulator tasks and return. display_init() and
 * input_init() must already have run. On a fatal error the reason is shown
 * on the panel and the function returns without starting anything. */
void cybiko_app_start(void);

/* Screen orientation for Cybiko mode, kept in NVS and chosen in the boot
 * menu: true (the default) is portrait, false landscape. */
bool cybiko_app_portrait(void);
void cybiko_app_set_portrait(bool portrait);

/* Whether the MP3 cartridge is plugged in, kept in NVS and switched in the
 * boot menu: true (the default) leaves it to the cart key in cybiko.cfg,
 * false takes it out. Some games refuse to start with the cartridge in, on
 * the real machine too. */
bool cybiko_app_cart_in(void);
void cybiko_app_set_cart_in(bool in);

/* Whole frames ("vsync"), kept in NVS and switched in the boot menu: true
 * (the default) shows the screen only once CyOS has finished drawing it,
 * false shows it as it is drawn. */
bool cybiko_app_vsync(void);
void cybiko_app_set_vsync(bool on);
