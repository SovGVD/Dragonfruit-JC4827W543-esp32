#pragma once
#include <stdbool.h>

/* Full-screen resistive touch calibration: the user taps four crosshairs near
 * the corners, then one in the middle to check the result, which is saved to
 * NVS and used by the boot menu, Palm OS and the Cybiko keyboard. Pressing the
 * BOOT button cancels and keeps the previous calibration. Needs display_init(),
 * input_init() and nvs_flash_init(). Returns true if a calibration was saved. */
bool touch_calib_run(void);
