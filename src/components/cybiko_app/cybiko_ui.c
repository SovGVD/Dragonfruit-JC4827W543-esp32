/* Cybiko screen and on-screen keyboard on the 480x272 panel.
 *
 *   +--------------------------+----------------+
 *   |                          | ESC   UP  HELP |
 *   |  Cybiko LCD 160x100 x2   | LEFT SEL RIGHT |
 *   |        (320x200)         | TAB  DOWN  DEL |
 *   |                          | ENTER   SPACE  |
 *   +--------------------------+----------------+
 *   | Q  W  E  R  T  Y  U  I  O  P              |
 *   | A  S  D  F  G  H  J  K  L  .              |
 *   | SHIFT Z X  C  V  B  N  M  FN  MORE        |
 *   +--------------------------------------------+
 *
 * MORE swaps the bottom block to F1-F7, AS and ON/OFF. Touch is single
 * point, so SHIFT and FN latch: they apply to the next key and release
 * with it. The BOOT button is the On/Off key; holding it 3 s saves to SD
 * and restarts to the boot menu. */
#include <string.h>
#include "cybiko_ui.h"
#include "display_ui.h"
#include "input.h"
#include "board.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_X 0
#define LCD_Y 0
#define LCD_SCALE 2
#define PAD_X 320
#define PAD_W 160
#define KB_Y 200
#define KB_ROW_H 24
#define KB_COLS 10
#define KB_KEY_W 48

#define COL_BG      UI_RGB(0x20, 0x24, 0x2c)
#define COL_KEY     UI_RGB(0x4a, 0x50, 0x5c)
#define COL_KEY_ALT UI_RGB(0x34, 0x3a, 0x46)
#define COL_KEY_ON  UI_RGB(0xe0, 0x9a, 0x2a)
#define COL_EDGE    UI_RGB(0x12, 0x14, 0x18)
#define COL_LABEL   UI_RGB(0xf0, 0xf0, 0xf0)

enum { K_NORMAL, K_SHIFT, K_FN, K_PAGE, K_NONE };

typedef struct {
    int16_t x, y, w, h;
    int16_t key;
    uint8_t kind;
    bool alt;            /* drawn in the darker colour */
    const char *label;
} ui_key_t;

#define PAD_KEY(col, row, k, l) { PAD_X + (col) * 53, (row) * 50, (col) == 2 ? 54 : 53, 50, k, K_NORMAL, true, l }
static const ui_key_t pad_keys[] = {
    PAD_KEY(0, 0, CYK_ESC, "ESC"), PAD_KEY(1, 0, CYK_UP, "UP"), PAD_KEY(2, 0, CYK_HELP, "HELP"),
    PAD_KEY(0, 1, CYK_LEFT, "LEFT"), PAD_KEY(1, 1, CYK_SELECT, "SEL"), PAD_KEY(2, 1, CYK_RIGHT, "RIGHT"),
    PAD_KEY(0, 2, CYK_TAB, "TAB"), PAD_KEY(1, 2, CYK_DOWN, "DOWN"), PAD_KEY(2, 2, CYK_DEL, "DEL"),
    { PAD_X, 150, 80, 50, CYK_ENTER, K_NORMAL, true, "ENTER" },
    { PAD_X + 80, 150, 80, 50, CYK_SPACE, K_NORMAL, true, "SPACE" },
};

#define KB(col, row, k, kind, l) { (col) * KB_KEY_W, KB_Y + (row) * KB_ROW_H, KB_KEY_W, KB_ROW_H, k, kind, false, l }
static const ui_key_t page_letters[] = {
    KB(0, 0, CYK_Q, K_NORMAL, "Q"), KB(1, 0, CYK_W, K_NORMAL, "W"), KB(2, 0, CYK_E, K_NORMAL, "E"),
    KB(3, 0, CYK_R, K_NORMAL, "R"), KB(4, 0, CYK_T, K_NORMAL, "T"), KB(5, 0, CYK_Y, K_NORMAL, "Y"),
    KB(6, 0, CYK_U, K_NORMAL, "U"), KB(7, 0, CYK_I, K_NORMAL, "I"), KB(8, 0, CYK_O, K_NORMAL, "O"),
    KB(9, 0, CYK_P, K_NORMAL, "P"),
    KB(0, 1, CYK_A, K_NORMAL, "A"), KB(1, 1, CYK_S, K_NORMAL, "S"), KB(2, 1, CYK_D, K_NORMAL, "D"),
    KB(3, 1, CYK_F, K_NORMAL, "F"), KB(4, 1, CYK_G, K_NORMAL, "G"), KB(5, 1, CYK_H, K_NORMAL, "H"),
    KB(6, 1, CYK_J, K_NORMAL, "J"), KB(7, 1, CYK_K, K_NORMAL, "K"), KB(8, 1, CYK_L, K_NORMAL, "L"),
    KB(9, 1, CYK_DOT, K_NORMAL, "."),
    KB(0, 2, CYK_SHIFT, K_SHIFT, "SHIFT"), KB(1, 2, CYK_Z, K_NORMAL, "Z"), KB(2, 2, CYK_X, K_NORMAL, "X"),
    KB(3, 2, CYK_C, K_NORMAL, "C"), KB(4, 2, CYK_V, K_NORMAL, "V"), KB(5, 2, CYK_B, K_NORMAL, "B"),
    KB(6, 2, CYK_N, K_NORMAL, "N"), KB(7, 2, CYK_M, K_NORMAL, "M"), KB(8, 2, CYK_FN, K_FN, "FN"),
    KB(9, 2, -1, K_PAGE, "MORE"),
};
static const ui_key_t page_more[] = {
    KB(0, 0, CYK_F1, K_NORMAL, "F1"), KB(1, 0, CYK_F2, K_NORMAL, "F2"), KB(2, 0, CYK_F3, K_NORMAL, "F3"),
    KB(3, 0, CYK_F4, K_NORMAL, "F4"), KB(4, 0, CYK_F5, K_NORMAL, "F5"), KB(5, 0, CYK_F6, K_NORMAL, "F6"),
    KB(6, 0, CYK_F7, K_NORMAL, "F7"), KB(7, 0, CYK_AS, K_NORMAL, "AS"),
    { 8 * KB_KEY_W, KB_Y, 2 * KB_KEY_W, KB_ROW_H, CYK_ONOFF, K_NORMAL, false, "ON/OFF" },
    { 0, KB_Y + KB_ROW_H, 10 * KB_KEY_W, KB_ROW_H, -1, K_NONE, true, "" },
    KB(0, 2, CYK_SHIFT, K_SHIFT, "SHIFT"),
    { KB_KEY_W, KB_Y + 2 * KB_ROW_H, 7 * KB_KEY_W, KB_ROW_H, -1, K_NONE, true, "" },
    KB(8, 2, CYK_FN, K_FN, "FN"), KB(9, 2, -1, K_PAGE, "ABC"),
};

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

static int page;                     /* 0 letters, 1 more */
static bool shift_on, fn_on;
static const ui_key_t *held;         /* key whose press is in progress */
static int64_t held_since;

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_key(const ui_key_t *k, bool lit)
{
    uint16_t bg = lit ? COL_KEY_ON : k->alt ? COL_KEY_ALT : COL_KEY;
    if (k->kind == K_SHIFT && shift_on) bg = COL_KEY_ON;
    if (k->kind == K_FN && fn_on) bg = COL_KEY_ON;
    if (k->kind == K_NONE) bg = COL_BG;
    display_ui_fill(k->x, k->y, k->w, k->h, COL_EDGE);
    display_ui_fill(k->x + 1, k->y + 1, k->w - 2, k->h - 2, bg);
    if (!k->label[0]) return;
    int scale = display_ui_text_width(k->label, 2) <= k->w - 6 && k->h >= 20 ? 2 : 1;
    int tw = display_ui_text_width(k->label, scale) - scale;
    int th = UI_FONT_H * scale;
    display_ui_text(k->x + (k->w - tw) / 2, k->y + (k->h - th) / 2, k->label, scale, COL_LABEL, bg);
}

static const ui_key_t *page_keys(int *n)
{
    if (page) { *n = COUNT(page_more); return page_more; }
    *n = COUNT(page_letters);
    return page_letters;
}

static void draw_keyboard(void)
{
    int n;
    const ui_key_t *keys = page_keys(&n);
    for (int i = 0; i < n; i++) draw_key(&keys[i], false);
}

static void draw_all(void)
{
    display_ui_fill(0, 0, BOARD_PANEL_W, BOARD_PANEL_H, COL_BG);
    for (int i = 0; i < COUNT(pad_keys); i++) draw_key(&pad_keys[i], false);
    draw_keyboard();
}

/* LCD luminance to a greenish STN palette, big-endian RGB565. */
static uint8_t lut[256][2];

static void build_lut(void)
{
    const int ink[3] = { 0x18, 0x20, 0x28 }, paper[3] = { 0xb4, 0xc4, 0xa4 };
    for (int v = 0; v < 256; v++) {
        int r = ink[0] + (paper[0] - ink[0]) * v / 255;
        int g = ink[1] + (paper[1] - ink[1]) * v / 255;
        int b = ink[2] + (paper[2] - ink[2]) * v / 255;
        uint16_t c = UI_RGB(r, g, b);
        lut[v][0] = (uint8_t)(c >> 8);
        lut[v][1] = (uint8_t)c;
    }
}

static void draw_lcd(const uint8_t *lum)
{
    static uint8_t row[CY_LCD_W * LCD_SCALE * 2];
    display_ui_window(LCD_X, LCD_Y, CY_LCD_W * LCD_SCALE, CY_LCD_H * LCD_SCALE);
    for (int y = 0; y < CY_LCD_H; y++) {
        const uint8_t *src = lum + y * CY_LCD_W;
        uint8_t *d = row;
        for (int x = 0; x < CY_LCD_W; x++) {
            const uint8_t *c = lut[src[x]];
            d[0] = c[0]; d[1] = c[1]; d[2] = c[0]; d[3] = c[1];
            d += 4;
        }
        for (int i = 0; i < LCD_SCALE; i++) display_ui_write_row(row, CY_LCD_W * LCD_SCALE);
    }
}

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

static const ui_key_t *hit(int x, int y)
{
    for (int i = 0; i < COUNT(pad_keys); i++) {
        const ui_key_t *k = &pad_keys[i];
        if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + k->h) return k;
    }
    int n;
    const ui_key_t *keys = page_keys(&n);
    for (int i = 0; i < n; i++) {
        const ui_key_t *k = &keys[i];
        if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + k->h)
            return k->kind == K_NONE ? NULL : k;
    }
    return NULL;
}

static void touch_down(int x, int y)
{
    const ui_key_t *k = hit(x, y);
    if (!k) return;
    switch (k->kind) {
    case K_SHIFT:
        shift_on = !shift_on;
        draw_key(k, false);
        return;
    case K_FN:
        fn_on = !fn_on;
        draw_key(k, false);
        return;
    case K_PAGE:
        page ^= 1;
        draw_keyboard();
        return;
    default:
        break;
    }
    if (shift_on) cybiko_app_key(CYK_SHIFT, true);
    if (fn_on) cybiko_app_key(CYK_FN, true);
    cybiko_app_key(k->key, true);
    held = k;
    held_since = esp_timer_get_time();
    draw_key(k, true);
}

static void release_held(void)
{
    cybiko_app_key(held->key, false);
    bool redraw = shift_on || fn_on;
    if (shift_on) cybiko_app_key(CYK_SHIFT, false);
    if (fn_on) cybiko_app_key(CYK_FN, false);
    shift_on = fn_on = false;
    draw_key(held, false);
    held = NULL;
    if (redraw) draw_keyboard();
}

/* ------------------------------------------------------------------ */

static void ui_task(void *arg)
{
    (void)arg;
    static uint8_t lum[CY_LCD_W * CY_LCD_H];
    cybiko_t *m = cybiko_app_machine();
    uint32_t shown = 0xffffffff;
    bool was_down = false, boot_was = false, onoff_held = false;
    int64_t boot_since = 0, last_frame = 0, last_save = esp_timer_get_time(), last_touch = 0;

    build_lut();
    draw_all();
    for (;;) {
        int64_t now = esp_timer_get_time();

        int x, y;
        bool down = input_touch_panel(&x, &y);
        if (down) last_touch = now;
        if (down && !was_down) touch_down(x, y);
        /* keep a key down at least 60 ms so CyOS's keyboard scan sees it */
        if (!down && held && now - held_since >= 60000) release_held();
        was_down = down;

        /* BOOT button: On/Off key; a 3 s hold saves and restarts */
        bool boot = input_boot_button_held();
        if (boot && !boot_was) {
            boot_since = now;
            cybiko_app_key(CYK_ONOFF, true);
            onoff_held = true;
        }
        if (boot && now - boot_since > 3000000) {
            if (onoff_held) cybiko_app_key(CYK_ONOFF, false);
            display_ui_fill(0, 0, CY_LCD_W * LCD_SCALE, CY_LCD_H * LCD_SCALE, COL_BG);
            display_ui_text(40, 90, "SAVING...", 3, COL_LABEL, COL_BG);
            cybiko_app_save(false);
            esp_restart();
        }
        if (!boot && boot_was && onoff_held) {
            cybiko_app_key(CYK_ONOFF, false);
            onoff_held = false;
        }
        boot_was = boot;

        if (now - last_frame >= 40000) {         /* up to 25 fps */
            uint32_t v = cybiko_lcd_render(m, lum);
            if (v != shown) {
                draw_lcd(lum);
                shown = v;
            }
            last_frame = now;
        }

        /* Save the RAM disk every 30 s if it changed, while the user is idle. */
        if (now - last_save > 30000000 && now - last_touch > 2000000) {
            cybiko_app_save(false);
            last_save = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void cybiko_ui_start(void)
{
    xTaskCreatePinnedToCore(ui_task, "cybiko_ui", 6144, NULL, 4, NULL, 0);
}
