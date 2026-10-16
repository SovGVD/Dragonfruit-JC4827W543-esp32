#pragma once

#include <stddef.h>
#include <stdbool.h>

/* Outcome of trying to read the optional layout config off the SD card. Only
 * CONFIG_OK means the buffer holds config text; every other value means "no
 * usable config" and the caller should fall back to the default layout. A
 * missing file is NORMAL (the stock layout is the default), not an error. */
typedef enum {
    CONFIG_OK = 0,
    CONFIG_NO_SD,      /* SD card absent or the FAT volume would not mount */
    CONFIG_NO_FILE,    /* SD mounted but /palm.cfg is absent (the common case) */
    CONFIG_TOO_BIG,    /* file larger than the supplied buffer */
    CONFIG_READ_ERR,   /* open/read failure */
} config_load_result_t;

/* Mount the board microSD, read /palm.cfg into `buf` (NUL-terminated, at most
 * cap-1 bytes), then unmount. On CONFIG_OK *out_len is the text length.
 *
 * The board SD component owns bus initialization and removes the card device
 * before returning. JC4827W543 uses SPI2 shared with resistive touch; the
 * original target requires display_init() to initialize its shared SPI3 bus. */
config_load_result_t config_load_from_sd(char *buf, size_t cap, size_t *out_len);

/* Generic form of config_load_from_sd: mount, read `path` (a full
 * M5_SD_MOUNT_POINT-rooted path, e.g. "/sdcard/digitizer.cfg") into `buf`
 * (NUL-terminated, at most cap-1 bytes), then unmount. CONFIG_NO_FILE only when
 * the file is genuinely absent (errno == ENOENT, a normal expected case for
 * optional config); any other open failure returns CONFIG_READ_ERR so a
 * transient error never masquerades as "absent" (which a caller might act on by
 * recreating the file). Same SD bus caveats as above. config_load_from_sd is a
 * thin wrapper over this. */
config_load_result_t config_read_file(const char *path, char *buf, size_t cap,
                                      size_t *out_len);

/* Mount the microSD, (over)write `len` bytes from `buf` to `path`, then unmount.
 * The file is removed on a short write so a partial file is never left behind.
 * Returns CONFIG_OK on success, CONFIG_NO_SD if the card won't mount, or
 * CONFIG_READ_ERR on any open/write failure. Used to create an optional config
 * file from the device's current values when it doesn't yet exist. */
config_load_result_t config_write_file(const char *path, const void *buf,
                                       size_t len);

/* Look up a flat "key = value" line in already-loaded /palm.cfg text (the same
 * INI-ish format layout_load consumes — it tolerates these as unknown keys).
 * Read-only on `text`; trims surrounding whitespace, honors #/; comments, and
 * strips a trailing CR. Copies the value into `out` (NUL-terminated, <= cap-1
 * bytes) and returns true; returns false if the key is absent. The single shared
 * key lookup for non-layout settings (e.g. serial.mode/serial.baud). */
bool config_get_value(const char *text, const char *key, char *out, size_t cap);
