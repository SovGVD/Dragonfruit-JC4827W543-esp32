/* On-screen calibration for the XPT2046 resistive touch panel. See
 * touch_calib.h. The raw readings for each target are the median of every
 * sample taken while the stylus is down, which rejects the noisy samples a
 * resistive panel gives as contact is made and broken. */
#include "touch_calib.h"
#include "board.h"
#include "display_ui.h"
#include "input.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "touch_calib";

#define INSET 30            /* corner targets, in pixels from the panel edges */
#define MAX_SAMPLES 64
#define MIN_SAMPLES 4       /* shorter contacts are ignored as bounces */
#define CHECK_TOLERANCE 12  /* the middle tap must land this close, in pixels */

#define COLOR_BG     UI_RGB(0x14, 0x14, 0x1c)
#define COLOR_TEXT   UI_RGB(0xff, 0xff, 0xff)
#define COLOR_DIM    UI_RGB(0x90, 0x90, 0xa0)
#define COLOR_TARGET UI_RGB(0xff, 0xd0, 0x30)
#define COLOR_BAD    UI_RGB(0xff, 0x60, 0x50)
#define COLOR_OK     UI_RGB(0x60, 0xe0, 0x60)

typedef enum { TAP_OK, TAP_CANCEL } tap_result_t;

static void text_centered(int y, const char *s, int scale, uint16_t fg)
{
    display_ui_text(BOARD_PANEL_W / 2 - display_ui_text_width(s, scale) / 2, y, s, scale, fg,
                    COLOR_BG);
}

static void screen(const char *step, const char *note, uint16_t note_color)
{
    display_ui_fill(0, 0, BOARD_PANEL_W, BOARD_PANEL_H, COLOR_BG);
    text_centered(70, "TOUCH CALIBRATION", 3, COLOR_TEXT);
    text_centered(104, step, 2, COLOR_DIM);
    if (note) text_centered(170, note, 2, note_color);
    text_centered(200, "TAP THE CENTRE OF EACH CROSS, A STYLUS WORKS BEST", 1, COLOR_DIM);
    text_centered(214, "BOOT KEY: CANCEL", 1, COLOR_DIM);
}

static void cross(int x, int y, uint16_t color)
{
    display_ui_fill(x - 12, y, 25, 1, color);
    display_ui_fill(x, y - 12, 1, 25, color);
    display_ui_fill(x - 3, y - 3, 7, 1, color);
    display_ui_fill(x - 3, y + 3, 7, 1, color);
    display_ui_fill(x - 3, y - 3, 1, 7, color);
    display_ui_fill(x + 3, y - 3, 1, 7, color);
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static bool cancelled(void)
{
    return input_boot_button_held();
}

/* Waits for one deliberate tap and returns the median of its samples, raw
 * (panel == false) or mapped to panel pixels (panel == true). */
static tap_result_t read_tap(bool panel, int *out_x, int *out_y)
{
    static int xs[MAX_SAMPLES], ys[MAX_SAMPLES];
    for (;;) {
        int x, y, n = 0;
        /* Start from no contact, so a held stylus doesn't count twice. */
        while (panel ? input_touch_panel(&x, &y) : input_touch_raw(&x, &y)) {
            if (cancelled()) return TAP_CANCEL;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        while (!(panel ? input_touch_panel(&x, &y) : input_touch_raw(&x, &y))) {
            if (cancelled()) return TAP_CANCEL;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        do {
            if (n < MAX_SAMPLES) { xs[n] = x; ys[n] = y; n++; }
            vTaskDelay(pdMS_TO_TICKS(10));
        } while (panel ? input_touch_panel(&x, &y) : input_touch_raw(&x, &y));
        if (n < MIN_SAMPLES) continue;
        qsort(xs, n, sizeof xs[0], cmp_int);
        qsort(ys, n, sizeof ys[0], cmp_int);
        *out_x = xs[n / 2];
        *out_y = ys[n / 2];
        return TAP_OK;
    }
}

static bool finish(bool saved)
{
    /* Let go of the BOOT button before the caller starts sampling it again. */
    while (input_boot_button_held()) vTaskDelay(pdMS_TO_TICKS(20));
    return saved;
}

bool touch_calib_run(void)
{
    const int tx[4] = { INSET, BOARD_PANEL_W - 1 - INSET, BOARD_PANEL_W - 1 - INSET, INSET };
    const int ty[4] = { INSET, INSET, BOARD_PANEL_H - 1 - INSET, BOARD_PANEL_H - 1 - INSET };
    const char *note = NULL;
    ESP_LOGI(TAG, "Starting");
    while (input_boot_button_held()) vTaskDelay(pdMS_TO_TICKS(20));

    for (;;) {
        int raw[4][2];
        for (int i = 0; i < 4; i++) {
            char step[16];
            snprintf(step, sizeof step, "STEP %d OF 5", i + 1);
            screen(step, note, COLOR_BAD);
            cross(tx[i], ty[i], COLOR_TARGET);
            if (read_tap(false, &raw[i][0], &raw[i][1]) == TAP_CANCEL) {
                ESP_LOGI(TAG, "Cancelled");
                return finish(false);
            }
            ESP_LOGI(TAG, "target %d,%d raw %d,%d", tx[i], ty[i], raw[i][0], raw[i][1]);
        }
        if (!input_touch_calibration_try(raw, tx[0], ty[0], tx[2], ty[2])) {
            note = "THE TAPS DID NOT LINE UP, ONCE MORE";
            continue;
        }

        const int cx = BOARD_PANEL_W / 2, cy = 145;
        screen("STEP 5 OF 5: CHECK", NULL, 0);
        cross(cx, cy, COLOR_TARGET);
        int x, y;
        if (read_tap(true, &x, &y) == TAP_CANCEL) {
            input_touch_calibration_commit(false);
            ESP_LOGI(TAG, "Cancelled");
            return finish(false);
        }
        int miss = abs(x - cx) > abs(y - cy) ? abs(x - cx) : abs(y - cy);
        ESP_LOGI(TAG, "check tap at %d,%d, %d px off", x, y, miss);
        if (miss > CHECK_TOLERANCE) {
            input_touch_calibration_commit(false);
            static char missed[48];
            snprintf(missed, sizeof missed, "MISSED BY %d PIXELS, ONCE MORE", miss);
            note = missed;
            continue;
        }
        input_touch_calibration_commit(true);
        screen("DONE", "CALIBRATION SAVED", COLOR_OK);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return finish(true);
    }
}
