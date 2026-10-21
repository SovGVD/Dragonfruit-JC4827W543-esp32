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
    free(ram);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all cybiko tests passed\n");
    return 0;
}
