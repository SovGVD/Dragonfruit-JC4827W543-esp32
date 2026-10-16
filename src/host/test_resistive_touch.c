#include "resistive_touch.h"
#include "layout.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    resistive_calibration_t c;
    resistive_calibration_default(&c);
    int x, y;
    assert(resistive_calibration_valid(&c));
    assert(resistive_touch_map(&c, 3900, 3900, 480, 272, &x, &y) && x == 0 && y == 0);
    assert(resistive_touch_map(&c, 200, 200, 480, 272, &x, &y) && x == 479 && y == 271);
    assert(resistive_touch_map(&c, 0, 0, 480, 272, &x, &y) && x == 479 && y == 271);
    assert(!resistive_touch_map(&c, -1, 0, 480, 272, &x, &y));
    assert(!resistive_touch_map(&c, 4096, 0, 480, 272, &x, &y));
    assert(!resistive_touch_map(&c, 0, 0, 0, 272, &x, &y));
    assert(!resistive_touch_map(&c, 0, 0, 480, 272, NULL, &y));
    c.x_max = c.x_min;
    assert(!resistive_touch_map(&c, 1000, 1000, 480, 272, &x, &y));
    c.x_max = 4096; assert(!resistive_calibration_valid(&c));
    resistive_calibration_default(&c);
    c.pressure_min = 0; assert(!resistive_calibration_valid(&c));

    /* Exercise all axis swap/inversion combinations over a grid of raw values,
     * checking bounds and monotonic direction independently of any layout. */
    for (int swap = 0; swap < 2; swap++) {
        for (int ix = 0; ix < 2; ix++) for (int iy = 0; iy < 2; iy++) {
            resistive_calibration_default(&c);
            c.swap_xy = swap; c.invert_x = ix; c.invert_y = iy;
            for (int rx = 0; rx < 4096; rx += 127) for (int ry = 0; ry < 4096; ry += 127) {
                assert(resistive_touch_map(&c, rx, ry, 480, 272, &x, &y));
                assert(x >= 0 && x < 480 && y >= 0 && y < 272);
            }
            int lo_x = swap ? 3900 : 200, lo_y = swap ? 200 : 3900;
            assert(resistive_touch_map(&c, lo_x, lo_y, 480, 272, &x, &y));
            assert(x == (ix ? 479 : 0) && y == (iy ? 0 : 271));
        }
    }

    /* A calibrated resistive sample must route to the LCD through the same
     * layout inverse as display rendering, including custom axis reversal. */
    resistive_calibration_default(&c);
    c.x_min = c.y_min = 0; c.x_max = 479; c.y_max = 271; c.invert_x = c.invert_y = false;
    layout_use_default(480, 272);
    const region_t *screen = layout_region(0);
    for (int py = 0; py < 160; py++) for (int px = 0; px < 160; px++) {
        int panel_x, panel_y, palm_x, palm_y;
        layout_src_to_panel(screen, px, py, &panel_x, &panel_y);
        assert(resistive_touch_map(&c, panel_x, panel_y, 480, 272, &x, &y));
        assert(layout_panel_to_palm(x, y, &palm_x, &palm_y));
        assert(palm_x == px && palm_y == py);
    }
    /* Captured physical crosshair taps from the earlier enlarged layout.
     * Assert their native-panel directions independently of the active scale. */
    resistive_calibration_default(&c);
    assert(resistive_touch_map(&c, 838, 3378, 480, 272, &x, &y));
    assert(x > 380 && y < 64); /* physical portrait top-left */
    assert(resistive_touch_map(&c, 2474, 616, 480, 272, &x, &y));
    assert(x < 210 && y > 210); /* physical portrait bottom-right */
    puts("ALL RESISTIVE TOUCH TESTS PASSED");
    return 0;
}
