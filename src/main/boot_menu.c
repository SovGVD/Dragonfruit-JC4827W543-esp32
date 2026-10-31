/* Boot menu: choose which emulated system starts from the SD card.
 *
 * Each system owns one top-level folder (PALM_SD_DIR, CYBIKO_SD_DIR). The menu
 * shows both, marks which ones have their ROM files, and starts the chosen one.
 * Tap a card to start it. The BOOT button cycles the selection with a short
 * press and starts it with a one-second hold. The last system started is kept
 * in NVS and starts by itself after a short countdown unless the user touches
 * the screen or the button first.
 *
 * A row of settings under the title: the Cybiko screen orientation
 * (portrait by default, or landscape), the speaker volume both systems start
 * with (kept in NVS, played as a short beep when changed) and CALIBRATE.
 * The Cybiko card has three buttons of its own: one takes the MP3 cartridge
 * out or puts it back (some games will not start with it in), one turns
 * whole frames ("vsync": the screen is shown only once CyOS has finished
 * drawing it) on or off, and one picks the screen's colours: the green
 * tint of the real LCD or plain greys. A fourth, in place of the folder
 * name, picks the model: the Xtreme (files in /Cybiko) or the Classic
 * (files in /Cybiko/Classic); the card shows whether that model's files are
 * there. The Classic has no MP3 cartridge, so its MP3 button is dimmed.
 *
 * The CALIBRATE button opens the touch calibration. It also opens by itself
 * when no calibration has been made yet (none saved, none in palm.cfg), and
 * when BOOT is held for two seconds as the menu appears, for a panel so far
 * off that the button can't be hit. */
#include "boot_menu.h"
#include "board.h"
#include "display_ui.h"
#include "input.h"
#include "m5_sd.h"
#include "cybiko_app.h"
#include "touch_calib.h"
#include "audio.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "boot_menu";

#define AUTOBOOT_SECONDS 5
#define HOLD_MS 1000
#define CALIB_HOLD_MS 2000

#define COLOR_BG     UI_RGB(0x14, 0x14, 0x1c)
#define COLOR_TITLE  UI_RGB(0xff, 0xff, 0xff)
#define COLOR_DIM    UI_RGB(0x90, 0x90, 0xa0)
#define COLOR_OK     UI_RGB(0x60, 0xe0, 0x60)
#define COLOR_BAD    UI_RGB(0xff, 0x60, 0x50)
#define COLOR_SELECT UI_RGB(0xff, 0xd0, 0x30)

typedef struct {
    const char *name, *folder, *missing;
    uint16_t color;
    bool ready;
    int x, y, w, h;
} card_t;

static card_t cards[BOOT_SYSTEM_COUNT] = {
    [BOOT_SYSTEM_PALM]   = { "PALM OS", "/PALMOS", "NO PALM.ROM", UI_RGB(0x2c, 0x5a, 0x34) },
    [BOOT_SYSTEM_CYBIKO] = { "CYBIKO",  "/CYBIKO", "NO ROM/FLASH", UI_RGB(0x28, 0x40, 0x78) },
};

/* The settings row: orientation, volume - and +, CALIBRATE. */
#define ROW_Y 34
#define CAL_X 384
#define CAL_Y ROW_Y
#define CAL_W 90
#define CAL_H 28
#define COLOR_BUTTON UI_RGB(0x3a, 0x3a, 0x4c)
#define ORI_X 6
#define ORI_Y ROW_Y
#define ORI_W 120
#define ORI_H 28
#define VOL_DN_X 150
#define VOL_UP_X 294
#define VOL_W 36
#define VOL_STEP 10
#define VOLUME_DEFAULT 60
/* The Cybiko card's buttons, side by side near its bottom, centred: the MP3
 * cartridge, VSYNC and the screen palette. */
enum { CY_BTN_CART, CY_BTN_VSYNC, CY_BTN_PALETTE, CY_BTN_COUNT };
#define CY_BTN_W 64
#define CY_BTN_H 24
#define CY_BTN_DY 120
#define CY_BTN_GAP 3
/* The model button, where the other card shows its folder. */
#define CY_MODEL_W 150
#define CY_MODEL_H 24
#define CY_MODEL_DY 60

static bool cy_ready_xtreme, cy_ready_classic;

static void text_centered(int cx, int y, const char *s, int scale, uint16_t fg, uint16_t bg)
{
    display_ui_text(cx - display_ui_text_width(s, scale) / 2, y, s, scale, fg, bg);
}

/* Left edge of the Cybiko card's button `b`. */
static int cy_button_x(int b)
{
    const card_t *c = &cards[BOOT_SYSTEM_CYBIKO];
    const int row = CY_BTN_COUNT * CY_BTN_W + (CY_BTN_COUNT - 1) * CY_BTN_GAP;
    return c->x + (c->w - row) / 2 + b * (CY_BTN_W + CY_BTN_GAP);
}

static void draw_card(int i, bool selected)
{
    const card_t *c = &cards[i];
    const int b = 4, cx = c->x + c->w / 2;
    uint16_t edge = selected ? COLOR_SELECT : c->color;
    display_ui_fill(c->x, c->y, c->w, b, edge);
    display_ui_fill(c->x, c->y + c->h - b, c->w, b, edge);
    display_ui_fill(c->x, c->y, b, c->h, edge);
    display_ui_fill(c->x + c->w - b, c->y, b, c->h, edge);
    display_ui_fill(c->x + b, c->y + b, c->w - 2 * b, c->h - 2 * b, c->color);
    text_centered(cx, c->y + 24, c->name, 4, COLOR_TITLE, c->color);
    if (i == BOOT_SYSTEM_CYBIKO) {
        display_ui_fill(cx - CY_MODEL_W / 2, c->y + CY_MODEL_DY, CY_MODEL_W, CY_MODEL_H, COLOR_BUTTON);
        text_centered(cx, c->y + CY_MODEL_DY + 5, cybiko_app_classic() ? "CLASSIC" : "XTREME", 2,
                      COLOR_TITLE, COLOR_BUTTON);
    } else {
        text_centered(cx, c->y + 66, c->folder, 2, COLOR_DIM, c->color);
    }
    text_centered(cx, c->y + 98, c->ready ? "READY" : c->missing, 2,
                  c->ready ? COLOR_OK : COLOR_BAD, c->color);
    if (i == BOOT_SYSTEM_CYBIKO) {
        static const char *const palette_label[CY_PALETTE_COUNT] = { "LCD: GREEN", "LCD: GRAY", "HALLOWEEN" };
        const char *label[CY_BTN_COUNT] = {
            [CY_BTN_CART] = cybiko_app_classic() ? "MP3: --" : cybiko_app_cart_in() ? "MP3: IN" : "MP3: OUT",
            [CY_BTN_VSYNC] = cybiko_app_vsync() ? "VSYNC: ON" : "VSYNC: OFF",
            [CY_BTN_PALETTE] = palette_label[cybiko_app_palette()],
        };
        for (int b = 0; b < CY_BTN_COUNT; b++) {
            int x0 = cy_button_x(b);
            display_ui_fill(x0, c->y + CY_BTN_DY, CY_BTN_W, CY_BTN_H, COLOR_BUTTON);
            text_centered(x0 + CY_BTN_W / 2, c->y + CY_BTN_DY + 8, label[b], 1, COLOR_TITLE, COLOR_BUTTON);
        }
    }
}

static void draw_footer(int countdown)
{
    char line[48];
    display_ui_fill(0, 230, BOARD_PANEL_W, 42, COLOR_BG);
    if (countdown > 0)
        snprintf(line, sizeof line, "STARTING IN %d - TAP TO CHOOSE", countdown);
    else
        snprintf(line, sizeof line, "TAP A SYSTEM TO START");
    text_centered(BOARD_PANEL_W / 2, 232, line, 2, COLOR_TITLE, COLOR_BG);
    text_centered(BOARD_PANEL_W / 2, 256, "BOOT KEY: PRESS = NEXT, HOLD = START", 1,
                  COLOR_DIM, COLOR_BG);
}

static void draw_orientation(void)
{
    display_ui_fill(ORI_X, ORI_Y, ORI_W, ORI_H, COLOR_BUTTON);
    text_centered(ORI_X + ORI_W / 2, ORI_Y + 10,
                  cybiko_app_portrait() ? "CYBIKO: PORTRAIT" : "CYBIKO: LANDSCAPE", 1,
                  COLOR_TITLE, COLOR_BUTTON);
}

static uint8_t volume = VOLUME_DEFAULT;

static void draw_volume(void)
{
    char line[16];
    display_ui_fill(VOL_DN_X, ROW_Y, VOL_W, ORI_H, COLOR_BUTTON);
    display_ui_fill(VOL_UP_X, ROW_Y, VOL_W, ORI_H, COLOR_BUTTON);
    text_centered(VOL_DN_X + VOL_W / 2, ROW_Y + 7, "-", 2, COLOR_TITLE, COLOR_BUTTON);
    text_centered(VOL_UP_X + VOL_W / 2, ROW_Y + 7, "+", 2, COLOR_TITLE, COLOR_BUTTON);
    int x0 = VOL_DN_X + VOL_W, w = VOL_UP_X - x0;
    display_ui_fill(x0, ROW_Y, w, ORI_H, COLOR_BG);
    if (volume) snprintf(line, sizeof line, "VOLUME %u%%", volume);
    else snprintf(line, sizeof line, "SOUND OFF");
    text_centered(x0 + w / 2, ROW_Y + 10, line, 1, COLOR_TITLE, COLOR_BG);
}

/* Step the volume by `delta`, keep it and let the user hear it. */
static void change_volume(int delta)
{
    int v = volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    volume = (uint8_t)v;
    audio_save_volume(volume);
    draw_volume();
    if (volume && audio_init(volume) == ESP_OK) {
        audio_tone(1000.0f, 0.5f);
        vTaskDelay(pdMS_TO_TICKS(120));
        audio_tone(0, 0);
    }
}

static int volume_button_at(int x, int y)
{
    if (y < ROW_Y || y >= ROW_Y + ORI_H) return 0;
    if (x >= VOL_DN_X && x < VOL_DN_X + VOL_W) return -VOL_STEP;
    if (x >= VOL_UP_X && x < VOL_UP_X + VOL_W) return VOL_STEP;
    return 0;
}

static void draw_all(int selected, int countdown)
{
    display_ui_fill(0, 0, BOARD_PANEL_W, BOARD_PANEL_H, COLOR_BG);
    text_centered(BOARD_PANEL_W / 2, 10, "DRAGONFRUIT", 3, COLOR_TITLE, COLOR_BG);
    draw_orientation();
    draw_volume();
    display_ui_fill(CAL_X, CAL_Y, CAL_W, CAL_H, COLOR_BUTTON);
    text_centered(CAL_X + CAL_W / 2, CAL_Y + 10, "CALIBRATE", 1, COLOR_TITLE, COLOR_BUTTON);
    for (int i = 0; i < BOOT_SYSTEM_COUNT; i++)
        draw_card(i, i == selected);
    draw_footer(countdown);
}

static int load_last(void)
{
    nvs_handle_t h;
    uint8_t v = BOOT_SYSTEM_PALM;
    if (nvs_open("boot", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "last", &v);
        nvs_close(h);
    }
    return v < BOOT_SYSTEM_COUNT ? v : BOOT_SYSTEM_PALM;
}

static void save_last(int system)
{
    nvs_handle_t h;
    if (nvs_open("boot", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "last", (uint8_t)system);
    nvs_commit(h);
    nvs_close(h);
}

static bool calibrate_at(int x, int y)
{
    return x >= CAL_X && x < CAL_X + CAL_W && y >= CAL_Y && y < CAL_Y + CAL_H;
}

static bool orientation_at(int x, int y)
{
    return x >= ORI_X && x < ORI_X + ORI_W && y >= ORI_Y && y < ORI_Y + ORI_H;
}

static bool cy_model_at(int x, int y)
{
    const card_t *c = &cards[BOOT_SYSTEM_CYBIKO];
    int x0 = c->x + c->w / 2 - CY_MODEL_W / 2, y0 = c->y + CY_MODEL_DY;
    return x >= x0 && x < x0 + CY_MODEL_W && y >= y0 && y < y0 + CY_MODEL_H;
}

static void cy_update_ready(void)
{
    card_t *c = &cards[BOOT_SYSTEM_CYBIKO];
    c->ready = cybiko_app_classic() ? cy_ready_classic : cy_ready_xtreme;
    c->missing = cybiko_app_classic() ? "NO CLASSIC FILES" : "NO ROM/FLASH";
}

/* The Cybiko card's button under (x, y), or -1. */
static int cy_button_at(int x, int y)
{
    int y0 = cards[BOOT_SYSTEM_CYBIKO].y + CY_BTN_DY;
    if (y < y0 || y >= y0 + CY_BTN_H) return -1;
    for (int b = 0; b < CY_BTN_COUNT; b++)
        if (x >= cy_button_x(b) && x < cy_button_x(b) + CY_BTN_W) return b;
    return -1;
}

static int card_at(int x, int y)
{
    for (int i = 0; i < BOOT_SYSTEM_COUNT; i++) {
        const card_t *c = &cards[i];
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) return i;
    }
    return -1;
}

static bool scan_card(void)
{
    sdmmc_card_t *card = NULL;
    if (m5_sd_mount(&card) != ESP_OK) return false;
    struct stat st;
    cards[BOOT_SYSTEM_PALM].ready = stat(PALM_SD_DIR "/palm.rom", &st) == 0;
    cy_ready_xtreme = cybiko_app_find_files(NULL, NULL, 0);
    cy_ready_classic = cybiko_app_find_classic(NULL, NULL, 0);
    cy_update_ready();
    m5_sd_unmount(card);
    return true;
}

boot_system_t boot_menu_run(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    uint8_t saved_volume;
    if (audio_saved_volume(&saved_volume)) volume = saved_volume;
    bool calibrate = input_touch_load_saved() == TOUCH_CAL_DEFAULT;
    if (!calibrate && input_boot_button_held()) {
        TickType_t t0 = xTaskGetTickCount();
        while (input_boot_button_held() && !calibrate) {
            vTaskDelay(pdMS_TO_TICKS(20));
            calibrate = xTaskGetTickCount() - t0 >= pdMS_TO_TICKS(CALIB_HOLD_MS);
        }
    }
    if (calibrate) touch_calib_run();

    /* Without a card the Palm path shows the "no SD card" screen. */
    if (!scan_card()) return BOOT_SYSTEM_PALM;

    const int gap = 20, w = (BOARD_PANEL_W - 3 * gap) / 2;
    for (int i = 0; i < BOOT_SYSTEM_COUNT; i++) {
        cards[i].x = gap + i * (w + gap);
        cards[i].y = 68;
        cards[i].w = w;
        cards[i].h = 150;
    }

    int selected = load_last();
    if (!cards[selected].ready)
        selected = cards[BOOT_SYSTEM_PALM].ready ? BOOT_SYSTEM_PALM :
                   cards[BOOT_SYSTEM_CYBIKO].ready ? BOOT_SYSTEM_CYBIKO : selected;
    int countdown = cards[selected].ready ? AUTOBOOT_SECONDS : 0;
    draw_all(selected, countdown);
    ESP_LOGI(TAG, "Palm %s, Cybiko %s, selected %s",
             cards[BOOT_SYSTEM_PALM].ready ? "ready" : "missing",
             cards[BOOT_SYSTEM_CYBIKO].ready ? "ready" : "missing", cards[selected].name);

    TickType_t start = xTaskGetTickCount(), pressed_at = 0;
    bool touching = false, was_pressed = input_boot_button_held(), held_fired = false;
    int touched = -1;
    bool cal_touched = false, ori_touched = false;
    int vol_touched = 0, cy_touched = -1;
    bool model_touched = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        TickType_t now = xTaskGetTickCount();

        int x, y;
        bool down = input_touch_panel(&x, &y);
        if (down) {
            countdown = 0;
            int hit = card_at(x, y);
            if (!touching) {
                touched = hit;
                cal_touched = calibrate_at(x, y);
                ori_touched = orientation_at(x, y);
                vol_touched = volume_button_at(x, y);
                cy_touched = cy_button_at(x, y);
                model_touched = cy_model_at(x, y);
            }
            if (hit >= 0 && hit != selected) {
                draw_card(selected, false);
                selected = hit;
                draw_card(selected, true);
            }
        } else if (touching && vol_touched) {
            change_volume(vol_touched);
            draw_footer(0);
        } else if (touching && model_touched) {
            cybiko_app_set_classic(!cybiko_app_classic());
            cy_update_ready();
            draw_card(BOOT_SYSTEM_CYBIKO, selected == BOOT_SYSTEM_CYBIKO);
            draw_footer(0);
        } else if (touching && cy_touched >= 0) {
            if (cy_touched == CY_BTN_CART) {
                if (!cybiko_app_classic()) cybiko_app_set_cart_in(!cybiko_app_cart_in());
            } else if (cy_touched == CY_BTN_VSYNC) cybiko_app_set_vsync(!cybiko_app_vsync());
            else cybiko_app_set_palette((cybiko_app_palette() + 1) % CY_PALETTE_COUNT);
            draw_card(BOOT_SYSTEM_CYBIKO, selected == BOOT_SYSTEM_CYBIKO);
            draw_footer(0);
        } else if (touching && ori_touched) {
            cybiko_app_set_portrait(!cybiko_app_portrait());
            draw_orientation();
            draw_footer(0);
        } else if (touching && cal_touched) {
            touch_calib_run();
            draw_all(selected, 0);
            was_pressed = false;
        } else if (touching) {
            /* Start on release over the card the touch began on. */
            if (touched >= 0 && touched == selected && cards[selected].ready) break;
            draw_footer(0);
        }
        touching = down;

        bool pressed = input_boot_button_held();
        if (pressed && !was_pressed) {
            pressed_at = now;
            held_fired = false;
            countdown = 0;
            draw_footer(0);
        } else if (pressed && !held_fired && now - pressed_at >= pdMS_TO_TICKS(HOLD_MS)) {
            held_fired = true;
            if (cards[selected].ready) break;
        } else if (!pressed && was_pressed && !held_fired) {
            draw_card(selected, false);
            selected = (selected + 1) % BOOT_SYSTEM_COUNT;
            draw_card(selected, true);
        }
        was_pressed = pressed;

        if (countdown > 0) {
            int left = AUTOBOOT_SECONDS - (int)((now - start) / pdMS_TO_TICKS(1000));
            if (left <= 0) break;
            if (left != countdown) draw_footer(countdown = left);
        }
    }

    /* Let go of the BOOT button before the chosen system starts sampling it. */
    while (input_boot_button_held()) vTaskDelay(pdMS_TO_TICKS(20));
    save_last(selected);
    display_ui_fill(0, 0, BOARD_PANEL_W, BOARD_PANEL_H, COLOR_BG);
    text_centered(BOARD_PANEL_W / 2, 120, "STARTING", 3, COLOR_TITLE, COLOR_BG);
    text_centered(BOARD_PANEL_W / 2, 156, cards[selected].name, 3, COLOR_SELECT, COLOR_BG);
    ESP_LOGI(TAG, "Starting %s", cards[selected].name);
    return (boot_system_t)selected;
}
