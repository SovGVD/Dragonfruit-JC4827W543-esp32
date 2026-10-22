/* Tests for the Cybiko Xtreme machine model: small H8S programs (sources in
 * cybiko_progs/) exercise interrupts and peripherals, and the board devices
 * are driven through the CPU's view of the bus. No Cybiko ROM is needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cybiko.h"
#include "cybiko_progs.h"
#include "cy_internal.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint8_t rom[CY_ROM_BUF], flash[CY_FLASH_SIZE];
static uint8_t *ram;
static cybiko_t m;

/* serial capture */
static char tx_buf[64];
static uint64_t tx_time[64];
static int tx_len;
static void on_tx(void *user, int port, uint8_t b)
{
    (void)user;
    if (port == 2 && tx_len < 64) {
        tx_time[tx_len] = h8s_time(&m.cpu);
        tx_buf[tx_len++] = (char)b;
    }
}

static float spk_freq, spk_duty;
static int spk_calls;
static void on_speaker(void *user, float f, float duty)
{
    (void)user;
    spk_freq = f;
    spk_duty = duty;
    spk_calls++;
}

static int64_t fake_now = 1700000000;  /* 2023-11-14 22:13:20 UTC, a Tuesday */
static int64_t wall(void *user) { (void)user; return fake_now; }

static void boot(const unsigned char *prog, size_t len)
{
    memset(rom, 0, sizeof rom);
    memcpy(rom, prog, len);
    memset(flash, 0xff, sizeof flash);
    memset(ram, 0, CY_RAM_SIZE);
    tx_len = 0;
    spk_freq = spk_duty = 0;
    spk_calls = 0;
    cy_host_t host = { .serial_tx = on_tx, .wall_time = wall, .speaker = on_speaker };
    cybiko_init(&m, &host, rom, flash, ram);
}

static void test_tpu_interrupts(void)
{
    boot(prog_tpu_irq, sizeof prog_tpu_irq);
    cybiko_run(&m, 100500);
    printf("tpu: %u interrupts in 100500 states\n", (unsigned)m.cpu.er[5]);
    CHECK(m.cpu.er[5] >= 99 && m.cpu.er[5] <= 101);
    CHECK(m.cpu.intm2);
    CHECK(m.illegal_ops == 0);
}

static void test_tmr_interrupts(void)
{
    boot(prog_tmr_irq, sizeof prog_tmr_irq);
    cybiko_run(&m, 80400);
    printf("tmr: %u interrupts in 80400 states\n", (unsigned)m.cpu.er[5]);
    CHECK(m.cpu.er[5] >= 99 && m.cpu.er[5] <= 101);
}

static void test_sci_tx(void)
{
    boot(prog_sci_tx, sizeof prog_sci_tx);
    cybiko_run(&m, 20000);
    CHECK(tx_len == 3 && memcmp(tx_buf, "OK\n", 3) == 0);
    /* 57600 baud 8N1 is 3200 states per byte at 18.432 MHz */
    if (tx_len == 3) {
        CHECK(tx_time[1] - tx_time[0] >= 3190 && tx_time[1] - tx_time[0] <= 3300);
        CHECK(tx_time[2] - tx_time[1] >= 3190 && tx_time[2] - tx_time[1] <= 3300);
    }
}

static void test_sci_rx(void)
{
    boot(prog_sci_rx, sizeof prog_sci_rx);
    cybiko_run(&m, 1000);
    CHECK(cybiko_serial_rx(&m, 2, 'a'));
    CHECK(cybiko_serial_rx(&m, 2, 'b'));
    CHECK(cybiko_serial_rx(&m, 2, 'c'));
    cybiko_run(&m, 3 * 3200 + 1000);
    CHECK(memcmp(ram, "abc", 3) == 0);
    CHECK(m.cpu.er[6] == 0x400003);
    CHECK((m.sci[2].ssr & 0x20) == 0);   /* no overrun */
}

static void test_keyboard(void)
{
    boot(prog_spin, sizeof prog_spin);
    /* Row r is selected by a 0 in bit r of the word offset. */
    uint32_t row3 = 0xe00000u + 2u * (0x7fffu & ~(1u << 3));
    CHECK(h8s_read16(&m.cpu, row3) == 0xffff);
    cybiko_key(&m, CYK_A, true);
    CHECK(h8s_read16(&m.cpu, row3) == (uint16_t)~0x0004);
    CHECK(h8s_read16(&m.cpu, 0xe00000u + 2u * (0x7fffu & ~(1u << 4))) == 0xffff);
    CHECK(h8s_read8(&m.cpu, row3 + 1) == 0xfb);
    cybiko_key(&m, CYK_A, false);
    CHECK(h8s_read16(&m.cpu, row3) == 0xffff);
    /* On/Off also shows on PA7 (0 = pressed) */
    CHECK((h8s_read8(&m.cpu, 0xffff59) & 0xc0) == 0xc0);
    cybiko_key(&m, CYK_ONOFF, true);
    CHECK((h8s_read8(&m.cpu, 0xffff59) & 0xc0) == 0x40);
}

static void lcd_reg(uint8_t r, uint8_t v) { h8s_write16(&m.cpu, 0x100000, (uint16_t)(r << 8 | v)); }

static void test_lcd(void)
{
    static uint8_t lum[CY_LCD_W * CY_LCD_H];
    boot(prog_spin, sizeof prog_spin);
    cybiko_lcd_render(&m, lum);
    CHECK(lum[0] == 255);                        /* display off: white */
    lcd_reg(0x00, 0x40);                         /* display on */
    lcd_reg(0x10, 0x00);                         /* contrast */
    lcd_reg(0x0c, 0x00);                         /* palette: 0 white .. 3 black */
    lcd_reg(0x0d, 0x0b);
    lcd_reg(0x0e, 0x16);
    lcd_reg(0x0f, 0x1f);
    lcd_reg(0x01, 0x02);                         /* increment x */
    lcd_reg(0x02, 0x00);
    lcd_reg(0x03, 0x00);
    lcd_reg(0x04, 0x1b);                         /* pixels 0,1,2,3 */
    lcd_reg(0x04, 0xff);
    uint32_t v = cybiko_lcd_render(&m, lum);
    const uint8_t *bottom = lum + (CY_LCD_H - 1) * CY_LCD_W;
    /* MAME: brightness = 31 - (COLOR - CONTRAST + 3), clamped */
    CHECK(bottom[0] == 255 * 28 / 31);
    CHECK(bottom[1] == 255 * 17 / 31);
    CHECK(bottom[2] == 255 * 6 / 31);
    CHECK(bottom[3] == 0);
    CHECK(bottom[4] == 0 && bottom[7] == 0);
    CHECK(bottom[8] == 255 * 28 / 31);            /* not written: index 0 */
    CHECK(m.lcd.x == 2 && m.lcd.y == 0);
    lcd_reg(0x04, 0xff);
    CHECK(cybiko_lcd_render(&m, lum) != v);
    /* index/data read back */
    h8s_write8(&m.cpu, 0x100000, 0x0d);
    CHECK(h8s_read8(&m.cpu, 0x100001) == 0x0b);
}

static void test_flash(void)
{
    boot(prog_spin, sizeof prog_spin);
    flash[0x1000] = 0x12; flash[0x1001] = 0x34;
    CHECK(h8s_read16(&m.cpu, 0x601000) == 0x1234);
    CHECK(h8s_read16(&m.cpu, 0x681000) == 0x1234);  /* mirror */
    /* software ID */
    h8s_write16(&m.cpu, 0x600000 + 0x5555 * 2, 0xaa);
    h8s_write16(&m.cpu, 0x600000 + 0x2aaa * 2, 0x55);
    h8s_write16(&m.cpu, 0x600000 + 0x5555 * 2, 0x90);
    CHECK(h8s_read16(&m.cpu, 0x600000) == 0x00bf);
    CHECK(h8s_read16(&m.cpu, 0x600002) == 0x2780);
    h8s_write16(&m.cpu, 0x600000, 0xf0);
    CHECK(h8s_read16(&m.cpu, 0x600000) == 0xffff);
    /* word program clears bits only */
    h8s_write16(&m.cpu, 0x600000 + 0x5555 * 2, 0xaa);
    h8s_write16(&m.cpu, 0x600000 + 0x2aaa * 2, 0x55);
    h8s_write16(&m.cpu, 0x600000 + 0x5555 * 2, 0xa0);
    h8s_write16(&m.cpu, 0x600010, 0xa55a);
    CHECK(h8s_read16(&m.cpu, 0x600010) == 0xa55a);
    CHECK(m.flash.dirty);
    h8s_write16(&m.cpu, 0x600012, 0x0000);        /* no unlock: ignored */
    CHECK(h8s_read16(&m.cpu, 0x600012) == 0xffff);
    /* sector erase of the 4 KB sector at 0x1000 */
    h8s_write16(&m.cpu, 0x600000 + 0x5555 * 2, 0xaa);
    h8s_write16(&m.cpu, 0x600000 + 0x2aaa * 2, 0x55);
    h8s_write16(&m.cpu, 0x600000 + 0x5555 * 2, 0x80);
    h8s_write16(&m.cpu, 0x600000 + 0x5555 * 2, 0xaa);
    h8s_write16(&m.cpu, 0x600000 + 0x2aaa * 2, 0x55);
    h8s_write16(&m.cpu, 0x601234, 0x30);
    CHECK(h8s_read16(&m.cpu, 0x601000) == 0xffff);
    CHECK(h8s_read16(&m.cpu, 0x600010) == 0xa55a);  /* other sector kept */
}

/* Bit-banged I2C on port F as the Cybiko wires it (MAME xtclock_r/w):
 * PF1 is SCL; SDA reads on PF6, and is released (high) only while PF6 is
 * an output driving 0. */
static uint8_t pfdr, pfddr;
static void pf_update(void)
{
    h8s_write8(&m.cpu, 0xfffebe, pfddr);
    h8s_write8(&m.cpu, 0xffff6e, pfdr);
}
static void scl(int v) { pfddr |= 0x02; pfdr = (uint8_t)(v ? pfdr | 0x02 : pfdr & ~0x02); pf_update(); }
static void sda(int v) { pfddr |= 0x40; pfdr = (uint8_t)(v ? pfdr & ~0x40 : pfdr | 0x40); pf_update(); }
static void i2c_start(void) { sda(1); scl(1); sda(0); scl(0); }
static void i2c_stop(void) { sda(0); scl(1); sda(1); }
static void i2c_write(uint8_t b)
{
    for (int i = 7; i >= 0; i--) { sda(b >> i & 1); scl(1); scl(0); }
    sda(1); scl(1); scl(0);                       /* ACK clock */
}
static uint8_t i2c_read(int last)
{
    uint8_t b = 0;
    pfddr &= (uint8_t)~0x40; pf_update();         /* PF6 as input */
    for (int i = 0; i < 8; i++) {
        scl(1);
        b = (uint8_t)(b << 1 | ((h8s_read8(&m.cpu, 0xffff5e) >> 6) & 1));
        scl(0);
    }
    if (last) sda(1);                             /* NAK */
    else { pfddr &= (uint8_t)~0x40; pf_update(); }  /* ACK (line low) */
    scl(1); scl(0);
    return b;
}

static void rtc_read_regs(uint8_t *r, int n)
{
    i2c_start(); i2c_write(0xa2); i2c_write(0x01);   /* set position 1 */
    i2c_stop();
    i2c_start(); i2c_write(0xa3);
    for (int i = 0; i < n; i++) r[i] = i2c_read(i == n - 1);
    i2c_stop();
}

static void test_rtc(void)
{
    boot(prog_spin, sizeof prog_spin);
    pfdr = pfddr = 0;
    uint8_t r[6];
    rtc_read_regs(r, 6);
    /* 2023-11-14 22:13:20 Tuesday; year register holds 2023 % 4 = 3 */
    printf("rtc: %02x %02x %02x %02x %02x %02x\n", r[0], r[1], r[2], r[3], r[4], r[5]);
    CHECK(r[1] == 0x20 && r[2] == 0x13 && r[3] == 0x22);
    CHECK(r[4] == (3 << 6 | 0x14));
    CHECK(r[5] == (2 << 5 | 0x11));

    /* set 2024-02-29 08:30:00, then read back 5 s later */
    i2c_start(); i2c_write(0xa2); i2c_write(0x02);
    i2c_write(0x00); i2c_write(0x30); i2c_write(0x08);
    i2c_write(0 << 6 | 0x29); i2c_write(4 << 5 | 0x02);
    i2c_stop();
    fake_now += 5;
    rtc_read_regs(r, 6);
    printf("rtc: %02x %02x %02x %02x %02x %02x\n", r[0], r[1], r[2], r[3], r[4], r[5]);
    CHECK(r[1] == 0x05 && r[2] == 0x30 && r[3] == 0x08);
    CHECK(r[4] == (0 << 6 | 0x29));
    CHECK((r[5] & 0x1f) == 0x02 && (r[5] >> 5) == 4);  /* Thursday */
}

static void test_watchdog(void)
{
    boot(prog_spin, sizeof prog_spin);
    /* interval mode, phi/2: overflows after 512 states */
    h8s_write16(&m.cpu, 0xffffbc, 0xa520);
    cybiko_run(&m, 400);
    CHECK(!(h8s_read8(&m.cpu, 0xffffbc) & 0x80));
    cybiko_run(&m, 200);
    CHECK(h8s_read8(&m.cpu, 0xffffbc) & 0x80);
    /* byte writes are ignored */
    h8s_write8(&m.cpu, 0xffffbc, 0x00);
    CHECK(h8s_read8(&m.cpu, 0xffffbc) & 0x20);
    /* watchdog mode with reset enabled resets the chip */
    h8s_write16(&m.cpu, 0xffffbe, 0x5a40);
    h8s_write16(&m.cpu, 0xffffbc, 0xa560);
    cybiko_run(&m, 1000);
    CHECK(m.wdt_resets == 1);
    CHECK(h8s_read8(&m.cpu, 0xffffbf) & 0x80);    /* WOVF */
    CHECK(!(h8s_read8(&m.cpu, 0xffffbc) & 0x20)); /* timer stopped by reset */
}

static void test_memory_map(void)
{
    boot(prog_spin, sizeof prog_spin);
    CHECK(h8s_read16(&m.cpu, 0x008000) == 0x0000);   /* ROM mirror */
    CHECK(h8s_read16(&m.cpu, 0x038002) == 0x0200);
    h8s_write16(&m.cpu, 0x400000, 0xbeef);
    CHECK(ram[0] == 0xbe && ram[1] == 0xef);
    h8s_write32(&m.cpu, 0xfffbfc, 0x12345678);       /* top of on-chip RAM */
    CHECK(h8s_read32(&m.cpu, 0xfffbfc) == 0x12345678);
    CHECK(m.iram[0x1ffc] == 0x12);
    h8s_write8(&m.cpu, 0x000010, 0x55);              /* ROM is read-only */
    CHECK(rom[0x10] == 0);
    CHECK(h8s_read8(&m.cpu, 0xffff3b) == 0x87);      /* MDCR */
    CHECK(h8s_read8(&m.cpu, 0xffff39) == 0x01);      /* SYSCR */
}

static void test_speaker(void)
{
    boot(prog_tpu_tone, sizeof prog_tpu_tone);
    cybiko_run(&m, 10000);
    printf("speaker (TPU): %.1f Hz duty %.2f\n", spk_freq, spk_duty);
    CHECK(spk_freq > 999.0f && spk_freq < 1001.0f);
    CHECK(spk_duty == 0.5f);

    boot(prog_beep_soft, sizeof prog_beep_soft);
    for (int i = 0; i < 10; i++) cybiko_run(&m, CY_CLOCK_HZ / 100);
    printf("speaker (software): %.1f Hz\n", spk_freq);
    CHECK(spk_freq > 7800.0f && spk_freq < 8200.0f);

    boot(prog_spin, sizeof prog_spin);
    for (int i = 0; i < 5; i++) cybiko_run(&m, CY_CLOCK_HZ / 100);
    CHECK(spk_calls == 0);                       /* silent machine: no calls */
}

static void test_dma(void)
{
    boot(prog_spin, sizeof prog_spin);
    /* Full address mode, auto-request burst, as CyOS does at start-up:
     * 12 words from a fixed source into on-chip RAM, fixed. */
    h8s_write16(&m.cpu, 0x400100, 0xbeef);
    h8s_write32(&m.cpu, 0xfffef0, 0x400100);         /* MAR1A: source */
    h8s_write32(&m.cpu, 0xfffef8, 0xffdc00);         /* MAR1B: destination */
    h8s_write16(&m.cpu, 0xfffef6, 12);               /* ETCR1A */
    h8s_write8(&m.cpu, 0xffff06, 0x80);              /* FAE1 */
    h8s_write8(&m.cpu, 0xffff04, 0x80);              /* DMACR1A: word, fixed */
    h8s_write8(&m.cpu, 0xffff05, 0x07);              /* DMACR1B: auto-request burst */
    h8s_write8(&m.cpu, 0xffff07, 0x80);              /* DTME1 */
    CHECK(h8s_read16(&m.cpu, 0xffdc00) == 0);
    h8s_write8(&m.cpu, 0xffff07, 0xc0);              /* DTE1: go */
    CHECK(h8s_read16(&m.cpu, 0xffdc00) == 0xbeef);
    CHECK(h8s_read8(&m.cpu, 0xffff07) == 0x80);      /* DTE1 cleared, DTME1 kept */
    CHECK(h8s_read16(&m.cpu, 0xfffef6) == 0);

    /* Full address, incrementing both sides, with the end interrupt. */
    for (int i = 0; i < 4; i++) h8s_write16(&m.cpu, 0x400200 + 2 * i, (uint16_t)(0x1111 * (i + 1)));
    h8s_write32(&m.cpu, 0xfffef0, 0x400200);
    h8s_write32(&m.cpu, 0xfffef8, 0x400300);
    h8s_write16(&m.cpu, 0xfffef6, 4);
    h8s_write8(&m.cpu, 0xffff04, 0xa0);              /* word, SAIDE: source++ */
    h8s_write8(&m.cpu, 0xffff05, 0x27);              /* DAIDE: destination++, burst */
    h8s_write8(&m.cpu, 0xffff07, 0xc4);              /* DTME1, DTE1, DTIE1A */
    for (int i = 0; i < 4; i++) CHECK(h8s_read16(&m.cpu, 0x400300 + 2 * i) == 0x1111 * (i + 1));
    CHECK(h8s_read16(&m.cpu, 0x400308) == 0);
    CHECK(m.pend[74 >> 5] & 1u << (74 & 31));         /* DEND1A */

    /* Short address mode, started by SCI0 TXI, DTA swallows the interrupt. */
    m.pend[82 >> 5] = 0;
    h8s_write8(&m.cpu, 0x400400, 0x5a);
    h8s_write8(&m.cpu, 0x400401, 0xa5);
    h8s_write32(&m.cpu, 0xfffee0, 0x400400);         /* MAR0A */
    h8s_write16(&m.cpu, 0xfffee4, 0xdc10);           /* IOAR0A: on-chip RAM */
    h8s_write16(&m.cpu, 0xfffee6, 2);                /* ETCR0A */
    h8s_write8(&m.cpu, 0xffff02, 0x04);              /* DMACR0A: byte, MAR to IOAR, TXI0 */
    h8s_write8(&m.cpu, 0xffff06, 0x81);              /* keep FAE1, DTA0A */
    h8s_write8(&m.cpu, 0xffff07, 0x10);              /* DTE0A */
    CHECK(h8s_read8(&m.cpu, 0xffdc10) == 0);
    cy_irq(&m, 82);
    CHECK(h8s_read8(&m.cpu, 0xffdc10) == 0x5a);
    CHECK(!(m.pend[82 >> 5] & 1u << (82 & 31)));
    cy_irq(&m, 82);
    CHECK(h8s_read8(&m.cpu, 0xffdc10) == 0xa5);
    CHECK(!(h8s_read8(&m.cpu, 0xffff07) & 0x10));    /* done */
    cy_irq(&m, 82);                                  /* now reaches the CPU */
    CHECK(m.pend[82 >> 5] & 1u << (82 & 31));
}

/* Files in a RAM disk image, and moving the RAM disk out of main RAM. */
static void test_disk(void)
{
    static uint8_t disk[CY_RAMDISK_SIZE], data[1000], back[1200];
    memset(disk, 0, sizeof disk);
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7);
    uint32_t free0 = cy_disk_free(disk, sizeof disk);
    CHECK(free0 == 178 + (CY_RAMDISK_SIZE / 258 - 5 - 1) * 250);
    CHECK(cy_disk_write(disk, sizeof disk, "game.app", data, sizeof data, 3155673600u) == 0);
    CHECK(cy_disk_write(disk, sizeof disk, "notes.txt", (const uint8_t *)"hi", 2, 0) == 0);
    cy_disk_file_t f;
    CHECK(cy_disk_find(disk, sizeof disk, "game.app", &f) && f.size == 1000 && f.id == 0);
    CHECK(cy_disk_find(disk, sizeof disk, "notes.txt", &f) && f.size == 2 && f.id == 5);
    CHECK(cy_disk_read(disk, sizeof disk, "game.app", back, sizeof back) == 1000);
    CHECK(memcmp(back, data, sizeof data) == 0);
    /* first page of game.app: header fields as CyOS writes them */
    const uint8_t *b = disk + 5 * 258 + 2;
    CHECK(b[0] == 0x80 && b[1] == 178 && b[6] == 0x20 && !strcmp((const char *)b + 7, "game.app"));
    CHECK(b[0x45] == 0x7b && b[0x46] == 0xff && b[0x47] == 0xff);
    CHECK(b[0x4a] == 0xbc && b[0x4b] == 0x17 && b[0x4c] == 0xc2 && b[0x4d] == 0x00);
    CHECK(cy_disk_free(disk, sizeof disk) == free0 - 6 * 250);
    /* replacing reuses the freed pages; the id follows the first block */
    CHECK(cy_disk_write(disk, sizeof disk, "game.app", data, 100, 0) == 0);
    CHECK(cy_disk_find(disk, sizeof disk, "game.app", &f) && f.size == 100 && f.id == 0);
    CHECK(cy_disk_read(disk, sizeof disk, "notes.txt", back, sizeof back) == 2 && !memcmp(back, "hi", 2));
    CHECK(cy_disk_delete(disk, sizeof disk, "notes.txt"));
    CHECK(!cy_disk_find(disk, sizeof disk, "notes.txt", NULL));
    CHECK(cy_disk_write(disk, sizeof disk, "huge", disk, sizeof disk, 0) == -1);
    CHECK(!cy_disk_find(disk, sizeof disk, "huge", NULL));
    CHECK(cy_disk_write(disk, sizeof disk, "", data, 1, 0) == -2);

    /* The kernel's RAM disk set-up is patched when the loader says
     * "Starting 0x"; the disk then answers at CY_DISK_BASE. */
    static const unsigned char idle[] = { 0x00, 0x00, 0x01, 0x00, 0x40, 0xfe };  /* reset vector, bra . */
    boot(idle, sizeof idle);
    static const uint8_t ctor[] = {
        0x7a, 0x02, 0x00, 0x00, 0x07, 0xd0, 0x01, 0x00, 0x6d, 0xf2, 0x79, 0x02, 0x01, 0x00,
        0x01, 0x00, 0x6d, 0xf2, 0x7a, 0x02, 0x00, 0x08, 0x00, 0x00, 0x7a, 0x01, 0x00, 0x40, 0x00, 0x00,
    };
    memcpy(ram + 0xa3512, ctor, sizeof ctor);
    static uint8_t big[0x200000];
    cybiko_set_disk(&m, big, sizeof big);
    for (const char *c = "Starting 0x4a3bf8...\n"; *c; c++) cy_disk_console(&m, (uint8_t)*c);
    CHECK(m.disk_patched == 1);
    const uint8_t *k = ram + 0xa3512;
    CHECK(k[4] == 0x7f && k[5] == 0xff);
    CHECK(k[20] == 0x00 && k[21] == 0x20 && k[22] == 0x00 && k[23] == 0x00);
    CHECK(k[26] == 0x00 && k[27] == 0x80 && k[28] == 0x00 && k[29] == 0x00);
    h8s_write16(&m.cpu, CY_DISK_BASE + 0x1234, 0xabcd);
    h8s_write8(&m.cpu, CY_DISK_BASE + 0x1fffff, 0x5a);
    CHECK(big[0x1234] == 0xab && big[0x1235] == 0xcd && big[0x1fffff] == 0x5a);
    CHECK(h8s_read16(&m.cpu, CY_DISK_BASE + 0x1234) == 0xabcd);
    CHECK(m.disk_dirty[0] == 0x2 && m.disk_dirty[511 / 32] == 1u << (511 % 32));
}

/* The CyOS page checksum run natively leaves the registers the H8S code
 * leaves. */
static void test_hle(void)
{
    static const unsigned char prog[] = {
        0x00, 0x00, 0x01, 0x00,                          /* reset vector -> 000100 */
    };
    static const uint8_t code[] = {                      /* CyOS 1.5.08 page checksum */
        0x01, 0x10, 0x6d, 0xf4, 0x0f, 0x82, 0x1a, 0x80, 0x1a, 0xb3, 0x1f, 0x90, 0x4c, 0x28,
        0x0f, 0xa4, 0x6c, 0x4a, 0xf2, 0x00, 0x17, 0x72, 0x01, 0xf0, 0x65, 0x20, 0x01, 0xf0,
        0x65, 0x30, 0x10, 0x30, 0x0f, 0x82, 0x0d, 0xa2, 0x19, 0xaa, 0xea, 0x01, 0xe2, 0x00,
        0x17, 0x72, 0x01, 0xf0, 0x64, 0x20, 0x0b, 0x03, 0x1f, 0x93, 0x4d, 0xda, 0x01, 0x10,
        0x6d, 0x75, 0x54, 0x70,
    };
    static const uint8_t caller[] = {
        0x7a, 0x07, 0x00, 0xff, 0xfb, 0x00,              /* mov.l #fffb00,er7 */
        0x7a, 0x00, 0x00, 0x40, 0x10, 0x00,              /* mov.l #401000,er0 */
        0x7a, 0x01, 0x00, 0x00, 0x01, 0x00,              /* mov.l #256,er1 */
        0x5e, 0x40, 0x20, 0x00,                          /* jsr @402000 */
        0x01, 0x80,                                      /* sleep */
    };
    uint32_t er[2][4];
    for (int native = 0; native < 2; native++) {
        boot(prog, sizeof prog);
        memcpy(rom + 0x100, caller, sizeof caller);
        memcpy(ram + 0x2000, code, sizeof code);
        for (int i = 0; i < 256; i++) ram[0x1000 + i] = (uint8_t)(i * 37 + 5);
        if (native) cy_hle_patch_kernel(&m);
        CHECK(m.hle_pc[0] == (native ? 0x402000u : 0xffffffffu));   /* page sum */
        cybiko_run(&m, 100000);
        CHECK(m.cpu.sleeping && m.illegal_ops == 0);
        memcpy(er[native], m.cpu.er, sizeof er[native]);
    }
    CHECK(er[0][0] == er[1][0] && er[0][2] == er[1][2] && er[0][3] == er[1][3]);
    CHECK(er[0][0] != 0);
}

/* The kernel and ROM loops run natively (cy_hle.c) leave registers, flags,
 * memory and states exactly as the H8S code does, whether they run whole or
 * stop at every slice end. The code is copied from cy_hle.c's patterns. */
typedef struct {
    uint32_t er[8], pc;
    uint8_t ccr;
    uint64_t states;
    uint32_t sum;
} loop_result_t;

static loop_result_t run_loop(uint32_t pc, const uint32_t *er, int32_t slice)
{
    for (int i = 0; i < 8; i++) m.cpu.er[i] = er[i];
    m.cpu.pc = pc;
    m.cpu.ccr = 0x80;
    m.cpu.sleeping = false;
    loop_result_t r = { .states = 0 };
    for (int guard = 0; !m.cpu.sleeping && guard < 1000000; guard++) r.states += (uint64_t)h8s_run(&m.cpu, slice);
    memcpy(r.er, m.cpu.er, sizeof r.er);
    r.pc = m.cpu.pc;
    r.ccr = m.cpu.ccr;
    for (uint32_t i = 0; i < CY_RAM_SIZE; i++)
        if (i - 0x2000 >= 0x100) r.sum = r.sum * 31 + ram[i];   /* not the code, which has the trap */
    return r;
}

/* A run ends with the slice it sleeps in, so states compare only between
 * runs in 1-state slices. */
static bool same_result(const loop_result_t *a, const loop_result_t *b, bool states)
{
    return !memcmp(a->er, b->er, sizeof a->er) && a->pc == b->pc && a->ccr == b->ccr &&
           (!states || a->states == b->states) && a->sum == b->sum;
}

static void test_hle_loops(void)
{
    static const unsigned char prog[] = { 0x00, 0x00, 0x01, 0x00, };
    static const uint8_t fill_code[] = {
        0x0d, 0x03, 0x17, 0xf3, 0x0f, 0xd2, 0x0a, 0xb2, 0x68, 0xa9, 0x0b, 0x00, 0x1b, 0x04, 0x0f, 0xc4,
        0x46, 0xee, 0x01, 0x80,                                          /* + sleep */
    };
    static const uint8_t lz_code[] = {
        0x01, 0x00, 0x69, 0xf5, 0x09, 0x30, 0x1b, 0x53, 0x0f, 0xa5, 0x1a, 0x85, 0x66, 0xe5, 0x66, 0xbd,
        0x0a, 0x95, 0x68, 0x53, 0x0f, 0xa5, 0x66, 0xe5, 0x66, 0xbd, 0x0a, 0x95, 0x68, 0xd3, 0x0b, 0x02,
        0xf3, 0x01, 0x18, 0x3b, 0x44, 0xe2, 0x01, 0x80,
    };
    static const uint8_t stream_code[] = {
        0x0f, 0xc0, 0x7a, 0x10, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x69, 0x01, 0x0f, 0x95, 0x7a, 0x65,
        0x00, 0x00, 0x1f, 0xff, 0x0f, 0xc2, 0x0a, 0xd2, 0x6e, 0x2a, 0x00, 0x14, 0x01, 0x00, 0x6f, 0x73,
        0x00, 0x10, 0x68, 0xba, 0x0b, 0x03, 0x01, 0x00, 0x6f, 0xf3, 0x00, 0x10, 0x0b, 0x01, 0x01, 0x00,
        0x69, 0x81, 0x0b, 0x06, 0x01, 0x00, 0x6f, 0x75, 0x00, 0x14, 0x1f, 0xd6, 0x47, 0x54, 0x01, 0x00,
        0x6f, 0x42, 0x00, 0x0c, 0x1f, 0xa1, 0x4d, 0xb8,
    };
    struct { const char *name; int kind; uint32_t er[8]; uint32_t pos, end, count; } cases[] = {
        { "fill", 0, { 0x7ff0, 0x5a, 0, 0, 300, 0x420000, 0, 0x5fff00 }, 0, 0, 0 },
        { "fill one", 0, { 3, 0xa5, 0, 0, 1, 0x401000, 0, 0x5fff00 }, 0, 0, 0 },
        { "lz copy", 1, { 0x30, 0x430000, 0x1234, 0x0000010f, 0, 0x55, 0x0fff0000, 0x5fff00 }, 0, 0, 0 },
        { "lz copy wrap", 1, { 0x3, 0x430000, 0x0ffa, 0x00000000, 0, 0x55, 0x0fff0000, 0x5fff00 }, 0, 0, 0 },
        { "stream count", 2, { 0, 0, 0, 0, 0x410000, 0, 7, 0x5fff00 }, 0x1ff0, 0x3000, 107 },
        { "stream end", 2, { 0, 0, 0, 0, 0x410000, 0, 0, 0x5fff00 }, 0x2fc0, 0x3000, 9000 },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        loop_result_t r[3];
        for (int run = 0; run < 3; run++) {           /* original, native whole, native in 1-state slices */
            boot(prog, sizeof prog);
            uint32_t pc = 0;
            for (int i = 0; i < 0x40000; i++) ram[0x20000 + i] = (uint8_t)(i * 7 + (i >> 8));
            if (cases[k].kind == 0) {
                memcpy(ram + 0x2000, fill_code, sizeof fill_code);
                pc = 0x402000;
            } else if (cases[k].kind == 1) {
                memcpy(rom + 0x1000, lz_code, sizeof lz_code);
                memcpy(rom + CY_ROM_SIZE + 0x1000, lz_code, sizeof lz_code);
                pc = 0x1000;
            } else {
                memcpy(ram + 0x2000, stream_code, sizeof stream_code);
                ram[0x2048] = ram[0x2092] = 0x01;     /* sleep at both exits */
                ram[0x2049] = ram[0x2093] = 0x80;
                h8s_write32(&m.cpu, 0x410010, cases[k].pos);
                h8s_write32(&m.cpu, 0x41000c, cases[k].end);
                h8s_write32(&m.cpu, 0x5fff10, 0x440000);
                h8s_write32(&m.cpu, 0x5fff14, cases[k].count);
                pc = 0x402000;
            }
            if (run) {
                if (cases[k].kind == 1) cy_hle_patch_rom(&m);
                else cy_hle_patch_kernel(&m);
                bool patched = false;
                for (int i = 0; i < CY_HLE_SLOTS; i++) patched |= m.hle_pc[i] == pc + (cases[k].kind == 1 ? 8u : 0u);
                CHECK(patched);
            }
            r[run] = run_loop(pc, cases[k].er, run == 1 ? 1000000 : 1);
        }
        if (!same_result(&r[0], &r[1], false) || !same_result(&r[0], &r[2], true)) {
            printf("hle %s: states %llu %llu %llu ccr %02x %02x %02x pc %06x %06x\n", cases[k].name,
                   (unsigned long long)r[0].states, (unsigned long long)r[1].states,
                   (unsigned long long)r[2].states, r[0].ccr, r[1].ccr, r[2].ccr,
                   (unsigned)r[0].pc, (unsigned)r[1].pc);
            printf("  ram sums %08x %08x %08x\n", (unsigned)r[0].sum, (unsigned)r[1].sum, (unsigned)r[2].sum);
            for (int i = 0; i < 8; i++)
                if (r[0].er[i] != r[1].er[i] || r[0].er[i] != r[2].er[i])
                    printf("  er%d %08x %08x %08x\n", i, (unsigned)r[0].er[i], (unsigned)r[1].er[i], (unsigned)r[2].er[i]);
        }
        CHECK(same_result(&r[0], &r[1], false));
        CHECK(same_result(&r[0], &r[2], true));
        CHECK(m.illegal_ops == 0);
    }
}

static uint8_t rf_pkt[CY_RF_LONG];
static size_t rf_len;
static int rf_channel, rf_sends;
static void on_rf(void *user, const uint8_t *pkt, size_t len, uint8_t channel)
{
    (void)user;
    memcpy(rf_pkt, pkt, len);
    rf_len = len;
    rf_channel = channel;
    rf_sends++;
}

static uint32_t crc32_ref(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    return ~c;
}

/* The radio co-processor as CyOS 1.5.08 drives it (cy_rf.c). */
static void test_radio(void)
{
    boot(prog_spin, sizeof prog_spin);
    m.host.rf_send = on_rf;
    rf_sends = 0;
    static const uint8_t tune[] = { 0x01, 0x02, 0x05 };
    for (size_t i = 0; i < sizeof tune; i++) cy_rf_h8_byte(&m, tune[i]);
    CHECK(m.rf.channel == 5);

    /* a short packet: command, flag, 50 bytes and the spare byte */
    cy_rf_h8_byte(&m, 0x30);
    cy_rf_h8_byte(&m, 0x00);
    for (int i = 0; i < CY_RF_SHORT; i++) cy_rf_h8_byte(&m, (uint8_t)(0xa0 + i));
    CHECK(rf_sends == 0);                              /* waits for the spare byte */
    cy_rf_h8_byte(&m, 0x00);
    CHECK(rf_sends == 1 && rf_len == CY_RF_SHORT && rf_channel == 5);
    CHECK(rf_pkt[0] == 0xa0 && rf_pkt[CY_RF_SHORT - 1] == 0xa0 + CY_RF_SHORT - 1);
    cybiko_run(&m, CY_CLOCK_HZ / 100);                 /* the 03 comes 2 ms later */
    CHECK(m.sci[2].rx_count == 1 && m.sci[2].rx_fifo[m.sci[2].rx_head] == 0x03);

    /* a packet heard: only once SCI2 listens, and only on our channel */
    m.sci[2].rx_count = 0;
    uint8_t pkt[CY_RF_LONG];
    for (int i = 0; i < CY_RF_LONG; i++) pkt[i] = (uint8_t)i;
    CHECK(cybiko_rf_receive(&m, pkt, CY_RF_LONG, 7));  /* other channel */
    CHECK(cybiko_rf_receive(&m, pkt, CY_RF_LONG, 5));
    CHECK(!cybiko_rf_receive(&m, pkt, 10, 5));         /* not a packet size */
    cybiko_run(&m, CY_CLOCK_HZ / 100);
    CHECK(m.rf.heard == 0 && m.rf.queued == 2);        /* SCI2 is off */
    m.mstpcr[1] &= 0x7f;
    m.sci[2].scr = 0x10;                               /* receiver on, no interrupts */
    cybiko_run(&m, CY_CLOCK_HZ / 100);
    CHECK(m.rf.heard == 1 && m.rf.queued == 0);
    CHECK(m.rf.inject[0] == 0xc8 && m.rf.inject[1 + 199] == 199);

    /* each board can get its own ID; the record's CRC-32 follows */
    boot(prog_spin, sizeof prog_spin);
    flash[0x7f800] = 0xba; flash[0x7f801] = 0x0b; flash[0x7f802] = 0xab; flash[0x7f803] = 0x15;
    flash[0x7f818] = 0x4c; flash[0x7f819] = 0x80; flash[0x7f81a] = 0x51; flash[0x7f81b] = 0xa3;
    uint32_t crc = crc32_ref(flash + 0x7f800, 0x7fc);
    for (int i = 0; i < 4; i++) flash[0x7fffc + i] = (uint8_t)(crc >> (24 - 8 * i));
    static uint8_t copy[CY_FLASH_SIZE];
    memcpy(copy, flash, sizeof copy);
    cybiko_set_unit_id(&m, 0x4c123456);
    CHECK(cybiko_unit_id(&m) == 0x4c123456);
    crc = crc32_ref(flash + 0x7f800, 0x7fc);
    CHECK(flash[0x7fffc] == (uint8_t)(crc >> 24) && flash[0x7ffff] == (uint8_t)crc);
    static uint8_t saved[CY_FLASH_SIZE];
    memcpy(saved, flash, sizeof saved);
    cybiko_flash_original(&m, saved);
    CHECK(memcmp(saved, copy, sizeof saved) == 0);
}

/* The MP3 cartridge: its SD card answers in SPI mode on SCI1 with chip
 * select on P2.2, and the decoder's MPEG input (SCI0) drives DEMAND. */
static uint8_t card_img[4 * 512];
static bool card_rd(void *user, uint32_t block, uint8_t *buf)
{
    (void)user;
    if (block >= 4) return false;
    memcpy(buf, card_img + block * 512, 512);
    return true;
}

static uint8_t spi(uint8_t out) { return cy_cart_spi(&m, 1, out); }

/* Send a command and return its R1, as a host polls for it. */
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    uint8_t b[6] = { (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                     (uint8_t)(arg >> 8), (uint8_t)arg, crc };
    for (int i = 0; i < 6; i++) spi(b[i]);
    for (int i = 0; i < 8; i++) {
        uint8_t r = spi(0xff);
        if (!(r & 0x80)) return r;
    }
    return 0xff;
}

static void test_cart(void)
{
    boot(prog_spin, sizeof prog_spin);
    CHECK(cy_cart_adc(&m, 3) == 0x3ff);          /* empty slot */
    m.cart.present = true;
    CHECK(cy_cart_adc(&m, 3) == 0);              /* ID 0: the MP3 cartridge */
    for (int i = 0; i < (int)sizeof card_img; i++) card_img[i] = (uint8_t)(i * 7);
    m.host.card_read = card_rd;
    m.cart.card_blocks = 4;
    CHECK((cy_cart_p2_input(&m) & 0x01) == 0);   /* card detect: low with a card */

    cy_cart_p2(&m, 0xfb, 0xfe);                  /* CS (P2.2) low */
    CHECK(sd_cmd(0, 0, 0x95) == 0x01);           /* idle */
    CHECK(sd_cmd(55, 0, 0xff) == 0x01);
    CHECK(sd_cmd(41, 0, 0xff) == 0x00);          /* ready */
    CHECK(sd_cmd(17, 512, 0xff) == 0x00);        /* read block 1 (byte address) */
    int token = 0;
    for (int i = 0; i < 16 && token != 0xfe; i++) token = spi(0xff);
    CHECK(token == 0xfe);
    bool same = true;
    for (int i = 0; i < 512; i++) same &= spi(0xff) == card_img[512 + i];
    CHECK(same);
    spi(0xff); spi(0xff);                        /* CRC16 */
    CHECK(sd_cmd(5, 0, 0xff) == 0x04);           /* illegal command */
    cy_cart_p2(&m, 0xff, 0xfe);

    /* DEMAND (P6.6, IRQ2 pin) is high from the first I2C access to the
     * decoder until its buffer fills, and back once the host takes data. */
    m.cart.mas_on = true;
    CHECK((h8s_read8(&m.cpu, 0xffff55) & 0x40) != 0);
    int sent = 0;
    while ((h8s_read8(&m.cpu, 0xffff55) & 0x40) && sent < 2 * CY_MP3_FIFO) cy_cart_spi(&m, 0, (uint8_t)sent++);
    CHECK(sent > CY_MP3_FIFO - 64 && sent <= CY_MP3_FIFO);
    uint8_t got[CY_MP3_FIFO];
    CHECK(cybiko_mp3_take(&m, got, 100) == 100 && got[0] == 0 && got[99] == 99);
    CHECK((h8s_read8(&m.cpu, 0xffff55) & 0x40) == 0);   /* not yet half empty */
    cybiko_mp3_take(&m, got, CY_MP3_FIFO);
    CHECK((h8s_read8(&m.cpu, 0xffff55) & 0x40) != 0);
}

int main(void)
{
    ram = malloc(CY_RAM_SIZE);
    test_memory_map();
    test_tpu_interrupts();
    test_tmr_interrupts();
    test_sci_tx();
    test_sci_rx();
    test_keyboard();
    test_lcd();
    test_flash();
    test_rtc();
    test_watchdog();
    test_speaker();
    test_dma();
    test_disk();
    test_hle();
    test_hle_loops();
    test_radio();
    test_cart();
    free(ram);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all cybiko tests passed\n");
    return 0;
}
