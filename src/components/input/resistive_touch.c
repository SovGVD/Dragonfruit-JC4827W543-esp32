#include "resistive_touch.h"

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
