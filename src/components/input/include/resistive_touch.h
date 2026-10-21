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

/* On-screen calibration: the user taps four targets at the corners of the
 * rectangle (x0,y0)-(x1,y1) on the panel, in the order top-left, top-right,
 * bottom-right, bottom-left. raw[i] is the {D0, 90} conversion pair for tap i.
 * Works out swap/invert and the raw values at the panel edges, keeping
 * pressure_min from *out. Returns false if the taps are inconsistent (an axis
 * that barely moved, or corners that disagree), leaving *out unchanged. */
bool resistive_calibration_solve(const int raw[4][2], int x0, int y0, int x1, int y1,
                                 int panel_w, int panel_h, resistive_calibration_t *out);
