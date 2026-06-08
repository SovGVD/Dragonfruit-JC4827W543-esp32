#pragma once

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ *
 *  GT911 touch-sensitivity config (digitizer.cfg)
 *
 *  The GT911 keeps its tuning in a config block at register 0x8047. This module
 *  is the file-format + block-mapping half of the digitizer.cfg feature: it has
 *  NO ESP-IDF or I2C dependencies (pure C), so the host build can unit-test the
 *  parse/serialize/checksum logic. input.c owns the actual I2C block read/write.
 *
 *  The full block is 186 bytes (0x8047..0x8100): config bytes 0..183, a checksum
 *  at offset 184 (reg 0x80FF), and a "config fresh" flag at offset 185 (0x8100).
 * ------------------------------------------------------------------ */

#define GT911_CFG_BASE   0x8047   /* first config register */
#define GT911_CFG_LEN    186      /* block length incl. checksum + fresh flag */
#define GT911_CFG_CKSUM  184      /* block offset of the checksum byte (0x80FF) */
#define GT911_CFG_FRESH  185      /* block offset of the config-fresh flag (0x8100) */

/* The curated set of touch-sensitivity knobs exposed via digitizer.cfg. Each is
 * one byte in the GT911 config block (offsets in touch_config.c). Lower
 * touch_level = more sensitive; leave_level must stay below touch_level. */
typedef struct {
    uint8_t shake_count;       /* 0x804F - samples to confirm a touch (debounce) */
    uint8_t filter;            /* 0x8050 - coordinate smoothing */
    uint8_t large_touch;       /* 0x8051 - large-area touch count */
    uint8_t noise_reduction;   /* 0x8052 - noise floor */
    uint8_t touch_level;       /* 0x8053 - press threshold (lower = more sensitive) */
    uint8_t leave_level;       /* 0x8054 - release threshold (< touch_level) */
} gt911_sens_t;

/* Optional per-key logging sink used by gt911_sens_parse() so the (pure) parser
 * can report applied/skipped keys without pulling in ESP_LOG. Pass NULL to mute. */
typedef void (*gt911_sens_log_fn)(void *ctx, const char *msg);

/* Extract the curated knobs from a freshly-read GT911 config block. */
void gt911_sens_from_block(const uint8_t *block, gt911_sens_t *out);

/* Overlay the curated knobs onto a config block in place. Does NOT touch the
 * checksum or fresh-flag bytes — the caller recomputes the checksum and sets the
 * fresh flag before writing the block back to the chip. */
void gt911_sens_to_block(const gt911_sens_t *s, uint8_t *block);

/* GT911 config checksum: two's-complement negation of the sum of the config
 * bytes [0, GT911_CFG_CKSUM). Store the result at block[GT911_CFG_CKSUM]. */
uint8_t gt911_cfg_checksum(const uint8_t *block);

/* Serialize the knobs to commented INI text (NUL-terminated) into buf. Returns
 * the text length (excluding the NUL), or -1 if it would not fit in cap. */
int gt911_sens_serialize(const gt911_sens_t *s, char *buf, size_t cap);

/* Parse digitizer.cfg `text`, merging valid keys onto *s (seed it with the
 * chip's current values first). Lenient: unknown keys and out-of-range values
 * (not 0..255) are skipped, leaving that field at its seeded value. After the
 * pass, enforces touch_level > leave_level, reverting whichever of the two was
 * changed this pass if the constraint is violated. `log`/`log_ctx` receive a
 * line per applied/skipped key (NULL to mute). Returns the number of keys
 * applied (after any hysteresis revert). */
int gt911_sens_parse(const char *text, gt911_sens_t *s,
                     gt911_sens_log_fn log, void *log_ctx);
