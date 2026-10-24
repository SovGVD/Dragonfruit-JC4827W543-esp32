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
 *   800000-dfffff  optional bigger RAM disk (cybiko_set_disk, not real hardware)
 *   e00000-efffff  keyboard matrix
 *   ffdc00-fffbff  8 KB on-chip RAM
 *   fffe80-ffffff  on-chip I/O registers
 *
 * SCI1 carries the boot loader's and CyOS's debug text, SCI2 the radio
 * co-processor (cy_rf.c). */
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
#define CY_HLE_SLOTS   16
#define CY_RAMDISK_SIZE 0x80000u     /* part of RAM kept across power cycles */
#define CY_DISK_BASE   0x800000u     /* where cybiko_set_disk puts the RAM disk */
#define CY_DISK_MAX    0x600000u     /* 6 MB: up to the keyboard at e00000 */
#define CY_DISK_CHUNK  4096u         /* dirty-tracking granularity */

/* Keys are (row << 4) | bit in the keyboard matrix, as MAME's A.0-A.8. CyOS
 * scans rows 0-9 only, with every higher address bit low, so Help, "." and P
 * (A.13 in MAME) must sit in row 9: in row 13 they showed up in every row
 * scanned. On/Off is not in the matrix; it is PA7/IRQ7 (row 15 here is only
 * its slot in keys[], never scanned). Menu, Ins, the punctuation keys and
 * the brackets are not in MAME; they were found by trying every free
 * position in a CyOS text field. Ins is the key MAME calls "As". */
#define CY_KEY(row, bit) (((row) << 4) | (bit))
enum {
    CYK_F7 = CY_KEY(0, 0), CYK_M = CY_KEY(0, 8), CYK_K = CY_KEY(0, 11),
    CYK_I = CY_KEY(0, 12), CYK_O = CY_KEY(0, 13), CYK_L = CY_KEY(0, 14),
    CYK_RPAREN = CY_KEY(0, 9), CYK_COMMA = CY_KEY(0, 10),
    CYK_F6 = CY_KEY(1, 0), CYK_G = CY_KEY(1, 1), CYK_B = CY_KEY(1, 2),
    CYK_N = CY_KEY(1, 3), CYK_H = CY_KEY(1, 4), CYK_Y = CY_KEY(1, 5),
    CYK_U = CY_KEY(1, 6), CYK_J = CY_KEY(1, 7),
    CYK_F5 = CY_KEY(2, 0), CYK_D = CY_KEY(2, 8), CYK_C = CY_KEY(2, 9),
    CYK_V = CY_KEY(2, 11), CYK_F = CY_KEY(2, 12), CYK_R = CY_KEY(2, 13),
    CYK_T = CY_KEY(2, 14), CYK_LPAREN = CY_KEY(2, 10),
    CYK_F4 = CY_KEY(3, 0), CYK_Q = CY_KEY(3, 1), CYK_A = CY_KEY(3, 2),
    CYK_Z = CY_KEY(3, 3), CYK_X = CY_KEY(3, 4), CYK_S = CY_KEY(3, 5),
    CYK_W = CY_KEY(3, 6), CYK_E = CY_KEY(3, 7),
    CYK_F3 = CY_KEY(4, 0), CYK_ENTER = CY_KEY(4, 3), CYK_SELECT = CY_KEY(4, 4),
    CYK_SPACE = CY_KEY(4, 6), CYK_MENU = CY_KEY(4, 5),
    CYK_F2 = CY_KEY(5, 0), CYK_TAB = CY_KEY(5, 7), CYK_DEL = CY_KEY(5, 8),
    CYK_INS = CY_KEY(5, 9), CYK_ESC = CY_KEY(5, 10),
    CYK_F1 = CY_KEY(6, 0), CYK_UP = CY_KEY(6, 11), CYK_RIGHT = CY_KEY(6, 12),
    CYK_DOWN = CY_KEY(6, 13), CYK_LEFT = CY_KEY(6, 14),
    CYK_FN = CY_KEY(7, 15), CYK_SHIFT = CY_KEY(8, 15),
    CYK_HELP = CY_KEY(9, 0), CYK_DOT = CY_KEY(9, 1), CYK_P = CY_KEY(9, 4),
    CYK_EXCL = CY_KEY(9, 2), CYK_SEMI = CY_KEY(9, 3),
    CYK_ONOFF = CY_KEY(15, 15),
};

/* Host hooks. Any of them may be NULL. */
typedef struct {
    void *user;
    /* A byte the CPU finished sending on SCI channel `port` (0-2). SCI1 is
     * the debug console; SCI2 bytes also go to the radio model. */
    void (*serial_tx)(void *user, int port, uint8_t byte);
    /* Current wall-clock time in seconds since 1970 (UTC is fine); the RTC
     * runs from it. NULL keeps a clock that counts emulated time from 0. */
    int64_t (*wall_time)(void *user);
    /* 10-bit value for A/D channel 0-7. NULL reads 0, as in MAME. */
    uint16_t (*adc_read)(void *user, int channel);
    /* The speaker tone changed: a rectangle wave of freq_hz with the given
     * duty, or silence when freq_hz is 0. Matches audio_tone(). */
    void (*speaker)(void *user, float freq_hz, float duty);
    /* A radio packet CyOS sent (CY_RF_SHORT or CY_RF_LONG bytes) on radio
     * channel `channel`; deliver it to the other machines' cybiko_rf_receive. */
    void (*rf_send)(void *user, const uint8_t *pkt, size_t len, uint8_t channel);
    /* The MP3 cartridge's SD card: 512-byte blocks (see cy_cart_t). */
    bool (*card_read)(void *user, uint32_t block, uint8_t *buf);
    bool (*card_write)(void *user, uint32_t block, const uint8_t *buf);
    /* A Cybiko program built for this emulator ran CY_APP_OP (see
     * cybiko_app_call). `fn` and `arg` are ER0 and ER1; the result goes
     * back in ER0. cybiko_run returns soon after, so the host can act on it
     * (the browser launcher asks the host to open the browser). */
    uint32_t (*app_call)(void *user, uint32_t fn, uint32_t arg);
} cy_host_t;

/* ---- peripheral state (internal, visible for tests and save states) ---- */

/* The last CPU read of a timer counter or serial status, to spot loops
 * waiting on it. */
typedef struct {
    uint32_t pc;
    uint16_t value;
    uint64_t at;
} cy_poll_t;

typedef struct {
    uint8_t tcr, tmdr, tior[2], tier, tsr;
    uint16_t tcnt, tgr[4];
    uint32_t acc;              /* states not yet turned into counts */
    cy_poll_t poll;
} cy_tpu_ch_t;

typedef struct {
    uint8_t tcr, tcsr, tcor[2], tcnt;
    uint32_t acc;
    cy_poll_t poll;
} cy_tmr_ch_t;

typedef struct {
    uint8_t smr, brr, scr, ssr, ssr_read, tdr, rdr, tsr;
    bool tx_busy, rx_busy;
    uint32_t tx_left, rx_left; /* states until the frame in flight finishes */
    cy_poll_t poll;            /* last SSR read (poll loop detection) */
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
    uint32_t writes;           /* counts the changes, to see when a reflash ends */
} cy_flash_t;

/* One DMA subchannel (or a full-address channel, kept in the B slot) while a
 * transfer is set up; see cy_dma.c. */
typedef struct {
    bool active, fae, word, block, block_dest, repeat, idle, mar_dest, tend, eat;
    int8_t trigger;            /* interrupt vector that requests a unit, or < 0 */
    int8_t incs, incd;
    uint32_t src, dst, count, bcount, blen;
} cy_dma_sub_t;

#define CY_RF_SHORT 50
#define CY_RF_LONG  200
#define CY_RF_QUEUE 4

/* The radio co-processor (cy_rf.c). */
typedef struct {
    uint8_t parse, cmd, channel;
    uint16_t have, need;
    uint8_t buf[1 + CY_RF_LONG + 1];
    bool ack_due;
    uint32_t ack_at;
    uint8_t q[CY_RF_QUEUE][CY_RF_LONG];        /* packets heard, waiting */
    uint8_t q_len[CY_RF_QUEUE], q_channel[CY_RF_QUEUE];
    uint8_t q_head, queued;
    uint8_t inject[1 + CY_RF_LONG];            /* packet being handed to SCI2 */
    uint16_t inject_len, inject_pos;
    uint32_t inject_at;                        /* when its next byte arrives */
    uint8_t id_orig[4], crc_orig[4];           /* flash bytes before cybiko_set_unit_id */
    bool id_applied;
    uint32_t sent, heard;                      /* packets, for diagnostics */
    uint64_t led_red, led_green;               /* when the LED last blinked (states) */
} cy_rf_t;

/* The MP3 player cartridge (cy_cart.c). */
#define CY_MP3_FIFO 2048
typedef struct {
    bool present;
    uint8_t p2_out, p2_ddr, p3_out, p3_ddr;
    /* the decoder's I2C slave */
    bool i2c_scl, i2c_sda, i2c_active, i2c_read, i2c_ack, i2c_drive, i2c_out;
    uint8_t i2c_bits, i2c_sr, i2c_tx;
    uint16_t i2c_n;
    uint8_t i2c_msg[8];                /* the first bytes written after the address */
    uint16_t codec[0x20];              /* the decoder's codec registers (6c writes) */
    /* the SD card (SPI mode); the host sets card_blocks, 0 = no card */
    uint32_t card_blocks;
    uint8_t sd_cmd[6], sd_cmd_len;
    bool sd_idle, sd_app, sd_multi;
    uint8_t sd_wmode;                  /* 1 single block write, 2 multiple */
    uint16_t sd_wpos;
    uint32_t sd_block;
    uint8_t sd_wbuf[512];
    uint8_t sd_out[520];
    int sd_out_len, sd_out_pos;
    /* the decoder's MPEG input; the host takes bytes with cybiko_mp3_take */
    bool mas_on;                       /* DEMAND works once the decoder is set up */
    bool mas_demand;
    uint8_t mp3[CY_MP3_FIFO];
    uint16_t mp3_head, mp3_count;
} cy_cart_t;

/* The USBN9604 USB device controller (cy_usb.c). FIFO 0 is endpoint 0's
 * (8 bytes each way), FIFOs 1-3 the 64-byte ones. */
typedef struct {
    uint8_t addr;                      /* register picked through 200003 */
    uint8_t r[64];
    uint8_t maev, fnh_latch;
    uint16_t frame;
    uint8_t tx[4][64], tx_len[4], txs[4];
    uint8_t rx[4][64], rx_len[4], rx_pos[4], rxs[4];
    bool vbus;                         /* a host's cable is in */
    uint32_t overflows;                /* bytes written to a full FIFO */
} cy_usb_t;

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
    cy_rf_t rf;
    cy_cart_t cart;
    cy_usb_t usb;
    uint8_t scmr[3];
    uint8_t adcsr, adcr;
    uint16_t addr[4];
    uint32_t adc_left;
    int adc_ch;

    cy_dma_sub_t dma[4];
    bool dma_waiting;          /* a transfer waits for a peripheral interrupt */
    bool dxc;                  /* the DMAC or DTC (not the CPU) is on the bus */
    bool dma_running;          /* inside cy_dma_trigger */
    uint8_t dma_stepping;      /* transfers moving a unit now, by id */
    uint32_t dma_again[3];     /* interrupts transfers raised meanwhile, by vector */

    uint64_t synced;
    bool in_sync;           /* CPU state count the peripherals have reached */
    /* The timers (TPU, TMR, WDT) only catch up when one of their registers
     * is accessed or their next event (timers_due) comes. */
    uint64_t timers_synced, timers_due;

    /* speaker (P1 bit 5, TIOCB1) */
    uint32_t spk_edges;        /* P15 toggles written by software */
    uint8_t spk_pin;           /* last port 1 output level */
    uint64_t spk_since;        /* start of the current edge-count window */
    float spk_freq, spk_duty;  /* last tone reported to the host */

    /* bigger RAM disk (cy_disk.c) */
    uint8_t *disk;
    uint32_t disk_size;
    uint32_t disk_dirty[CY_DISK_MAX / CY_DISK_CHUNK / 32];  /* chunks written */
    uint8_t con_match;         /* progress through "Starting 0x" on SCI1 */
    uint8_t disk_patched;      /* kernel patches applied since power-on */
    uint32_t hle_pc[CY_HLE_SLOTS];  /* traps of the routines run natively (cy_hle.c), ~0 if none */
    uint32_t hle_scanned;            /* 64 KB pages of RAM cy_hle_probe searched lately */
    uint64_t hle_scan_reset, hle_sample_at;
    /* Debug: called for every on-chip register access when set (PC runner). */
    void (*io_trace)(struct cybiko *m, uint32_t a, uint8_t v, bool write);
    void (*cart_log)(struct cybiko *m, const char *msg);

    /* diagnostics: things a ROM used that the model does not cover */
    uint32_t unmapped_reads, unmapped_writes, illegal_ops;
    uint32_t dtc_dma_use;      /* DMA transfers started by the DREQ pins (not wired) */
    uint32_t wdt_resets;
    bool app_called;           /* cybiko_run stops early after an app call */
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

/* Radio (cy_rf.c). A packet heard from another machine, as its rf_send hook
 * gave it; returns false if the queue is full or the length is wrong. */
bool cybiko_rf_receive(cybiko_t *m, const uint8_t *pkt, size_t len, uint8_t channel);
/* The status LED, as the Xtreme's manual describes it: blinking red while
 * the radio sends, blinking green while it hears other Cybikos (each blink
 * lasts CY_LED_BLINK_MS). Steady red on mains or USB power is not shown. */
enum { CY_LED_OFF, CY_LED_RED, CY_LED_GREEN };
#define CY_LED_BLINK_MS 100
int cybiko_led(const cybiko_t *m);
/* The MP3 cartridge: take up to `max` bytes of the MPEG stream CyOS sent to
 * the decoder (call it as the audio output needs data). */
size_t cybiko_mp3_take(cybiko_t *m, uint8_t *buf, size_t max);
/* The player's volume setting (codec register 0010's high byte): 0 is mute,
 * CyOS's default is 6a, louder is higher (taken as 1 dB a step, a guess). */
uint8_t cybiko_mp3_volume(const cybiko_t *m);
/* Give this machine its own Cybiko ID (all copies of a flash dump share
 * one, and CyOS ignores packets from its own ID) by rewriting the ID record
 * in the flash image. Call before the machine boots; 0 keeps the dump's. */
void cybiko_set_unit_id(cybiko_t *m, uint32_t id);
/* The ID in the flash image. */
uint32_t cybiko_unit_id(const cybiko_t *m);
/* Undo the ID change in a copy of the flash about to be saved. */
void cybiko_flash_original(const cybiko_t *m, uint8_t *copy);

/* The per-machine record (magic, Cybiko ID, CRC) at the top of the flash. */
#define CY_FLASH_ID_RECORD 0x7f800u
enum { CY_BOOT_OK, CY_BOOT_BAD_IMAGE, CY_BOOT_NO_LOADER, CY_BOOT_TOO_BIG };
/* Stage a CyOS update image (a .boot file, magic ffc0ffab, such as the
 * Russian ccyphRU1.5.08r.boot) in a flash image before the machine boots:
 * the flash's own loader then installs it into the flash on the next start,
 * the way a real Xtreme does after EZ Loader sends one. The flash must hold
 * a CyOS (its loader is used); everything between the loader and the ID
 * record is replaced. Returns a CY_BOOT_ value. */
int cybiko_flash_stage_boot(uint8_t *flash, const uint8_t *boot, size_t len);

/* Programs made for this emulator (the Browser launcher from the
 * cybiko-browser project) call the host with an
 * opcode the H8S leaves undefined: the word CY_APP_OP, with the function in
 * ER0 and its argument in ER1; the host's app_call result comes back in ER0.
 * Real hardware would take it as a NOP, and the programs are only offered
 * by the emulator. */
#define CY_APP_OP 0x0042
enum {
    CY_APP_VERSION = 0,        /* returns CY_APP_API */
    CY_APP_BROWSER = 1,        /* open the web browser; returns when it closes */
};
#define CY_APP_API 1

/* Show a picture of the host's own on the LCD while CyOS is not running (the
 * browser): CY_LCD_H rows of CY_LCD_W / 4 bytes from the top, four pixels a
 * byte with the leftmost in the top bits, 0 the lightest pen to 3 the
 * darkest. Save lcd.ram first and copy it back (and bump lcd.version)
 * to give the screen back to CyOS. */
void cybiko_lcd_show(cybiko_t *m, const uint8_t *picture);

/* Render the LCD as 8-bit luminance (0 black, 255 white), CY_LCD_W x
 * CY_LCD_H, row 0 at the top. Returns lcd.version so callers can skip
 * unchanged frames. */
uint32_t cybiko_lcd_render(const cybiko_t *m, uint8_t *lum);

/* ---- bigger RAM disk ----
 * CyOS 1.5 keeps user files in a RAM disk of 512 KB at 400000. After
 * cybiko_set_disk the machine moves it to CY_DISK_BASE and gives it `size`
 * bytes (a multiple of 64 KB, at most CY_DISK_MAX): when the boot loader
 * prints "Starting 0x" on SCI1 the freshly unpacked kernel is patched before
 * it runs, so its RAM disk driver uses the new place and size. The disk keeps
 * the RAM disk's format (pages of 258 bytes from the start), so a 512 KB
 * ramdisk.bin loaded at the start of `disk` keeps its files. Call after
 * cybiko_init. Writes set bits in disk_dirty, one per CY_DISK_CHUNK. */
void cybiko_set_disk(cybiko_t *m, uint8_t *disk, uint32_t size);

/* Files in a RAM disk image (`size` bytes, the old 512 KB one or a bigger
 * disk), for use while the machine is not running. Names are at most
 * CY_DISK_NAME_MAX characters. */
#define CY_DISK_NAME_MAX 58
typedef struct {
    char name[CY_DISK_NAME_MAX + 1];
    uint32_t size;
    uint16_t id;
} cy_disk_file_t;
/* Calls fn for each file; stops early when fn returns false. */
void cy_disk_list(const uint8_t *disk, uint32_t size,
                  bool (*fn)(void *user, const cy_disk_file_t *f), void *user);
bool cy_disk_find(const uint8_t *disk, uint32_t size, const char *name, cy_disk_file_t *out);
/* Bytes that can still be stored in one new file. */
uint32_t cy_disk_free(const uint8_t *disk, uint32_t size);
/* Store a file, replacing one with the same name. `date` is in seconds since
 * 1900-01-01, as CyOS keeps it. Returns 0, -1 when it does not fit (nothing
 * changes) or -2 for a bad name. */
int cy_disk_write(uint8_t *disk, uint32_t size, const char *name,
                  const uint8_t *data, uint32_t len, uint32_t date);
/* Read a file into buf (at most cap bytes). Returns its length or -1. */
int32_t cy_disk_read(const uint8_t *disk, uint32_t size, const char *name,
                     uint8_t *buf, uint32_t cap);
bool cy_disk_delete(uint8_t *disk, uint32_t size, const char *name);

/* True once the OS has put the CPU into software standby (switched off). */
static inline bool cybiko_is_off(const cybiko_t *m) { return m->standby; }

/* USB, the host's side: one transaction per call, to the device at USB
 * address addr. They answer what the chip would: ACK, NAK, STALL, or
 * nothing (TIMEOUT: detached, not addressed, no such endpoint). toggle is
 * the DATA0/1 bit of the packet. cybiko_usb_vbus plugs the cable in;
 * CyOS then puts its pull-up on (cybiko_usb_attached) and waits for a bus
 * reset. */
enum { CY_USB_ACK, CY_USB_NAK, CY_USB_STALL, CY_USB_TIMEOUT };
void cybiko_usb_vbus(cybiko_t *m, bool on);
bool cybiko_usb_attached(const cybiko_t *m);
void cybiko_usb_bus_reset(cybiko_t *m);
void cybiko_usb_frame(cybiko_t *m);            /* a start of frame, every 1 ms */
int cybiko_usb_setup(cybiko_t *m, int addr, const uint8_t setup[8]);
int cybiko_usb_out(cybiko_t *m, int addr, int ep, const uint8_t *data, int len, int toggle);
int cybiko_usb_in(cybiko_t *m, int addr, int ep, uint8_t *data, int max, int *len, int *toggle);
