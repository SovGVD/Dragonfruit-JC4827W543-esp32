#include <string.h>
#include "cy_mp3.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define IN_SIZE 4096            /* two of the largest layer 3 frames */

struct cy_mp3 {
    mp3dec_t dec;
    uint8_t in[IN_SIZE];
    size_t in_len;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t pcm_n, pcm_pos;      /* samples per channel in pcm, and the next */
    int channels;
    unsigned hz, kbps;
    uint32_t frac;              /* Q16 position between pcm_pos and pcm_pos + 1 */
    int16_t prev;               /* the sample before pcm_pos, for interpolation */
};

size_t cy_mp3_size(void) { return sizeof(cy_mp3_t); }

void cy_mp3_init(cy_mp3_t *d)
{
    memset(d, 0, sizeof *d);
    mp3dec_init(&d->dec);
}

size_t cy_mp3_space(const cy_mp3_t *d) { return IN_SIZE - d->in_len; }

void cy_mp3_feed(cy_mp3_t *d, const uint8_t *data, size_t len)
{
    if (len > IN_SIZE - d->in_len) len = IN_SIZE - d->in_len;
    memcpy(d->in + d->in_len, data, len);
    d->in_len += len;
}

bool cy_mp3_busy(const cy_mp3_t *d) { return d->in_len || d->pcm_pos < d->pcm_n; }
unsigned cy_mp3_rate(const cy_mp3_t *d) { return d->hz; }
unsigned cy_mp3_kbps(const cy_mp3_t *d) { return d->kbps; }

/* Decode the next frame into pcm; false when the input holds none. */
static bool decode(cy_mp3_t *d)
{
    while (d->in_len) {
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(&d->dec, d->in, (int)d->in_len, d->pcm, &info);
        size_t used = (size_t)info.frame_bytes;
        if (!used) {
            /* No whole frame yet. A full buffer without one is junk. */
            if (d->in_len < IN_SIZE) return false;
            used = d->in_len;
        }
        memmove(d->in, d->in + used, d->in_len - used);
        d->in_len -= used;
        if (n > 0) {
            d->pcm_n = (size_t)n;
            d->pcm_pos = 0;
            d->channels = info.channels;
            d->hz = (unsigned)info.hz;
            d->kbps = (unsigned)info.bitrate_kbps;
            return true;
        }
    }
    return false;
}

static int16_t sample(const cy_mp3_t *d, size_t i)
{
    if (d->channels == 2) return (int16_t)(((int32_t)d->pcm[2 * i] + d->pcm[2 * i + 1]) / 2);
    return d->pcm[i];
}

size_t cy_mp3_render(cy_mp3_t *d, int16_t *out, size_t n, unsigned rate)
{
    size_t done = 0;
    while (done < n) {
        if (d->pcm_pos >= d->pcm_n && !decode(d)) break;
        /* Linear interpolation from the stream rate to `rate`. */
        uint32_t step = (uint32_t)(((uint64_t)d->hz << 16) / rate);
        while (done < n && d->pcm_pos < d->pcm_n) {
            int32_t a = d->prev, b = sample(d, d->pcm_pos);
            out[done++] = (int16_t)(a + (((b - a) * (int32_t)d->frac) >> 16));
            d->frac += step;
            while (d->frac >= 0x10000 && d->pcm_pos < d->pcm_n) {
                d->frac -= 0x10000;
                d->prev = sample(d, d->pcm_pos++);
            }
        }
    }
    size_t real = done;
    if (done < n) {
        memset(out + done, 0, (n - done) * sizeof *out);
        d->prev = 0;
        d->frac = 0;
    }
    return real;
}
