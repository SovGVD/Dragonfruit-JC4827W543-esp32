/* Cybiko Xtreme board devices: HD66421 LCD controller, PCF8593 real-time
 * clock on a bit-banged I2C bus, and the SST39VF400A flash. Register and
 * bus behaviour follow MAME's hd66421, pcf8593 and intelfsh devices; the
 * flash ID values come from the SST39VF400A datasheet. */
#include <string.h>
#include "cy_internal.h"

/* ------------------------------------------------------------------ */
/* HD66421 LCD                                                         */
/* ------------------------------------------------------------------ */

#define LCD_ROW_BYTES (CY_LCD_W / 4)

void cy_lcd_reset(cy_lcd_t *l)
{
    memset(l->reg, 0, sizeof l->reg);
    l->cmd = l->x = l->y = 0;
    l->version++;
}

uint8_t cy_lcd_read(cy_lcd_t *l, int data)
{
    if (!data) return l->cmd;
    return l->cmd < sizeof l->reg ? l->reg[l->cmd] : 0;
}

void cy_lcd_write(cy_lcd_t *l, int data, uint8_t v)
{
    if (!data) { l->cmd = v; return; }
    if (l->cmd >= sizeof l->reg) return;
    l->reg[l->cmd] = v;
    switch (l->cmd) {
    case 0x02: l->x = v; break;
    case 0x03: l->y = v; break;
    case 0x04: {                    /* display RAM, with auto-increment */
        unsigned at = (unsigned)l->y * LCD_ROW_BYTES + l->x;
        if (at < sizeof l->ram && l->ram[at] != v) {
            l->ram[at] = v;
            l->version++;
        }
        if (l->reg[0x01] & 0x02) l->x++;
        else l->y++;
        if (l->x >= LCD_ROW_BYTES) { l->x = 0; l->y++; }
        if (l->y >= CY_LCD_H) l->y = 0;
        break;
    }
    case 0x00: case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x10:
        l->version++;               /* display on/off, palette, contrast */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* PCF8593 RTC                                                         */
/* ------------------------------------------------------------------ */

static uint8_t to_bcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }
static int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant). */
static int64_t days_from_civil(int y, int mon, int d)
{
    y -= mon <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *mon, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mon = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*mon <= 2));
}

static int64_t host_seconds(const cybiko_t *m)
{
    if (m->host.wall_time) return m->host.wall_time(m->host.user);
    return (int64_t)(h8s_time(&m->cpu) / m->clock_hz);
}

static int64_t rtc_seconds(const cybiko_t *m)
{
    return host_seconds(m) + m->rtc.offset;
}

/* Load the time registers (1-6) from the clock. */
static void rtc_latch(cybiko_t *m)
{
    cy_rtc_t *r = &m->rtc;
    int64_t t = rtc_seconds(m);
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int sod = (int)(t - days * 86400);
    int y, mon, d;
    civil_from_days(days, &y, &mon, &d);
    int wday = (int)((days % 7 + 11) % 7);   /* 1970-01-01 was a Thursday (4) */
    int hour = sod / 3600;

    r->data[1] = to_bcd((int)((h8s_time(&m->cpu) % m->clock_hz) * 100 / m->clock_hz));
    r->data[2] = to_bcd(sod % 60);
    r->data[3] = to_bcd(sod / 60 % 60);
    if (r->data[4] & 0x80) {                 /* 12-hour format */
        int h12 = hour % 12 ? hour % 12 : 12;
        r->data[4] = (uint8_t)(0x80 | (hour >= 12 ? 0x40 : 0) | to_bcd(h12));
    } else {
        r->data[4] = to_bcd(hour);
    }
    r->data[5] = (uint8_t)(((y % 4) << 6) | to_bcd(d));
    r->data[6] = (uint8_t)((wday << 5) | to_bcd(mon));
}

/* The CPU wrote the time registers: move the clock to match them. The chip
 * only keeps the year modulo 4, so take the nearest such year. */
static void rtc_store(cybiko_t *m)
{
    cy_rtc_t *r = &m->rtc;
    int64_t old = rtc_seconds(m);
    int64_t odays = old >= 0 ? old / 86400 : (old - 86399) / 86400;
    int oy, om, od;
    civil_from_days(odays, &oy, &om, &od);

    int y4 = r->data[5] >> 6;
    int y = oy - ((oy % 4 + 4) % 4) + y4;
    if (y - oy > 2) y -= 4;
    if (oy - y > 1) y += 4;
    int mon = from_bcd(r->data[6] & 0x1f);
    int d = from_bcd(r->data[5] & 0x3f);
    if (mon < 1 || mon > 12) mon = 1;
    if (d < 1 || d > 31) d = 1;
    int hour;
    if (r->data[4] & 0x80) {
        hour = from_bcd(r->data[4] & 0x1f) % 12;
        if (r->data[4] & 0x40) hour += 12;
    } else {
        hour = from_bcd(r->data[4] & 0x3f);
    }
    int64_t t = days_from_civil(y, mon, d) * 86400 + hour * 3600 +
                from_bcd(r->data[3] & 0x7f) * 60 + from_bcd(r->data[2] & 0x7f);
    r->offset = t - host_seconds(m);
}

void cy_rtc_reset(cybiko_t *m)
{
    cy_rtc_t *r = &m->rtc;
    /* A real RTC keeps its registers on the backup battery. CyOS checks the
     * alarm registers at boot and, finding them blank, takes the clock for
     * dead and sets it to 1.01.2000 12:00am. Start with what CyOS writes
     * there itself, so it believes the clock (the host's time). */
    if (!r->data[0] && !r->data[8]) {
        r->data[0] = 0x04;
        r->data[8] = 0x30;
        r->data[0xd] = 0x01;
        r->data[0xe] = 0x01;
    }
    r->scl = r->sda = true;
    r->active = r->send = false;
    r->inp = 0;
    r->bits = r->pos = r->idx = 0;
    memset(r->recv, 0, sizeof r->recv);
}

static void rtc_scl(cybiko_t *m, bool state)
{
    cy_rtc_t *r = &m->rtc;
    if (r->active && !r->scl && state) {          /* rising edge */
        if (!r->send) {
            if (r->bits < 8 && r->sda) r->recv[r->idx < 3 ? r->idx : 3] |= (uint8_t)(0x80 >> r->bits);
            r->bits++;
            r->inp = 0;                           /* acknowledge everything */
            if (r->bits > 8) {
                uint8_t b = r->recv[r->idx < 3 ? r->idx : 3];
                if (r->idx == 0 && b == 0xa3) {
                    r->send = true;
                } else if (r->recv[0] == 0xa2 && r->idx == 1) {
                    r->pos = b;
                } else if (r->recv[0] == 0xa2 && r->idx >= 2) {
                    uint8_t at = (uint8_t)((r->recv[1] + r->idx - 2) & 15);
                    r->data[at] = b;
                    if (at >= 2 && at <= 6) rtc_store(m);
                }
                r->bits = 0;
                if (r->idx < 255) r->idx++;
                r->recv[3] = 0;
            }
        } else {
            r->inp = r->bits < 8 ? (r->data[r->pos & 15] >> (7 - r->bits)) & 1 : 0;
            r->bits++;
            if (r->bits > 8) {
                if (r->sda) {                     /* master NAK: done */
                    r->send = false;
                    memset(r->recv, 0, sizeof r->recv);
                    r->idx = 0;
                }
                r->bits = 0;
                r->pos++;
            }
        }
    }
    r->scl = state;
}

static void rtc_sda(cybiko_t *m, bool state)
{
    cy_rtc_t *r = &m->rtc;
    if (r->scl) {
        if (!state && r->sda) {                   /* start */
            r->active = true;
            r->send = false;
            r->bits = 0;
            r->idx = 0;
            memset(r->recv, 0, sizeof r->recv);
            rtc_latch(m);
        } else if (state && !r->sda) {            /* stop */
            r->active = false;
        }
    }
    r->sda = state;
}

void cy_rtc_pins(cybiko_t *m, bool scl, bool sda)
{
    if (scl != m->rtc.scl) rtc_scl(m, scl);
    if (sda != m->rtc.sda) rtc_sda(m, sda);
}

bool cy_rtc_sda(const cybiko_t *m)
{
    return m->rtc.inp;
}

/* ------------------------------------------------------------------ */
/* SST39VF400A flash                                                   */
/* ------------------------------------------------------------------ */

enum { FL_READ, FL_UNLOCK1, FL_UNLOCK2, FL_ERASE1, FL_ERASE2, FL_ERASE3, FL_PROGRAM, FL_ID };

#define FL_MAKER  0x00bf
#define FL_DEVICE 0x2780

static void flash_set_mode(cybiko_t *m, int mode)
{
    bool was_read = m->flash.mode == FL_READ, now_read = mode == FL_READ;
    m->flash.mode = mode;
    if (was_read == now_read) return;
    /* Reads go through the slow path while the chip shows ID data. */
    h8s_map(&m->cpu, 0x600000, 0x200000, now_read ? m->flash.mem : NULL, CY_FLASH_SIZE, false);
}

void cy_flash_reset(cybiko_t *m)
{
    m->flash.mode = FL_ID;          /* force the remap */
    flash_set_mode(m, FL_READ);
}

uint16_t cy_flash_read16(cybiko_t *m, uint32_t off)
{
    if (m->flash.mode == FL_ID) {
        switch ((off >> 1) & 0xff) {
        case 0: return FL_MAKER;
        case 1: return FL_DEVICE;
        default: return 0;
        }
    }
    return (uint16_t)(m->flash.mem[off] << 8 | m->flash.mem[off + 1]);
}

void cy_flash_write16(cybiko_t *m, uint32_t off, uint16_t v)
{
    uint32_t wa = (off >> 1) & 0x7fff;
    uint8_t d = (uint8_t)v;
    cy_flash_t *f = &m->flash;

    switch (f->mode) {
    case FL_PROGRAM: {
        uint8_t hi = (uint8_t)(f->mem[off] & (v >> 8)), lo = (uint8_t)(f->mem[off + 1] & v);
        if (hi != f->mem[off] || lo != f->mem[off + 1]) {
            f->mem[off] = hi;
            f->mem[off + 1] = lo;
            f->dirty = true;
            f->writes++;
        }
        flash_set_mode(m, FL_READ);
        return;
    }
    case FL_UNLOCK1:
        flash_set_mode(m, wa == 0x2aaa && d == 0x55 ? FL_UNLOCK2 : FL_READ);
        return;
    case FL_UNLOCK2:
        if (wa == 0x5555 && d == 0xa0) flash_set_mode(m, FL_PROGRAM);
        else if (wa == 0x5555 && d == 0x80) flash_set_mode(m, FL_ERASE1);
        else if (wa == 0x5555 && d == 0x90) flash_set_mode(m, FL_ID);
        else flash_set_mode(m, FL_READ);
        return;
    case FL_ERASE1:
        flash_set_mode(m, wa == 0x5555 && d == 0xaa ? FL_ERASE2 : FL_READ);
        return;
    case FL_ERASE2:
        flash_set_mode(m, wa == 0x2aaa && d == 0x55 ? FL_ERASE3 : FL_READ);
        return;
    case FL_ERASE3:
        /* Erases finish at once; status polling then sees the final data. */
        if (wa == 0x5555 && d == 0x10) {
            memset(f->mem, 0xff, CY_FLASH_SIZE);
            f->dirty = true;
            f->writes++;
        } else if (d == 0x30) {
            memset(f->mem + (off & ~0xfffu), 0xff, 0x1000);
            f->dirty = true;
            f->writes++;
        } else if (d == 0x50) {
            memset(f->mem + (off & ~0xffffu), 0xff, 0x10000);
            f->dirty = true;
            f->writes++;
        }
        flash_set_mode(m, FL_READ);
        return;
    case FL_ID:
        if (d == 0xf0) { flash_set_mode(m, FL_READ); return; }
        if (wa == 0x5555 && d == 0xaa) { flash_set_mode(m, FL_UNLOCK1); return; }
        return;
    default:
        if (wa == 0x5555 && d == 0xaa) flash_set_mode(m, FL_UNLOCK1);
        return;
    }
}

/* A CyOS update image (.boot, as EZ Loader sent it over USB) starts with the
 * magic ffc0ffab and its size twice. The flash loader at 6001c8 knows this
 * format: it unpacks the installer inside, which programs the new CyOS into
 * the flash and restarts. So an update is staged by putting the image where
 * the loader looks for the kernel, the first image header after the loader
 * itself. */
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int cybiko_flash_stage_boot(uint8_t *flash, const uint8_t *boot, size_t len)
{
    if (len < 12 || be32(boot) != 0xffc0ffabu || be32(boot + 4) != be32(boot + 8) ||
        be32(boot + 4) + 12 != len)
        return CY_BOOT_BAD_IMAGE;
    if (be32(flash) != 0x1234abcdu || be32(flash + 4) != 0x6001c8u) return CY_BOOT_NO_LOADER;
    /* The loader's code has the magics as immediates too; a real header has
     * sizes that fit the flash. */
    for (uint32_t off = 0x200; off < 0x10000; off += 2) {
        uint32_t magic = be32(flash + off), packed = be32(flash + off + 4), plain = be32(flash + off + 8);
        bool lzss = magic == 0x01c0ffabu && packed < CY_FLASH_SIZE && plain >= packed && plain < 0x200000;
        bool update = magic == 0xffc0ffabu && packed == plain && packed < CY_FLASH_SIZE;
        if (!lzss && !update) continue;
        if (off + len > CY_FLASH_ID_RECORD) return CY_BOOT_TOO_BIG;
        memcpy(flash + off, boot, len);
        memset(flash + off + len, 0xff, CY_FLASH_ID_RECORD - off - len);
        return CY_BOOT_OK;
    }
    return CY_BOOT_NO_LOADER;
}
