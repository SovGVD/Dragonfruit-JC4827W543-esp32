#include "input.h"
#include "input_events.h"
#include "resistive_touch.h"
#include "board.h"
#include "m5_sd.h"
#include "layout.h"
#include "palm_config.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR    /* host builds */
#endif

static const char *TAG = "xpt2046";
static spi_device_handle_t touch;
static resistive_calibration_t calibration, calibration_prev;
static touch_cal_source_t calibration_source = TOUCH_CAL_DEFAULT;
static bool log_raw, pen_down;
static volatile bool power_pending;
static int last_x = -1, last_y = -1;

/* 12-bit differential conversion, PD1:PD0=00: power down after each sample
 * so PENIRQ is enabled between polls. An 8-bit command followed by 16 clocks
 * returns one null bit, 12 ADC bits, then three trailing bits. */
static esp_err_t read_adc(uint8_t command, int *value)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 24,
        .tx_data = { command, 0, 0 },
    };
    esp_err_t err = spi_device_polling_transmit(touch, &t);
    if (err == ESP_OK)
        *value = (((unsigned)t.rx_data[1] << 8) | t.rx_data[2]) >> 3 & 0xfff;
    return err;
}

static int median5(int v[5])
{
    for (int i = 1; i < 5; i++) {
        int n = v[i], j = i;
        while (j > 0 && v[j - 1] > n) { v[j] = v[j - 1]; j--; }
        v[j] = n;
    }
    return v[2];
}

static bool sample(int *raw_x, int *raw_y, int *pressure)
{
    if (!touch || gpio_get_level(BOARD_TOUCH_INT)) return false;
    /* ESP-IDF SPI bus acquisition currently requires portMAX_DELAY. Runtime
     * SD work parks this input task before mounting, so there is no SD owner
     * waiting for us while we hold the bus across a filtered sample. */
    if (spi_device_acquire_bus(touch, portMAX_DELAY) != ESP_OK) return false;
    int z1 = 0, z2 = 4095, discard, xs[5], ys[5];
    bool ok = read_adc(0xb0, &z1) == ESP_OK && read_adc(0xc0, &z2) == ESP_OK;
    *pressure = z1 + 4095 - z2;
    ok = ok && *pressure >= calibration.pressure_min;
    /* Throw away the first conversion after switching the panel drive axis. */
    for (int i = 0; ok && i < 5; i++) {
        ok = read_adc(0xd0, &discard) == ESP_OK && read_adc(0xd0, &xs[i]) == ESP_OK &&
             read_adc(0x90, &discard) == ESP_OK && read_adc(0x90, &ys[i]) == ESP_OK;
    }
    spi_device_release_bus(touch);
    if (!ok) return false;
    *raw_x = median5(xs);
    *raw_y = median5(ys);
    return true;
}

static bool config_int(const char *text, const char *key, int low, int high, int *out)
{
    char value[32], *end;
    if (!config_get_value(text, key, value, sizeof(value))) return true;
    long n = strtol(value, &end, 10);
    if (end == value || *end || n < low || n > high) {
        ESP_LOGW(TAG, "%s invalid: '%s'", key, value);
        return false;
    }
    *out = (int)n;
    return true;
}

static void load_calibration(void)
{
    resistive_calibration_default(&calibration);
    static EXT_RAM_BSS_ATTR char text[LAYOUT_CFG_MAX];
    size_t length;
    if (config_load_from_sd(text, sizeof(text), &length) != CONFIG_OK) return;
    resistive_calibration_t next = calibration;
    int swap = next.swap_xy, ix = next.invert_x, iy = next.invert_y, log = 0;
    bool ok = true;
    ok &= config_int(text, "touch.x_min", 0, 4095, &next.x_min);
    ok &= config_int(text, "touch.x_max", 0, 4095, &next.x_max);
    ok &= config_int(text, "touch.y_min", 0, 4095, &next.y_min);
    ok &= config_int(text, "touch.y_max", 0, 4095, &next.y_max);
    ok &= config_int(text, "touch.swap_xy", 0, 1, &swap);
    ok &= config_int(text, "touch.invert_x", 0, 1, &ix);
    ok &= config_int(text, "touch.invert_y", 0, 1, &iy);
    ok &= config_int(text, "touch.pressure_min", 1, 4095, &next.pressure_min);
    ok &= config_int(text, "touch.log_raw", 0, 1, &log);
    next.swap_xy = swap; next.invert_x = ix; next.invert_y = iy;
    if (ok && resistive_calibration_valid(&next)) {
        calibration = next;
        log_raw = log;
        char v[32];
        if (config_get_value(text, "touch.x_min", v, sizeof v))
            calibration_source = TOUCH_CAL_CONFIG;
    } else {
        ESP_LOGW(TAG, "Invalid touch calibration; using defaults");
    }
}

/* On-screen calibration result in NVS. pressure_min stays with palm.cfg. */
typedef struct {
    uint8_t version;
    uint8_t swap_xy, invert_x, invert_y;
    int16_t x_min, x_max, y_min, y_max;
} saved_cal_t;
#define SAVED_CAL_VERSION 1

touch_cal_source_t input_touch_load_saved(void)
{
    nvs_handle_t h;
    saved_cal_t s;
    size_t len = sizeof s;
    if (nvs_open("touch", NVS_READONLY, &h) != ESP_OK) return calibration_source;
    esp_err_t err = nvs_get_blob(h, "cal", &s, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof s || s.version != SAVED_CAL_VERSION) return calibration_source;
    resistive_calibration_t next = calibration;
    next.x_min = s.x_min; next.x_max = s.x_max;
    next.y_min = s.y_min; next.y_max = s.y_max;
    next.swap_xy = s.swap_xy; next.invert_x = s.invert_x; next.invert_y = s.invert_y;
    if (!resistive_calibration_valid(&next)) {
        ESP_LOGW(TAG, "Saved touch calibration invalid; ignoring it");
        return calibration_source;
    }
    calibration = next;
    calibration_source = TOUCH_CAL_SAVED;
    ESP_LOGI(TAG, "Saved touch calibration: x %d-%d y %d-%d swap %d invert %d,%d",
             next.x_min, next.x_max, next.y_min, next.y_max,
             next.swap_xy, next.invert_x, next.invert_y);
    return calibration_source;
}

bool input_touch_raw(int *raw_x, int *raw_y)
{
    int pressure;
    return sample(raw_x, raw_y, &pressure);
}

bool input_touch_calibration_try(const int raw[4][2], int x0, int y0, int x1, int y1)
{
    resistive_calibration_t next = calibration;
    if (!resistive_calibration_solve(raw, x0, y0, x1, y1, BOARD_PANEL_W, BOARD_PANEL_H, &next))
        return false;
    calibration_prev = calibration;
    calibration = next;
    return true;
}

void input_touch_calibration_commit(bool keep)
{
    if (!keep) {
        calibration = calibration_prev;
        return;
    }
    const resistive_calibration_t *c = &calibration;
    saved_cal_t s = {
        SAVED_CAL_VERSION, c->swap_xy, c->invert_x, c->invert_y,
        (int16_t)c->x_min, (int16_t)c->x_max, (int16_t)c->y_min, (int16_t)c->y_max,
    };
    nvs_handle_t h;
    bool ok = nvs_open("touch", NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(h, "cal", &s, sizeof s) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    if (!ok) {
        ESP_LOGW(TAG, "Could not save touch calibration");
    } else {
        calibration_source = TOUCH_CAL_SAVED;
        ESP_LOGI(TAG, "Touch calibration saved: x %d-%d y %d-%d swap %d invert %d,%d",
                 c->x_min, c->x_max, c->y_min, c->y_max, c->swap_xy, c->invert_x, c->invert_y);
    }
}

esp_err_t input_init(void)
{
    ESP_RETURN_ON_ERROR(input_events_init(), TAG, "input queues");
    /* Read configuration before attaching the touch slave. This initializes
     * SPI2 even if no card is present; later SD mounts reuse the same bus. */
    load_calibration();
    ESP_RETURN_ON_ERROR(m5_sd_bus_init(), TAG, "shared SPI2 init");
    const gpio_config_t irq = {
        .pin_bit_mask = (1ULL << BOARD_TOUCH_INT) | (1ULL << BOARD_BTN_POWER),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq), TAG, "touch IRQ / BOOT GPIO");
    const spi_device_interface_config_t cfg = {
        .clock_speed_hz = 2000000,
        .mode = 0,
        .spics_io_num = BOARD_TOUCH_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &cfg, &touch), TAG, "XPT2046 device");
    int discard;
    read_adc(0x90, &discard); /* ensure ADC power-down / PENIRQ enabled */
    ESP_LOGI(TAG, "Resistive touch ready: CS=%d IRQ=%d, SPI2 shared with SD",
             BOARD_TOUCH_CS, BOARD_TOUCH_INT);
    return ESP_OK;
}

void input_poll(void)
{
    static bool raw_down;
    int rx, ry, pressure, px, py, x, y;
    bool down = sample(&rx, &ry, &pressure) &&
        resistive_touch_map(&calibration, rx, ry, layout_panel_w(), layout_panel_h(), &px, &py);
    if (down && !raw_down)
        ESP_LOGI(TAG, "contact raw=%d,%d pressure=%d panel=%d,%d", rx, ry, pressure, px, py);
    if (down && log_raw) {
        static TickType_t last_log;
        TickType_t now = xTaskGetTickCount();
        if (!raw_down || now - last_log >= pdMS_TO_TICKS(200)) {
            ESP_LOGI(TAG, "raw=%d,%d pressure=%d panel=%d,%d", rx, ry, pressure, px, py);
            last_log = now;
        }
    }
    raw_down = down;
    down = down && layout_panel_to_palm(px, py, &x, &y);
    if (down) {
        if (!pen_down)
            ESP_LOGI(TAG, "pen DOWN palm=%d,%d", x, y);
        if (!pen_down || x != last_x || y != last_y)
            input_inject_pen(x, y, false);
        last_x = x; last_y = y;
    } else if (pen_down) {
        input_inject_pen(-1, -1, true);
        last_x = last_y = -1;
    }
    pen_down = down;

    static bool was_pressed, fired;
    static TickType_t pressed_at;
    bool pressed = !gpio_get_level(BOARD_BTN_POWER);
    if (pressed && !was_pressed) {
        pressed_at = xTaskGetTickCount();
        fired = false;
    } else if (pressed && !fired && xTaskGetTickCount() - pressed_at >= pdMS_TO_TICKS(1000)) {
        power_pending = true;
        fired = true;
    }
    was_pressed = pressed;
}

bool input_pop_power_key(void)
{
    bool pending = power_pending;
    power_pending = false;
    return pending;
}

int input_pop_br_request(void) { return INPUT_BR_NONE; }
bool input_button_held(int which) { (void)which; return false; }

bool input_touch_panel(int *x, int *y)
{
    int rx, ry, pressure;
    return sample(&rx, &ry, &pressure) &&
        resistive_touch_map(&calibration, rx, ry, BOARD_PANEL_W, BOARD_PANEL_H, x, y);
}

bool input_boot_button_held(void)
{
    return !gpio_get_level(BOARD_BTN_POWER);
}
