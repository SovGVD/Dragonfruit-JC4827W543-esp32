/* Boot menu: choose which emulated system starts from the SD card.
 *
 * Each system owns one top-level folder (PALM_SD_DIR, CYBIKO_SD_DIR). The menu
 * shows both, marks which ones have their ROM files, and starts the chosen one.
 * Tap a card to start it. The BOOT button cycles the selection with a short
 * press and starts it with a one-second hold. The last system started is kept
 * in NVS and starts by itself after a short countdown unless the user touches
 * the screen or the button first. */
#include "boot_menu.h"
#include "board.h"
#include "display_ui.h"
#include "input.h"
#include "m5_sd.h"
#include "cybiko_app.h"
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

static void text_centered(int cx, int y, const char *s, int scale, uint16_t fg, uint16_t bg)
{
    display_ui_text(cx - display_ui_text_width(s, scale) / 2, y, s, scale, fg, bg);
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
    text_centered(cx, c->y + 66, c->folder, 2, COLOR_DIM, c->color);
    text_centered(cx, c->y + 98, c->ready ? "READY" : c->missing, 2,
                  c->ready ? COLOR_OK : COLOR_BAD, c->color);
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

static void draw_all(int selected, int countdown)
{
    display_ui_fill(0, 0, BOARD_PANEL_W, BOARD_PANEL_H, COLOR_BG);
    text_centered(BOARD_PANEL_W / 2, 10, "DRAGONFRUIT", 3, COLOR_TITLE, COLOR_BG);
    text_centered(BOARD_PANEL_W / 2, 40, "CHOOSE A SYSTEM", 2, COLOR_DIM, COLOR_BG);
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
    cards[BOOT_SYSTEM_CYBIKO].ready = cybiko_app_find_files(NULL, NULL, 0);
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
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        TickType_t now = xTaskGetTickCount();

        int x, y;
        bool down = input_touch_panel(&x, &y);
        if (down) {
            countdown = 0;
            int hit = card_at(x, y);
            if (!touching) touched = hit;
            if (hit >= 0 && hit != selected) {
                draw_card(selected, false);
                selected = hit;
                draw_card(selected, true);
            }
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
