#pragma once
/* Cybiko Xtreme machine: H8S/2323 on-chip peripherals plus the board
 * devices, built on the portable H8S core.
 *
 * Portable C with no platform dependencies, so the same code runs in the
 * firmware and in the host runner/tests. The hardware description follows
 * MAME's cybiko driver and H8 peripheral models (BSD-3-Clause, Tim
 * Schuerewegen and Olivier Galibert) and the Hitachi H8S/2323 hardware
 * manual.
 *
 * Memory map:
 *   000000-03ffff  32 KB mask ROM (cyrom150.bin), mirrored
 *   100000-100001  HD66421 LCD controller (index, data)
 *   200000-200003  USB controller (writes ignored)
 *   400000-5fffff  2 MB RAM; the first 512 KB is the battery-backed RAM disk
 *   600000-7fffff  SST39VF400A 512 KB flash (cyos_v1508.bin), mirrored
 *   e00000-efffff  keyboard matrix
 *   ffdc00-fffbff  8 KB on-chip RAM
 *   fffe80-ffffff  on-chip I/O registers */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "h8s.h"

#define CY_CLOCK_HZ    18432000u
#define CY_LCD_W       160
#define CY_LCD_H       100
#define CY_ROM_SIZE    0x8000u
#define CY_ROM_BUF     0x10000u      /* ROM buffer: 32 KB image + its mirror */
#define CY_FLASH_SIZE  0x80000u
#define CY_RAM_SIZE    0x200000u
#define CY_RAMDISK_SIZE 0x80000u     /* part of RAM kept across power cycles */

/* Keys are (row << 4) | bit in the keyboard matrix (rows A.0-A.14 in MAME). */
#define CY_KEY(row, bit) (((row) << 4) | (bit))
enum {
    CYK_F7 = CY_KEY(0, 0), CYK_M = CY_KEY(0, 8), CYK_K = CY_KEY(0, 11),
    CYK_I = CY_KEY(0, 12), CYK_O = CY_KEY(0, 13), CYK_L = CY_KEY(0, 14),
    CYK_F6 = CY_KEY(1, 0), CYK_G = CY_KEY(1, 1), CYK_B = CY_KEY(1, 2),
    CYK_N = CY_KEY(1, 3), CYK_H = CY_KEY(1, 4), CYK_Y = CY_KEY(1, 5),
    CYK_U = CY_KEY(1, 6), CYK_J = CY_KEY(1, 7),
    CYK_F5 = CY_KEY(2, 0), CYK_D = CY_KEY(2, 8), CYK_C = CY_KEY(2, 9),
    CYK_V = CY_KEY(2, 11), CYK_F = CY_KEY(2, 12), CYK_R = CY_KEY(2, 13),
    CYK_T = CY_KEY(2, 14),
    CYK_F4 = CY_KEY(3, 0), CYK_Q = CY_KEY(3, 1), CYK_A = CY_KEY(3, 2),
    CYK_Z = CY_KEY(3, 3), CYK_X = CY_KEY(3, 4), CYK_S = CY_KEY(3, 5),
    CYK_W = CY_KEY(3, 6), CYK_E = CY_KEY(3, 7),
    CYK_F3 = CY_KEY(4, 0), CYK_ENTER = CY_KEY(4, 3), CYK_SELECT = CY_KEY(4, 4),
    CYK_SPACE = CY_KEY(4, 6),
    CYK_F2 = CY_KEY(5, 0), CYK_TAB = CY_KEY(5, 7), CYK_DEL = CY_KEY(5, 8),
    CYK_AS = CY_KEY(5, 9), CYK_ESC = CY_KEY(5, 10),
    CYK_F1 = CY_KEY(6, 0), CYK_UP = CY_KEY(6, 11), CYK_RIGHT = CY_KEY(6, 12),
    CYK_DOWN = CY_KEY(6, 13), CYK_LEFT = CY_KEY(6, 14),
    CYK_FN = CY_KEY(7, 15), CYK_SHIFT = CY_KEY(8, 15),
    CYK_HELP = CY_KEY(13, 0), CYK_DOT = CY_KEY(13, 1), CYK_P = CY_KEY(13, 4),
    CYK_ONOFF = CY_KEY(14, 15),
};

/* Host hooks. Any of them may be NULL. */
typedef struct {
    void *user;
    /* A byte the CPU finished sending on SCI channel `port` (0-2). SCI2 is
     * the debug console at 57600 baud. */
    void (*serial_tx)(void *user, int port, uint8_t byte);
    /* Current wall-clock time in seconds since 1970 (UTC is fine); the RTC
     * runs from it. NULL keeps a clock that counts emulated time from 0. */
    int64_t (*wall_time)(void *user);
    /* 10-bit value for A/D channel 0-7. NULL reads 0, as in MAME. */
    uint16_t (*adc_read)(void *user, int channel);
} cy_host_t;

/* ---- peripheral state (internal, visible for tests and save states) ---- */

typedef struct {
    uint8_t tcr, tmdr, tior[2], tier, tsr;
    uint16_t tcnt, tgr[4];
    uint32_t acc;              /* states not yet turned into counts */
} cy_tpu_ch_t;

typedef struct {
    uint8_t tcr, tcsr, tcor[2], tcnt;
    uint32_t acc;
} cy_tmr_ch_t;

typedef struct {
    uint8_t smr, brr, scr, ssr, ssr_read, tdr, rdr, tsr;
    bool tx_busy, rx_busy;
    uint32_t tx_left, rx_left; /* states until the frame in flight finishes */
    uint8_t rx_fifo[64];
    uint8_t rx_head, rx_count;
} cy_sci_t;

typedef struct {
    uint8_t reg[0x20];
    uint8_t cmd, x, y;
    uint8_t ram[CY_LCD_W * CY_LCD_H / 4];
    uint32_t version;          /* bumps on every change that affects output */
} cy_lcd_t;

typedef struct {
    uint8_t data[16];
    bool scl, sda, active, send;
    uint8_t inp, bits, pos, idx;
    uint8_t recv[4];
    int64_t offset;            /* RTC seconds minus host seconds */
    uint32_t hundredths_base;
} cy_rtc_t;

typedef struct {
    uint8_t *mem;              /* 512 KB, big-endian 16-bit words */
    int mode;
    bool dirty;                /* contents changed since cybiko_flash_saved */
} cy_flash_t;

typedef struct cybiko {
    h8s_cpu_t cpu;
    cy_host_t host;

    uint8_t *rom, *ram;
    uint8_t iram[0x2000];
    cy_flash_t flash;
    cy_lcd_t lcd;
    cy_rtc_t rtc;
    uint16_t keys[16];         /* pressed bits per keyboard row */

    /* interrupt controller */
    uint32_t pend[3];          /* latched internal interrupt requests by vector */
    uint8_t ipr[11], ier, isr;
    uint16_t iscr;

    /* system */
    uint8_t syscr, sbycr, sckcr, mstpcr[2];
    uint8_t misc[0x180];       /* plain storage for other registers fe80-ffff */
    uint8_t dr[16], ddr[16];   /* ports 1-6 at 0-5, ports A-G at 9-15 */
    uint8_t pcr[5], odr[2];
    bool standby;              /* software standby: SLEEP with SSBY set */
    bool reset_pending;        /* watchdog asked for a chip reset */

    cy_tpu_ch_t tpu[6];
    uint8_t tstr, tsyr;
    cy_tmr_ch_t tmr[2];
    uint8_t wdt_tcsr, wdt_tcnt, wdt_rst;
    uint32_t wdt_acc;
    cy_sci_t sci[3];
    uint8_t adcsr, adcr;
    uint16_t addr[4];
    uint32_t adc_left;
    int adc_ch;

    uint64_t synced;           /* CPU state count the peripherals have reached */

    /* diagnostics: things a ROM used that the model does not cover */
    uint32_t unmapped_reads, unmapped_writes, illegal_ops, dtc_dma_use;
    uint32_t wdt_resets;
} cybiko_t;

/* Set up a machine on caller-owned memory: rom is CY_ROM_BUF bytes with the
 * 32 KB ROM image in the first half, flash is CY_FLASH_SIZE bytes, ram is
 * CY_RAM_SIZE bytes. RAM keeps its contents (pass it zeroed for a first
 * boot, or with a saved RAM disk). Ends with a reset. */
void cybiko_init(cybiko_t *m, const cy_host_t *host, uint8_t *rom,
                 uint8_t *flash, uint8_t *ram);
/* Power-on reset of the CPU and peripherals; memory is kept. */
void cybiko_reset(cybiko_t *m);
/* Run for about `states` CPU states (18.432 MHz). Returns states done. */
int32_t cybiko_run(cybiko_t *m, int32_t states);

void cybiko_key(cybiko_t *m, int key, bool down);
/* Queue a byte for the CPU to receive on SCI channel `port`. Returns false
 * when that channel's queue is full. */
bool cybiko_serial_rx(cybiko_t *m, int port, uint8_t byte);

/* Render the LCD as 8-bit luminance (0 black, 255 white), CY_LCD_W x
 * CY_LCD_H, row 0 at the top. Returns lcd.version so callers can skip
 * unchanged frames. */
uint32_t cybiko_lcd_render(const cybiko_t *m, uint8_t *lum);

/* True once the OS has put the CPU into software standby (switched off). */
static inline bool cybiko_is_off(const cybiko_t *m) { return m->standby; }
