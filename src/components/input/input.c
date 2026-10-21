#include "input.h"
#include "input_events.h"
#include "board.h"
#include "dragonball.h"
#include "layout.h"
#include "touch_config.h"
#include "palm_config.h"
#include "m5_sd.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "input";

/* Latched once per awake Power-button press; drained by the CPU task. */
static volatile bool power_key_pending = false;

bool input_pop_power_key(void)
{
    if (!power_key_pending)
        return false;
    power_key_pending = false;
    return true;
}

/* Backup/restore request, latched once per >=1s hold of the UP/DOWN side button
 * (see handle_hold_button below); drained by the CPU task. */
static volatile input_br_req_t br_req_pending = INPUT_BR_NONE;

int input_pop_br_request(void)
{
    input_br_req_t r = br_req_pending;
    if (r != INPUT_BR_NONE)
        br_req_pending = INPUT_BR_NONE;
    return r;
}

/* GT911 I2C */
#define GT911_ADDR          0x14
#define GT911_ADDR_ALT      0x5D
#define GT911_CMD_REG       0x8040
#define GT911_CFG_REG       0x8047
#define GT911_STATUS_REG    0x814E
#define GT911_TOUCH_REG     0x8150
#define GT911_PRODUCT_REG   0x8140
#define GT911_FWVER_REG     0x8144
#define GT911_RESOL_REG     0x8146

#define PIN_SDA BOARD_TOUCH_SDA
#define PIN_SCL BOARD_TOUCH_SCL
#define PIN_INT BOARD_TOUCH_INT
#define PIN_BTN_M BOARD_BTN_POWER
#if !CONFIG_BOARD_JC4827W543
#define PIN_BTN_L 37
#define PIN_BTN_R 39
#endif

bool input_button_held(int which)
{
#if CONFIG_BOARD_JC4827W543
    (void)which;
    return false; /* no M5Paper side buttons on this board */
#else
    return !gpio_get_level(which ? PIN_BTN_R : PIN_BTN_L);
#endif
}

/* Panel<->Palm mapping lives in components/layout (shared with display.c, the
 * single source of truth). The only fixed relation we still own here is the
 * GT911 raw -> native IT8951 panel-pixel step below (a hardware constant,
 * independent of how the device is held); layout_panel_to_palm() then turns a
 * native panel pixel into Palm coords for whatever layout is active. */

static uint8_t gt911_addr = GT911_ADDR;
static bool gt911_found = false;
static i2c_master_bus_handle_t gt911_bus;   /* I2C0 master bus */
static i2c_master_dev_handle_t gt911_dev;   /* GT911 device (7-bit addr bound at probe) */
static bool last_pen_down = false;
static int16_t last_px = -1, last_py = -1;   /* last streamed stroke point */
#if !CONFIG_BOARD_JC4827W543
static bool last_btn_l = false;
static bool last_btn_r = false;
/* Long-hold timing for the UP/DOWN side buttons (backup/restore trigger) plus
 * the deferred-tap pulse that lets a short tap still send a single page key. */
static TickType_t l_press_tick = 0, r_press_tick = 0;
static bool l_hold_fired = false, r_hold_fired = false;
static int  tap_row = -1, tap_col = 0;   /* page key held for a tap; row < 0 = idle */
static TickType_t tap_pulse_until = 0;
#endif
static bool last_btn_m = false;
static uint32_t poll_diag_count = 0;

/* --- GT911 I2C helpers --- */

/* Bind the GT911 device handle to a 7-bit address. The panel answers at 0x14 or
 * 0x5D (probed at init); re-creating the device handle is how the i2c_master API
 * retargets the address. */
static esp_err_t gt911_use_addr(uint8_t addr)
{
    if (gt911_dev) { i2c_master_bus_rm_device(gt911_dev); gt911_dev = NULL; }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    if (gt911_dev) {
        i2c_master_bus_rm_device(gt911_dev);
        gt911_dev = NULL;
    }
    gt911_addr = addr;
    return i2c_master_bus_add_device(gt911_bus, &dc, &gt911_dev);
}

/* Reads/writes are a big-endian 16-bit register address followed by data. The
 * 100 ms timeout matches the legacy i2c_master_cmd_begin budget. */
static esp_err_t gt911_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t r[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(gt911_dev, r, sizeof(r), buf, len, 100);
}

static esp_err_t gt911_write_byte(uint16_t reg, uint8_t val)
{
    uint8_t w[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(gt911_dev, w, sizeof(w), 100);
}

/* Write a multi-byte block in one transaction (used to push the whole 186-byte
 * GT911 config block at 0x8047). The GT911 applies an updated config only on a
 * single contiguous write ending with the checksum + fresh-flag bytes, so the
 * register address and payload go out as one transmit. */
static esp_err_t gt911_write_block(uint16_t reg, const uint8_t *buf, size_t len)
{
    uint8_t w[2 + GT911_CFG_LEN];   /* the 186-byte config block is the largest write */
    if (len > GT911_CFG_LEN) return ESP_ERR_INVALID_SIZE;
    w[0] = (uint8_t)(reg >> 8);
    w[1] = (uint8_t)(reg & 0xFF);
    memcpy(w + 2, buf, len);
    return i2c_master_transmit(gt911_dev, w, 2 + len, 100);
}

/* digitizer.cfg lives in the Palm folder (PALM_SD_DIR "/digitizer.cfg"). */
#define DIGITIZER_CFG_PATH PALM_SD_DIR "/digitizer.cfg"

/* Logging sink for the (pure) digitizer.cfg parser — routes per-key
 * applied/skipped messages to the serial log. */
static void gt911_cfg_log(void *ctx, const char *msg)
{
    (void)ctx;
    ESP_LOGW(TAG, "digitizer.cfg: %s", msg);
}

/* Read the GT911's current sensitivity config, then either CREATE digitizer.cfg
 * from those current values (when it doesn't exist) or APPLY the file's values
 * to the chip (when it does). Runs once at the end of input_init(): the chip is
 * probed + awake and, since no task has started yet, the display's shared SPI
 * bus is free for the SD mount. A missing card or unreadable chip just leaves
 * the panel at its factory config. */
static void gt911_sync_sensitivity(void)
{
    uint8_t block[GT911_CFG_LEN];
    if (gt911_read(GT911_CFG_BASE, block, GT911_CFG_LEN) != ESP_OK) {
        ESP_LOGW(TAG, "GT911 config read failed; leaving sensitivity at default");
        return;
    }
    gt911_sens_t cur;
    gt911_sens_from_block(block, &cur);

    static char text[1024];
    size_t len = 0;
    config_load_result_t r = config_read_file(DIGITIZER_CFG_PATH, text, sizeof(text), &len);

    if (r == CONFIG_NO_FILE) {
        /* First boot with this card: persist the current values so the user has
         * a documented file to edit. Do NOT reprogram the chip. */
        int n = gt911_sens_serialize(&cur, text, sizeof(text));
        if (n < 0) { ESP_LOGE(TAG, "digitizer.cfg serialize overflow"); return; }
        if (config_write_file(DIGITIZER_CFG_PATH, text, (size_t)n) == CONFIG_OK)
            ESP_LOGI(TAG, "created digitizer.cfg from current values "
                     "(touch=%u leave=%u noise=%u shake=%u filter=%u large=%u)",
                     cur.touch_level, cur.leave_level, cur.noise_reduction,
                     cur.shake_count, cur.filter, cur.large_touch);
        return;
    }
    if (r != CONFIG_OK)
        return;   /* no SD / read error — keep the factory config */

    gt911_sens_t want = cur;
    gt911_sens_parse(text, &want, gt911_cfg_log, NULL);
    if (memcmp(&want, &cur, sizeof want) == 0) {
        ESP_LOGI(TAG, "digitizer.cfg matches current config; no change");
        return;
    }

    /* Overlay the requested knobs onto the freshly-read block, recompute the
     * GT911 checksum, and set the config-fresh flag so the chip reloads. The
     * config-version byte (block[0]) is deliberately left as-read so the chip
     * treats this as a volatile session update re-applied every boot. */
    gt911_sens_to_block(&want, block);
    block[GT911_CFG_CKSUM] = gt911_cfg_checksum(block);
    block[GT911_CFG_FRESH] = 0x01;
    if (gt911_write_block(GT911_CFG_BASE, block, GT911_CFG_LEN) == ESP_OK)
        ESP_LOGI(TAG, "applied digitizer.cfg "
                 "(touch=%u leave=%u noise=%u shake=%u filter=%u large=%u)",
                 want.touch_level, want.leave_level, want.noise_reduction,
                 want.shake_count, want.filter, want.large_touch);
    else
        ESP_LOGE(TAG, "GT911 config write failed");
}

/* --- Public API --- */

esp_err_t input_init(void)
{
    ESP_RETURN_ON_ERROR(input_events_init(), TAG, "input queues");

#if CONFIG_BOARD_JC4827W543
    /* BOOT is active-low with an internal pull-up; GPIO38 is touch RESET,
     * GPIO39 is an LCD data line. Never configure those as M5 buttons. */
    gpio_config_t power = {
        .pin_bit_mask = 1ULL << PIN_BTN_M,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&power), TAG, "BOOT GPIO");
    /* GT911 reset: hold INT low while RESET rises to select address 0x5d. */
    gpio_set_direction(BOARD_TOUCH_RST, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_INT, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_TOUCH_RST, 0);
    gpio_set_level(PIN_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(60));
    gpio_set_direction(PIN_INT, GPIO_MODE_INPUT);
#endif
    i2c_master_bus_config_t bus_conf = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_conf, &gt911_bus), TAG, "I2C bus init failed");

    /* M5EPD library waits 100ms after Wire.begin() before first GT911 access */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* GT911 INT pin (input-only GPIO, no internal pull-up on 34-39) */
    gpio_config_t int_conf = {
        .pin_bit_mask = (1ULL << PIN_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_conf);

#if !CONFIG_BOARD_JC4827W543
    /* Hardware buttons (input-only GPIOs 37-39, external pull-ups on M5 Paper) */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << PIN_BTN_L) | (1ULL << PIN_BTN_M) | (1ULL << PIN_BTN_R),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

#endif
    /* Probe GT911 at primary address, then alternate */
    uint8_t product_id[4];
    if (gt911_use_addr(GT911_ADDR) == ESP_OK &&
        gt911_read(GT911_PRODUCT_REG, product_id, 4) == ESP_OK) {
        gt911_found = true;
    } else if (gt911_use_addr(GT911_ADDR_ALT) == ESP_OK &&
               gt911_read(GT911_PRODUCT_REG, product_id, 4) == ESP_OK) {
        gt911_found = true;
    }

    if (gt911_found) {
        ESP_LOGI(TAG, "GT911 at 0x%02X  product=%.4s", gt911_addr, product_id);

        /* Read firmware version */
        uint8_t fw_ver[2] = {0};
        gt911_read(GT911_FWVER_REG, fw_ver, 2);
        ESP_LOGI(TAG, "  FW version: %d", fw_ver[0] | (fw_ver[1] << 8));

        /* Read active resolution */
        uint8_t res[4] = {0};
        gt911_read(GT911_RESOL_REG, res, 4);
        ESP_LOGI(TAG, "  Resolution: %dx%d",
                 res[0] | (res[1] << 8), res[2] | (res[3] << 8));

        /* Read config header */
        uint8_t cfg[7] = {0};
        gt911_read(GT911_CFG_REG, cfg, 7);
        ESP_LOGI(TAG, "  Config: ver=%d x=%d y=%d touches=%d mod_sw1=0x%02X",
                 cfg[0],
                 cfg[1] | (cfg[2] << 8),
                 cfg[3] | (cfg[4] << 8),
                 cfg[5] & 0x0F,
                 cfg[6]);

        /* Ensure GT911 is in active coordinate-reading mode (not asleep) */
        gt911_write_byte(GT911_CMD_REG, 0x00);
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Clear status register */
        gt911_write_byte(GT911_STATUS_REG, 0x00);

        /* Create or apply the SD sensitivity config (digitizer.cfg) now that the
         * chip is probed + awake and the shared display SPI bus is still free. */
        gt911_sync_sensitivity();
    } else {
        ESP_LOGW(TAG, "GT911 not found");
    }

    ESP_LOGI(TAG, "Input ready (touch=%s)", gt911_found ? "yes" : "no");
    return ESP_OK;
}

#if !CONFIG_BOARD_JC4827W543
#define BR_HOLD_MS   1000     /* hold >= this -> backup/restore request */
#define TAP_PULSE_MS 60       /* a deferred tap holds the page key this long */

/* Release the deferred-tap page key once its short pulse has elapsed. Called
 * every input_poll() cycle. */
static void tap_pulse_service(void)
{
    if (tap_row >= 0 && (int32_t)(xTaskGetTickCount() - tap_pulse_until) >= 0) {
        db_kbd_set(tap_row, tap_col, false);
        tap_row = -1;
    }
}

/* Deferred-tap + long-hold handling for one UP/DOWN side button. The page key is
 * NOT asserted on press (unlike a plain edge->matrix mapping): a short tap
 * (< BR_HOLD_MS) emits a single page key on release — held TAP_PULSE_MS so the
 * ROM's matrix scan catches it — while a >= BR_HOLD_MS hold latches a backup/
 * restore request instead and suppresses the tap. Mirrors the middle-button
 * hold. `row,col` is the page key (KBD_KEY_PGUP/PGDN expands to both args). */
static void handle_hold_button(bool pressed, bool *last, TickType_t *press_tick,
                               bool *hold_fired, int row, int col,
                               input_br_req_t req)
{
    TickType_t now = xTaskGetTickCount();
    if (pressed && !*last) {                  /* press edge: start timing, defer the key */
        *press_tick = now;
        *hold_fired = false;
    } else if (pressed && *last) {            /* held */
        if (!*hold_fired &&
            (now - *press_tick) >= pdMS_TO_TICKS(BR_HOLD_MS)) {
            br_req_pending = req;
            *hold_fired = true;
            ESP_LOGI(TAG, "%s (hold) requested",
                     req == INPUT_BR_BACKUP ? "BACKUP" : "RESTORE");
        }
    } else if (!pressed && *last) {           /* release edge */
        if (!*hold_fired) {                   /* short tap -> one page key pulse */
            /* The pulse slot is shared by UP and DOWN (different cols, same row); if a
             * prior tap's pulse is still in flight, release ITS key before claiming the
             * slot, or that key would stay asserted forever (only the last-claimed col
             * is ever cleared by tap_pulse_service). */
            if (tap_row >= 0)
                db_kbd_set(tap_row, tap_col, false);
            db_kbd_set(row, col, true);
            tap_row = row; tap_col = col;
            tap_pulse_until = now + pdMS_TO_TICKS(TAP_PULSE_MS);
        }
    }
    *last = pressed;
}

#endif

void input_poll(void)
{
    /* --- Touch --- */
    if (gt911_found) {
        uint8_t status = 0;
        esp_err_t err = gt911_read(GT911_STATUS_REG, &status, 1);

        /* Periodic diagnostic logging (every ~5 s at the 100 Hz poll rate) */
        if (poll_diag_count++ % 500 == 0)
            ESP_LOGI(TAG, "poll: err=%d status=0x%02X int=%d",
                     err, status, gpio_get_level(PIN_INT));

        if (err == ESP_OK && (status & 0x80)) {
            uint8_t num_fingers = status & 0x0F;

            if (num_fingers > 0) {
                uint8_t data[8];
                if (gt911_read(GT911_TOUCH_REG, data, 8) == ESP_OK) {
                    /* Step 1 — GT911 raw -> native IT8951 panel coords. This is
                     * a FIXED hardware relation (independent of how the device
                     * is held). GT911 reports X=0-539 (short), Y=0-959 (long);
                     * the panel is addressed landscape (eink_x=0-959 along the
                     * long edge, eink_y=0-539). The GT911 short axis (gt_x) runs
                     * opposite the panel Y axis, so invert it. */
                    uint16_t gt_x = ((uint16_t)data[1] << 8) | data[0];
                    uint16_t gt_y = ((uint16_t)data[3] << 8) | data[2];

#if CONFIG_BOARD_JC4827W543
                    int eink_x = (int)gt_x;
                    int eink_y = (int)gt_y;
#else
                    int eink_x = (int)gt_y;
                    int eink_y = (layout_panel_h() - 1) - (int)gt_x;
#endif

                    /* Step 2 — native panel pixel -> Palm coords via the active
                     * layout (the same transform display.c renders through, so
                     * a tap lands on whatever was drawn there, for any rotation/
                     * placement). Returns false when the touch is in dead space
                     * (no region) — treated like a pen-up so a stroke that
                     * leaves a region ends cleanly. palm_y<160 = screen,
                     * 160..219 = silkscreen; both inject via the same pen path
                     * and the ROM dispatches. */
                    int palm_x, palm_y;
                    if (layout_panel_to_palm(eink_x, eink_y, &palm_x, &palm_y)) {
                        /* Stream the stroke: pen-down on the press transition,
                         * then a fresh point whenever the position changes.
                         * Graffiti recognition needs the whole path — the ROM
                         * collector samples the live pen point until pen-up. A
                         * still tap produces no moves, so taps and silkscreen
                         * buttons behave exactly as before (single down + up). */
                        if (!last_pen_down) {
                            input_inject_pen((int16_t)palm_x, (int16_t)palm_y, false);
                            ESP_LOGI(TAG, "pen DOWN gt=%d,%d eink=%d,%d palm=%d,%d",
                                     gt_x, gt_y, eink_x, eink_y, palm_x, palm_y);
                            last_pen_down = true;
                        } else if (palm_x != last_px || palm_y != last_py) {
                            input_inject_pen((int16_t)palm_x, (int16_t)palm_y, false);
                        }
                        last_px = (int16_t)palm_x;
                        last_py = (int16_t)palm_y;
                    } else if (last_pen_down) {
                        input_inject_pen(-1, -1, true);
                        last_pen_down = false;
                        last_px = last_py = -1;
                    }
                }
            } else {
                if (last_pen_down) {
                    input_inject_pen(-1, -1, true);
                    last_pen_down = false;
                    last_px = last_py = -1;
                }
            }

            gt911_write_byte(GT911_STATUS_REG, 0x00);
        }
    }

    /* --- Hardware buttons (active low). UP/DOWN (Left/Right) tap = Palm page
     * up/down; a >= 1s hold latches a backup (UP) / restore (DOWN) request that
     * the CPU task confirms with an on-panel countdown (see main.c). Holding no
     * longer auto-repeat-scrolls — the page key is deferred to the tap-release.
     * MIDDLE is the POWER (sleep/wake) button: a hold of >= 1s while awake
     * injects vchrHardPower so the running OS sleeps. There is no Home mapping
     * (Home is reached by tapping the on-screen silkscreen icon). Waking from
     * sleep is handled by the ESP32 light-sleep GPIO wake + a >=1s re-hold check
     * in the CPU task (main.c), which then pulses the matrix POWER key — while
     * asleep this input task isn't running, so no asleep branch is needed. */
    bool btn_m = !gpio_get_level(PIN_BTN_M);
#if !CONFIG_BOARD_JC4827W543
    bool btn_l = !gpio_get_level(PIN_BTN_L);
    bool btn_r = !gpio_get_level(PIN_BTN_R);

    handle_hold_button(btn_l, &last_btn_l, &l_press_tick, &l_hold_fired,
                       KBD_KEY_PGUP, INPUT_BR_BACKUP);

#endif

    /* Middle button: awake long-hold (>= 1s) = Power -> sleep. vchrHardPower is
     * delivered via the EvtEnqueueKey syscall (the running event loop ignores
     * the matrix->event path for power). */
    #define MID_POWER_HOLD_MS 1000
    static TickType_t mid_press_tick = 0;
    static bool mid_power_fired = false;   /* this press already produced a POWER action */
    if (btn_m && !last_btn_m) {                 /* press edge */
        mid_press_tick = xTaskGetTickCount();
        mid_power_fired = false;
    } else if (btn_m && last_btn_m) {           /* held */
        if (!mid_power_fired &&
            (xTaskGetTickCount() - mid_press_tick) >= pdMS_TO_TICKS(MID_POWER_HOLD_MS)) {
            power_key_pending = true;
            mid_power_fired = true;
            ESP_LOGI(TAG, "POWER (hold) -> vchrHardPower");
        }
    }
    last_btn_m = btn_m;

#if !CONFIG_BOARD_JC4827W543
    handle_hold_button(btn_r, &last_btn_r, &r_press_tick, &r_hold_fired,
                       KBD_KEY_PGDN, INPUT_BR_RESTORE);

    tap_pulse_service();
#endif
}

/* The GT911 path reports touches through the Palm layout only. */
bool input_touch_panel(int *x, int *y) { (void)x; (void)y; return false; }
touch_cal_source_t input_touch_load_saved(void) { return TOUCH_CAL_DEFAULT; }
bool input_touch_raw(int *raw_x, int *raw_y) { (void)raw_x; (void)raw_y; return false; }
bool input_touch_calibration_try(const int raw[4][2], int x0, int y0, int x1, int y1)
{
    (void)raw; (void)x0; (void)y0; (void)x1; (void)y1;
    return false;
}
void input_touch_calibration_commit(bool keep) { (void)keep; }
bool input_boot_button_held(void) { return !gpio_get_level(PIN_BTN_M); }
