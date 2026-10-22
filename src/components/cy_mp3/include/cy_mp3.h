#pragma once

/* The MP3 cartridge's decoder output: turns the MPEG stream CyOS sends to the
 * cartridge (cybiko_mp3_take) into mono 16-bit samples at the speaker's rate.
 * minimp3 does the decoding, so MPEG 1/2/2.5 layers 1-3 all play, as on the
 * MAS 3507D the real cartridge carries.
 *
 * Not thread safe: feed and render from one task, or guard them. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct cy_mp3 cy_mp3_t;

/* About 16 KB; the caller picks the memory (PSRAM is fine). */
size_t cy_mp3_size(void);
void cy_mp3_init(cy_mp3_t *d);

/* Bytes of MPEG data the decoder can take now. */
size_t cy_mp3_space(const cy_mp3_t *d);
void cy_mp3_feed(cy_mp3_t *d, const uint8_t *data, size_t len);

/* No more data is coming for now: decode what is buffered even when it is
 * less than a frame and the next header. Feeding data clears it. */
void cy_mp3_end(cy_mp3_t *d);

/* Fill `n` samples at `rate` Hz. Returns how many of them came from the
 * stream (junk in it plays as silence for as long as its bytes would last);
 * the rest are silence because the stream ran dry. */
size_t cy_mp3_render(cy_mp3_t *d, int16_t *out, size_t n, unsigned rate);

/* True while decoded or undecoded data is left. */
bool cy_mp3_busy(const cy_mp3_t *d);

/* The last frame's format, 0 before the first. */
unsigned cy_mp3_rate(const cy_mp3_t *d);
unsigned cy_mp3_kbps(const cy_mp3_t *d);
