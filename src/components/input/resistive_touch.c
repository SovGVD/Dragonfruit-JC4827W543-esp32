#include "resistive_touch.h"
#include <stdlib.h>

void resistive_calibration_default(resistive_calibration_t *c)
{
    *c = (resistive_calibration_t){
        .x_min = 200, .x_max = 3900, .y_min = 200, .y_max = 3900,
        .invert_x = true, .invert_y = true, .pressure_min = 400,
    };
}

bool resistive_calibration_valid(const resistive_calibration_t *c)
{
    return c && c->x_min >= 0 && c->x_min < c->x_max && c->x_max <= 4095 &&
        c->y_min >= 0 && c->y_min < c->y_max && c->y_max <= 4095 &&
        c->pressure_min >= 1 && c->pressure_min <= 4095;
}

static int axis(int value, int min, int max, int size, bool invert)
{
    if (value < min) value = min;
    if (value > max) value = max;
    int px = (value - min) * (size - 1) / (max - min);
    return invert ? size - 1 - px : px;
}

bool resistive_touch_map(const resistive_calibration_t *c, int raw_x, int raw_y,
                         int panel_w, int panel_h, int *x, int *y)
{
    if (!resistive_calibration_valid(c) || panel_w <= 0 || panel_h <= 0 ||
        !x || !y || raw_x < 0 || raw_x > 4095 || raw_y < 0 || raw_y > 4095)
        return false;
    if (c->swap_xy) { int t = raw_x; raw_x = raw_y; raw_y = t; }
    *x = axis(raw_x, c->x_min, c->x_max, panel_w, c->invert_x);
    *y = axis(raw_y, c->y_min, c->y_max, panel_h, c->invert_y);
    return true;
}

/* Extrapolates one panel axis from the mean raw reading at its two target
 * lines (a0 at pixel p0, a1 at pixel p1) to the panel edges 0 and size-1. */
static bool solve_axis(int a0, int a1, int p0, int p1, int size, int *min, int *max, bool *invert)
{
    if (abs(a1 - a0) < 200) return false;      /* the axis barely moved */
    int r0 = a0 - p0 * (a1 - a0) / (p1 - p0);
    int r1 = a0 + (size - 1 - p0) * (a1 - a0) / (p1 - p0);
    *invert = r0 > r1;
    *min = *invert ? r1 : r0;
    *max = *invert ? r0 : r1;
    if (*min < 0) *min = 0;
    if (*max > 4095) *max = 4095;
    return *min < *max;
}

bool resistive_calibration_solve(const int raw[4][2], int x0, int y0, int x1, int y1,
                                 int panel_w, int panel_h, resistive_calibration_t *out)
{
    enum { TL, TR, BR, BL };
    if (!out || x1 <= x0 || y1 <= y0) return false;
    /* Whichever raw channel changes more from left to right drives panel X. */
    int dx0 = abs(raw[TR][0] + raw[BR][0] - raw[TL][0] - raw[BL][0]);
    int dx1 = abs(raw[TR][1] + raw[BR][1] - raw[TL][1] - raw[BL][1]);
    bool swap = dx1 > dx0;
    int a[4], b[4];                             /* raw value along panel X, Y */
    for (int i = 0; i < 4; i++) {
        a[i] = raw[i][swap ? 1 : 0];
        b[i] = raw[i][swap ? 0 : 1];
    }
    /* The two taps on each target line must agree, or a tap went astray. */
    int span_x = abs(a[TR] + a[BR] - a[TL] - a[BL]) / 2;
    int span_y = abs(b[BL] + b[BR] - b[TL] - b[TR]) / 2;
    if (abs(a[TL] - a[BL]) > span_x / 5 || abs(a[TR] - a[BR]) > span_x / 5 ||
        abs(b[TL] - b[TR]) > span_y / 5 || abs(b[BL] - b[BR]) > span_y / 5)
        return false;
    resistive_calibration_t c = *out;
    c.swap_xy = swap;
    if (!solve_axis((a[TL] + a[BL]) / 2, (a[TR] + a[BR]) / 2, x0, x1, panel_w,
                    &c.x_min, &c.x_max, &c.invert_x) ||
        !solve_axis((b[TL] + b[TR]) / 2, (b[BL] + b[BR]) / 2, y0, y1, panel_h,
                    &c.y_min, &c.y_max, &c.invert_y) ||
        !resistive_calibration_valid(&c))
        return false;
    *out = c;
    return true;
}
