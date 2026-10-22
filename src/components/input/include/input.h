#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* A pen sample produced by input_poll() (touch task) and consumed by the CPU
 * task. Coordinates are in Palm screen space (0..159). pen_up == true marks a
 * release; for an "up" event x/y are (-1,-1). */
typedef struct {
    int16_t x, y;
    bool    pen_up;
} input_pen_event_t;

esp_err_t input_init(void);

/* Touch in native panel pixels, outside the Palm layout (boot menu, Cybiko
 * keyboard). Samples the controller directly; returns false while untouched.
 * Not for use while the input task is polling. XPT2046 only; others return false. */
bool input_touch_panel(int *x, int *y);
/* Resistive touch calibration (XPT2046 only; elsewhere these do nothing and
 * report TOUCH_CAL_DEFAULT / false). Where the calibration in use came from: */
typedef enum { TOUCH_CAL_DEFAULT, TOUCH_CAL_CONFIG, TOUCH_CAL_SAVED } touch_cal_source_t;
/* Loads the calibration saved by the on-screen calibration from NVS, if any; it
 * replaces the touch.* bounds from palm.cfg. Call after nvs_flash_init(). */
touch_cal_source_t input_touch_load_saved(void);
/* Raw {D0, 90} conversion pair while touched, like input_touch_panel(). */
bool input_touch_raw(int *raw_x, int *raw_y);
/* Works out a calibration from taps on four targets at the corners of
 * (x0,y0)-(x1,y1): top-left, top-right, bottom-right, bottom-left. On success
 * it is used at once, so the caller can check it, then
 * input_touch_calibration_commit(true) saves it to NVS and false goes back to
 * the previous one. Returns false if the taps were inconsistent. */
bool input_touch_calibration_try(const int raw[4][2], int x0, int y0, int x1, int y1);
void input_touch_calibration_commit(bool keep);
/* Live level of the BOOT/power button (true while held). */
bool input_boot_button_held(void);
void input_poll(void);

/* Non-blocking: pop one queued pen event into *out. Returns true if one was
 * available. Safe to call from a different task than input_poll(). */
bool input_pop_pen_event(input_pen_event_t *out);

/* Non-blocking: returns true once per Power-button press while AWAKE (consumes
 * the latch). Consumed by the CPU task, which injects vchrHardPower via
 * EvtEnqueueKey to trigger sleep. (Waking from sleep uses the matrix POWER key
 * directly, not this latch — see input.c.) */
bool input_pop_power_key(void);

/* Backup/restore request, latched once per >=1s hold of a side button while
 * awake: UP (PIN_BTN_L) = backup, DOWN (PIN_BTN_R) = restore. A short tap of the
 * same button instead pages up/down (deferred tap; see input.c). Drained once by
 * the CPU task, which runs the confirm-countdown + engine. */
typedef enum { INPUT_BR_NONE = 0, INPUT_BR_BACKUP, INPUT_BR_RESTORE } input_br_req_t;
int input_pop_br_request(void);

/* Live state of a side button (active-low, sampled directly): which==0 -> UP
 * (PIN_BTN_L), which==1 -> DOWN (PIN_BTN_R). Lets the CPU task poll "is the
 * triggering button still held" during the backup/restore confirm countdown
 * while the input task is parked. Returns true while pressed. */
bool input_button_held(int which);

/* A key event queued for injection via the ROM's EvtEnqueueKey trap. ascii is
 * the WChar (or vchr* virtual code); modifiers carries e.g. commandKeyMask for
 * virtual keys. Drained by the CPU task (the sole writer of CPU state). */
typedef struct {
    uint16_t ascii, keycode, modifiers;
} input_key_event_t;

/* --- Injection API (used by the physical input task AND the debug console) ---
 * All three enqueue onto the same queues the CPU task already drains, so input
 * from any source flows through one race-free path and never touches CPU state
 * off the CPU task. */
void input_inject_pen(int16_t x, int16_t y, bool pen_up);
void input_inject_key(uint16_t ascii, uint16_t keycode, uint16_t modifiers);
bool input_pop_key(input_key_event_t *out);
