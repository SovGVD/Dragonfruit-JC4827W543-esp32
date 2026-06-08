#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define IT8951_MODE_INIT  0
#define IT8951_MODE_DU    1
#define IT8951_MODE_GC16  2
#define IT8951_MODE_GL16  3
#define IT8951_MODE_A2    6

typedef struct {
    uint16_t panel_w;
    uint16_t panel_h;
    uint32_t img_buf_addr;
    char fw_version[16];
    char lut_version[16];
} it8951_dev_info_t;

esp_err_t it8951_init(it8951_dev_info_t *info);
void it8951_clear(uint16_t mode);
void it8951_load_area_start(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void it8951_load_pixels(const uint8_t *data_4bpp, size_t len);
void it8951_load_end(void);
void it8951_display_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode);
void it8951_wait_ready(void);

/* Fill an area of the controller image buffer with a solid 4bpp value (0x00 =
 * all ink/black, 0xFF = all blank/white) WITHOUT triggering a waveform — used
 * by the timing benchmark to set a known before/after pattern. x and w must be
 * 4-px aligned (4bpp pack-write constraint, see it8951_load_area_start). */
void it8951_fill_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t pix4);

/* Trigger a waveform refresh of the area in `mode` and BLOCK until the IT8951
 * display engine reports idle (LUTAFSR == 0), returning the elapsed wall-clock
 * microseconds. Unlike it8951_wait_ready() this polls tightly (microsecond
 * resolution) rather than at 10 ms granularity, so it can time the fast modes
 * accurately. Used only by the `wavetime` benchmark, on the display task. */
int64_t it8951_display_timed(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode);
