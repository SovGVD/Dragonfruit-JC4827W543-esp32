#pragma once
#include <stdbool.h>

typedef struct {
    int x_min, x_max, y_min, y_max;
    bool swap_xy, invert_x, invert_y;
    int pressure_min;
} resistive_calibration_t;

void resistive_calibration_default(resistive_calibration_t *c);
bool resistive_calibration_valid(const resistive_calibration_t *c);
/* Raw X is the XPT2046 D0 conversion, raw Y is the 90 conversion. Bounds
 * apply AFTER optional axis swap. Output is clamped to native panel pixels. */
bool resistive_touch_map(const resistive_calibration_t *c, int raw_x, int raw_y,
                         int panel_w, int panel_h, int *x, int *y);
