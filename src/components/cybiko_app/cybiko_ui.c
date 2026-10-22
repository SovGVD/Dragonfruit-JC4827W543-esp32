/* Cybiko screen and on-screen keyboard on the 480x272 panel, in portrait
 * (the default: the board held with its long edge vertical and USB at the
 * bottom, like the Palm compact layout) or landscape. The boot menu picks one
 * (cybiko_app_portrait()).
 *
 * Portrait, 272x480, every key on one page, LCD at 1x:
 *
 *        +------------------------+
 *        | Cybiko LCD 160x100 100%|
 *        | F1 F2 F3 F4 F5 F6 F7 INS|
 *        |    UP     | ESC  MENU  |
 *        | LEFT SEL RIGHT| TAB DEL|
 *        |   DOWN    | HELP ON/OFF|
 *        | Q W E R T Y U I O P    |
 *        | A S D F G H J K L ;    |
 *        | Z X C V B N M , . !    |
 *        | SHIFT ( SPACE ) FN ENTER|
 *        +------------------------+
 *
 * Landscape, LCD at 2x:
 *
 *   +--------------------------+----------------+
 *   |                          | SPEED 100%     |
 *   |                          | ESC   UP  MENU |
 *   |  Cybiko LCD 160x100 x2   | LEFT SEL RIGHT |
 *   |        (320x200)         | TAB  DOWN  DEL |
 *   |                          | ENTER   SPACE  |
 *   +--------------------------+----------------+
 *   | Q  W  E  R  T  Y  U  I  O  P              |
 *   | A  S  D  F  G  H  J  K  L  .              |
 *   | SHIFT Z X  C  V  B  N  M  FN  MORE        |
 *   +--------------------------------------------+
 *
 * MORE swaps the bottom block to F1-F7, INS, ON/OFF, ( ) , ; ! and HELP. Only whole-number LCD
 * zoom is used, so every Cybiko pixel stays square. Key positions are in
 * "logical" coordinates (272x480 in portrait), turned onto the panel when
 * drawn and when a touch is read. Touch is single point, so SHIFT and FN
 * latch: they apply to the next key and release with it. The BOOT button is
 * the On/Off key; holding it 3 s saves to SD and restarts to the boot menu. */
#include <stdio.h>
#include <string.h>
#include "cybiko_ui.h"
#include "display_ui.h"
#include "input.h"
#include "board.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cybiko_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR    /* host builds */
#endif

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

#define PAD_Y 14             /* the speed line above the pad */
#define PAD_KEY(col, row, k, l) { PAD_X + (col) * 53, PAD_Y + (row) * 46, (col) == 2 ? 54 : 53, 46, k, K_NORMAL, true, l }
static const ui_key_t pad_keys[] = {
    PAD_KEY(0, 0, CYK_ESC, "ESC"), PAD_KEY(1, 0, CYK_UP, "UP"), PAD_KEY(2, 0, CYK_MENU, "MENU"),
    PAD_KEY(0, 1, CYK_LEFT, "LEFT"), PAD_KEY(1, 1, CYK_SELECT, "SEL"), PAD_KEY(2, 1, CYK_RIGHT, "RIGHT"),
    PAD_KEY(0, 2, CYK_TAB, "TAB"), PAD_KEY(1, 2, CYK_DOWN, "DOWN"), PAD_KEY(2, 2, CYK_DEL, "DEL"),
    { PAD_X, PAD_Y + 138, 80, KB_Y - PAD_Y - 138, CYK_ENTER, K_NORMAL, true, "ENTER" },
    { PAD_X + 80, PAD_Y + 138, 80, KB_Y - PAD_Y - 138, CYK_SPACE, K_NORMAL, true, "SPACE" },
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
    KB(6, 0, CYK_F7, K_NORMAL, "F7"), KB(7, 0, CYK_INS, K_NORMAL, "INS"),
    { 8 * KB_KEY_W, KB_Y, 2 * KB_KEY_W, KB_ROW_H, CYK_ONOFF, K_NORMAL, false, "ON/OFF" },
    KB(0, 1, CYK_LPAREN, K_NORMAL, "("), KB(1, 1, CYK_RPAREN, K_NORMAL, ")"),
    KB(2, 1, CYK_COMMA, K_NORMAL, ","), KB(3, 1, CYK_SEMI, K_NORMAL, ";"),
    KB(4, 1, CYK_EXCL, K_NORMAL, "!"),
    { 5 * KB_KEY_W, KB_Y + KB_ROW_H, 2 * KB_KEY_W, KB_ROW_H, CYK_HELP, K_NORMAL, false, "HELP" },
    { 7 * KB_KEY_W, KB_Y + KB_ROW_H, 3 * KB_KEY_W, KB_ROW_H, -1, K_NONE, true, "" },
    KB(0, 2, CYK_SHIFT, K_SHIFT, "SHIFT"),
    { KB_KEY_W, KB_Y + 2 * KB_ROW_H, 7 * KB_KEY_W, KB_ROW_H, -1, K_NONE, true, "" },
    KB(8, 2, CYK_FN, K_FN, "FN"), KB(9, 2, -1, K_PAGE, "ABC"),
};

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

typedef struct {
    const ui_key_t *fixed;
    int n_fixed;
    const ui_key_t *pages[2];     /* the bottom block; pages[1] NULL if none */
    int n_pages[2];
    int lcd_x, lcd_y, lcd_scale;
    int speed_x, speed_y, speed_w, speed_h;    /* the emulation speed box */
} ui_layout_t;

static const ui_layout_t layout_landscape = {
    pad_keys, COUNT(pad_keys), { page_letters, page_more },
    { COUNT(page_letters), COUNT(page_more) }, 0, 0, 2, PAD_X, 0, PAD_W, PAD_Y,
};

/* Portrait keys are laid out at start-up; see build_portrait(). */
#define P_W BOARD_PANEL_H
#define P_H BOARD_PANEL_W
static EXT_RAM_BSS_ATTR ui_key_t portrait_keys[64];
static ui_layout_t layout_portrait = { portrait_keys, 0, { NULL, NULL }, { 0, 0 }, 56, 10, 1,
                                     218, 10, 54, 20 };

static void add_key(int x, int y, int w, int h, int key, int kind, bool alt, const char *label)
{
    if (layout_portrait.n_fixed == COUNT(portrait_keys)) return;
    portrait_keys[layout_portrait.n_fixed++] =
        (ui_key_t){ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)key, (uint8_t)kind,
                    alt, label };
}

typedef struct { int16_t key; const char *label; } key_label_t;

/* A row of keys spread evenly over x0..x1 (whole pixels, no gaps). */
static void add_row(const key_label_t *keys, int n, int x0, int x1, int y, int h)
{
    for (int i = 0; i < n; i++) {
        int a = x0 + (x1 - x0) * i / n, b = x0 + (x1 - x0) * (i + 1) / n;
        add_key(a, y, b - a, h, keys[i].key, K_NORMAL, false, keys[i].label);
    }
}

static void build_portrait(void)
{
    static const key_label_t fkeys[] = {
        { CYK_F1, "F1" }, { CYK_F2, "F2" }, { CYK_F3, "F3" }, { CYK_F4, "F4" },
        { CYK_F5, "F5" }, { CYK_F6, "F6" }, { CYK_F7, "F7" }, { CYK_INS, "INS" },
    };
    static const key_label_t row0[] = {
        { CYK_Q, "Q" }, { CYK_W, "W" }, { CYK_E, "E" }, { CYK_R, "R" }, { CYK_T, "T" },
        { CYK_Y, "Y" }, { CYK_U, "U" }, { CYK_I, "I" }, { CYK_O, "O" }, { CYK_P, "P" },
    };
    static const key_label_t row1[] = {
        { CYK_A, "A" }, { CYK_S, "S" }, { CYK_D, "D" }, { CYK_F, "F" }, { CYK_G, "G" },
        { CYK_H, "H" }, { CYK_J, "J" }, { CYK_K, "K" }, { CYK_L, "L" }, { CYK_SEMI, ";" },
    };
    static const key_label_t row2[] = {
        { CYK_Z, "Z" }, { CYK_X, "X" }, { CYK_C, "C" }, { CYK_V, "V" }, { CYK_B, "B" },
        { CYK_N, "N" }, { CYK_M, "M" }, { CYK_COMMA, "," }, { CYK_DOT, "." }, { CYK_EXCL, "!" },
    };
    layout_portrait.n_fixed = 0;
    add_row(fkeys, COUNT(fkeys), 0, P_W, 122, 36);
    /* gamepad: a cross of 40 px keys */
    const int c = 40, gx = 8, gy = 166;
    add_key(gx + c, gy, c, c, CYK_UP, K_NORMAL, true, "UP");
    add_key(gx, gy + c, c, c, CYK_LEFT, K_NORMAL, true, "LEFT");
    add_key(gx + c, gy + c, c, c, CYK_SELECT, K_NORMAL, true, "SEL");
    add_key(gx + 2 * c, gy + c, c, c, CYK_RIGHT, K_NORMAL, true, "RIGHT");
    add_key(gx + c, gy + 2 * c, c, c, CYK_DOWN, K_NORMAL, true, "DOWN");
    add_key(132, gy, 70, c, CYK_ESC, K_NORMAL, true, "ESC");
    add_key(202, gy, 70, c, CYK_MENU, K_NORMAL, true, "MENU");
    add_key(132, gy + c, 70, c, CYK_TAB, K_NORMAL, true, "TAB");
    add_key(202, gy + c, 70, c, CYK_DEL, K_NORMAL, true, "DEL");
    add_key(132, gy + 2 * c, 70, c, CYK_HELP, K_NORMAL, true, "HELP");
    add_key(202, gy + 2 * c, 70, c, CYK_ONOFF, K_NORMAL, true, "ON/OFF");
    /* keyboard: four 47 px rows down to the bottom edge, laid out like the
     * Cybiko's own, with SHIFT and FN beside the space bar */
    const int ky = 292, kh = 47, by = ky + 3 * kh;
    add_row(row0, COUNT(row0), 0, P_W, ky, kh);
    add_row(row1, COUNT(row1), 0, P_W, ky + kh, kh);
    add_row(row2, COUNT(row2), 0, P_W, ky + 2 * kh, kh);
    add_key(0, by, 40, kh, CYK_SHIFT, K_SHIFT, false, "SHIFT");
    add_key(40, by, 28, kh, CYK_LPAREN, K_NORMAL, false, "(");
    add_key(68, by, 80, kh, CYK_SPACE, K_NORMAL, false, "SPACE");
    add_key(148, by, 28, kh, CYK_RPAREN, K_NORMAL, false, ")");
    add_key(176, by, 40, kh, CYK_FN, K_FN, false, "FN");
    add_key(216, by, P_W - 216, kh, CYK_ENTER, K_NORMAL, false, "ENTER");
}

static const ui_layout_t *L;
static bool portrait;
static int page;                     /* 0 letters, 1 more (landscape only) */
static bool shift_on, fn_on;
static const ui_key_t *held;         /* key whose press is in progress */
static int64_t held_since;

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* Scratch image for one key or box, big-endian RGB565, in PSRAM. */
#define BOX_MAX_PX (BOARD_PANEL_W * 48)
static uint8_t *box;
static uint8_t line[BOARD_PANEL_W * 2];

/* Logical rectangle to panel window. Portrait turns the picture 90 degrees
 * clockwise: logical (u, v) lands on panel (479 - v, u). */
static void window(int x, int y, int w, int h)
{
    if (portrait) display_ui_window(BOARD_PANEL_W - y - h, x, h, w);
    else display_ui_window(x, y, w, h);
}

/* Writes a w x h logical image to logical (x, y). */
static void blit(int x, int y, int w, int h, const uint8_t *img)
{
    window(x, y, w, h);
    if (!portrait) {
        for (int j = 0; j < h; j++) display_ui_write_row(img + j * w * 2, w);
        return;
    }
    for (int i = 0; i < w; i++) {            /* each logical column is a panel row */
        uint8_t *d = line;
        for (int j = h - 1; j >= 0; j--, d += 2) {
            d[0] = img[(j * w + i) * 2];
            d[1] = img[(j * w + i) * 2 + 1];
        }
        display_ui_write_row(line, h);
    }
}

static void box_put(int w, int x, int y, uint16_t c)
{
    box[(y * w + x) * 2] = (uint8_t)(c >> 8);
    box[(y * w + x) * 2 + 1] = (uint8_t)c;
}

/* A framed box with a centred label, drawn at logical (x, y). */
static void draw_box(int x, int y, int w, int h, uint16_t bg, const char *label)
{
    if (!box || w * h > BOX_MAX_PX) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            box_put(w, i, j, i == 0 || j == 0 || i == w - 1 || j == h - 1 ? COL_EDGE : bg);
    if (label[0]) {
        int scale = display_ui_text_width(label, 2) <= w - 6 && h >= 20 ? 2 : 1;
        int tw = display_ui_text_width(label, scale) - scale, th = UI_FONT_H * scale;
        int tx = (w - tw) / 2, ty = (h - th) / 2;
        for (int n = 0; label[n]; n++) {
            for (int r = 0; r < th; r++) {
                int bits = display_font_row(label[n], r / scale);
                for (int c = 0; c < 5 * scale; c++) {
                    int px = tx + n * 6 * scale + c, py = ty + r;
                    if ((bits & (0x10 >> (c / scale))) && px >= 0 && px < w && py >= 0 && py < h)
                        box_put(w, px, py, COL_LABEL);
                }
            }
        }
    }
    blit(x, y, w, h, box);
}

/* What CyOS types for Fn + a letter key (found by typing each one), by
 * letter. C gives a copyright sign, shown as "(C)". */
static const char *const fn_chars[26] = {
    "@", "}", "(C)", "$", "3", "%", "*", "+", "8", "-", "_", "=", ">",
    "<", "9", "0", "1", "4", "&", "5", "7", "{", "2", "#", "6", "\"",
};

/* What Fn and Shift type on the punctuation keys (found the same way). Shift
 * + ")" gives a smiley, shown as ":)". */
static const struct { char key; const char *fn, *shift; } punct_chars[] = {
    { '.', "/", "\\" }, { ',', "'", "~" }, { ';', ":", "|" }, { '!', "?", "^" },
    { '(', "[", "`" }, { ')', "]", ":)" },
};

/* The label a key shows: letters follow SHIFT (lower case otherwise, as
 * CyOS types them) and FN shows what Fn + the key types. */
static const char *key_label(const ui_key_t *k)
{
    static char buf[2];
    const char *l = k->label;
    if (k->kind != K_NORMAL || !l[0] || l[1]) return l;
    for (int i = 0; i < COUNT(punct_chars); i++)
        if (l[0] == punct_chars[i].key) return fn_on ? punct_chars[i].fn : shift_on ? punct_chars[i].shift : l;
    if (l[0] < 'A' || l[0] > 'Z') return l;
    if (fn_on && fn_chars[l[0] - 'A']) return fn_chars[l[0] - 'A'];
    buf[0] = shift_on ? l[0] : (char)(l[0] - 'A' + 'a');
    buf[1] = 0;
    return buf;
}

static void draw_key(const ui_key_t *k, bool lit)
{
    uint16_t bg = lit ? COL_KEY_ON : k->alt ? COL_KEY_ALT : COL_KEY;
    if (k->kind == K_SHIFT && shift_on) bg = COL_KEY_ON;
    if (k->kind == K_FN && fn_on) bg = COL_KEY_ON;
    if (k->kind == K_NONE) bg = COL_BG;
    draw_box(k->x, k->y, k->w, k->h, bg, key_label(k));
}

static const ui_key_t *page_keys(int *n)
{
    *n = L->n_pages[page];
    return L->pages[page];
}

static void draw_keyboard(void)
{
    int n;
    const ui_key_t *keys = page_keys(&n);
    for (int i = 0; i < n; i++) draw_key(&keys[i], false);
}

static void draw_keys(void)
{
    for (int i = 0; i < L->n_fixed; i++) draw_key(&L->fixed[i], false);
    draw_keyboard();
}

/* How fast the emulation runs against the real machine, over the last
 * second (cybiko_app_speed()). */
static void draw_speed(int pct)
{
    char buf[16];
    if (pct < 0) buf[0] = 0;
    else snprintf(buf, sizeof buf, portrait ? "%d%%" : "SPEED %d%%", pct > 999 ? 999 : pct);
    draw_box(L->speed_x, L->speed_y, L->speed_w, L->speed_h, COL_BG, buf);
}

static void draw_all(void)
{
    display_ui_fill(0, 0, BOARD_PANEL_W, BOARD_PANEL_H, COL_BG);
    draw_keys();
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
    const int s = L->lcd_scale, w = CY_LCD_W * s, h = CY_LCD_H * s;
    window(L->lcd_x, L->lcd_y, w, h);
    if (!portrait) {
        for (int y = 0; y < CY_LCD_H; y++) {
            const uint8_t *src = lum + y * CY_LCD_W;
            uint8_t *d = line;
            for (int x = 0; x < w; x++, d += 2) {
                const uint8_t *c = lut[src[x / s]];
                d[0] = c[0]; d[1] = c[1];
            }
            for (int i = 0; i < s; i++) display_ui_write_row(line, w);
        }
        return;
    }
    for (int i = 0; i < w; i++) {            /* each LCD column is a panel row */
        uint8_t *d = line;
        for (int j = h - 1; j >= 0; j--, d += 2) {
            const uint8_t *c = lut[lum[(j / s) * CY_LCD_W + i / s]];
            d[0] = c[0]; d[1] = c[1];
        }
        display_ui_write_row(line, h);
    }
}

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

static const ui_key_t *hit(int x, int y)
{
    for (int i = 0; i < L->n_fixed; i++) {
        const ui_key_t *k = &L->fixed[i];
        if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + k->h)
            return k->kind == K_NONE ? NULL : k;
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

static void touch_down(int px, int py)
{
    int x = portrait ? py : px, y = portrait ? BOARD_PANEL_W - 1 - px : py;
    const ui_key_t *k = hit(x, y);
    if (!k) return;
    switch (k->kind) {
    case K_SHIFT:
        shift_on = !shift_on;
        draw_keys();
        return;
    case K_FN:
        fn_on = !fn_on;
        draw_keys();
        return;
    case K_PAGE:
        if (L->pages[1]) page ^= 1;
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
    if (redraw) draw_keys();
}

/* ------------------------------------------------------------------ */

static void ui_task(void *arg)
{
    (void)arg;
    uint8_t *lum = heap_caps_malloc(CY_LCD_W * CY_LCD_H, MALLOC_CAP_SPIRAM);
    cybiko_t *m = cybiko_app_machine();
    uint32_t shown = 0xffffffff;
    int speed_shown = -2;
    bool was_down = false, boot_was = false, onoff_held = false;
    int64_t boot_since = 0, last_frame = 0, last_save = esp_timer_get_time(), last_touch = 0;

    build_lut();
    portrait = cybiko_app_portrait();
    if (portrait) build_portrait();
    L = portrait ? &layout_portrait : &layout_landscape;
    box = heap_caps_malloc(BOX_MAX_PX * 2, MALLOC_CAP_SPIRAM);
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
            draw_box(L->lcd_x, L->lcd_y, CY_LCD_W, CY_LCD_H, COL_BG, "SAVING...");
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
        int speed = cybiko_app_speed();
        if (speed != speed_shown) {
            draw_speed(speed);
            speed_shown = speed;
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
