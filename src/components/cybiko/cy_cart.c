/* The MP3 player cartridge (CY45P01) on the expansion port, as its drivers
 * from the cartridge's CD (CartMan.dl, 00000000.dl) use it:
 *
 *   ADC channel 3   cartridge ID: 0x3ff with the slot empty, ID 0 (a
 *                   reading below 15) is the MP3 player
 *   P2.1 / P2.3     I2C clock and data (open drain) to a Micronas MAS35xx
 *                   decoder at 3c/3d; 6c writes codec registers (0010 is
 *                   the volume), 68/69 the DSP (70 reads the chip version)
 *   SCI0            MPEG data to the decoder, clocked serial, 32-byte bursts
 *   P6.6 + IRQ2     the decoder's DEMAND line, high while it takes data; IRQ2
 *                   on its rising edge starts the next bursts
 *   SCI1            the SD card in SPI mode (MSB first via SCMR), with DMA
 *                   0A/0B clocking blocks; P2.2 is its chip select and P2.0
 *                   reads low while a card is in
 *
 * mp3-cartridge.md in cybiko-docs (https://github.com/SovGVD/cybiko-docs) has the
 * details. The card is the host's disk
 * image (card_read/card_write); the MPEG stream goes to the host through
 * cybiko_mp3_take, which decodes it. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "cy_internal.h"

static void logf_(cybiko_t *m, const char *fmt, ...)
{
    if (!m->cart_log) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    m->cart_log(m, buf);
}

void cy_cart_reset(cybiko_t *m)
{
    cy_cart_t *c = &m->cart;
    c->p2_out = 0xff;
    c->p3_out = 0xff;
    c->mas_on = false;
    c->mas_demand = true;
    c->mp3_head = c->mp3_count = 0;
}

uint16_t cy_cart_adc(cybiko_t *m, int ch)
{
    (void)ch;
    return m->cart.present ? 0 : 0x3ff;
}

/* ---- the MP3 decoder's I2C port: SCL on P2.1, SDA on P2.3 ---- */

#define SCL 0x02
#define SDA 0x08
#define SD_CS 0x04
#define CARD_IN 0x01

static void i2c_byte(cybiko_t *m)
{
    cy_cart_t *c = &m->cart;
    if (c->i2c_n == 0) {
        c->i2c_read = c->i2c_sr & 1;
        c->i2c_ack = (c->i2c_sr & 0xfe) == 0x3c;
        if (c->i2c_ack && !c->mas_on) {
            uint8_t old = cy_irq_pins(m);
            c->mas_on = true;               /* DEMAND rises: the decoder wants data */
            cy_irq_pins_changed(m, old);
        }
        logf_(m, "i2c %s %02x", c->i2c_read ? "read" : "write", c->i2c_sr);
    } else if (!c->i2c_read) {
        logf_(m, "i2c  w %02x", c->i2c_sr);
        if (c->i2c_n <= sizeof c->i2c_msg) c->i2c_msg[c->i2c_n - 1] = c->i2c_sr;
        /* 6c rr rr vv vv writes codec register rrrr; 0010 is the volume
         * (high byte, 0 mutes), 0011 the balance. */
        if (c->i2c_n == 5 && c->i2c_msg[0] == 0x6c && c->i2c_msg[1] == 0 && c->i2c_msg[2] < 0x20)
            c->codec[c->i2c_msg[2]] = (uint16_t)(c->i2c_msg[3] << 8 | c->i2c_msg[4]);
    }
    c->i2c_n++;
}

static uint8_t i2c_next_read(cybiko_t *m)
{
    (void)m;
    return 0x00;
}

static void i2c_pins(cybiko_t *m, bool scl, bool sda)
{
    cy_cart_t *c = &m->cart;
    bool pscl = c->i2c_scl, psda = c->i2c_sda;
    c->i2c_scl = scl;
    c->i2c_sda = sda;
    if (pscl && scl && psda && !sda) {          /* start */
        c->i2c_active = true;
        c->i2c_bits = 0;
        c->i2c_n = 0;
        c->i2c_read = false;
        c->i2c_drive = false;
        return;
    }
    if (pscl && scl && !psda && sda) {          /* stop */
        c->i2c_active = false;
        c->i2c_drive = false;
        return;
    }
    if (!c->i2c_active) return;
    if (!pscl && scl) {                         /* rising: sample */
        if (c->i2c_bits < 8) {
            c->i2c_sr = (uint8_t)(c->i2c_sr << 1 | sda);
        } else if (c->i2c_read && c->i2c_n > 1 && sda) {
            c->i2c_active = false;              /* master NAK ends a read */
        }
        c->i2c_bits++;
    } else if (pscl && !scl) {                  /* falling: drive */
        if (c->i2c_bits == 8) {
            bool reading = c->i2c_read && c->i2c_n > 0;
            if (!reading) i2c_byte(m);
            else c->i2c_n++;
            c->i2c_drive = !reading ? c->i2c_ack : false;
            c->i2c_out = 0;
        } else if (c->i2c_bits == 9) {
            c->i2c_bits = 0;
            if (c->i2c_read && c->i2c_ack) {
                c->i2c_tx = i2c_next_read(m);
                c->i2c_sr = c->i2c_tx;
            }
            c->i2c_drive = false;
        }
        if (c->i2c_read && c->i2c_n > 0 && c->i2c_bits < 8 && c->i2c_ack) {
            c->i2c_drive = true;
            c->i2c_out = (c->i2c_tx >> (7 - c->i2c_bits)) & 1;
        }
    }
}

uint8_t cy_cart_p2_input(cybiko_t *m)
{
    cy_cart_t *c = &m->cart;
    uint8_t v = 0xff;
    if (c->present && c->i2c_drive && !c->i2c_out) v &= (uint8_t)~SDA;
    if (c->present && c->card_blocks) v &= (uint8_t)~CARD_IN;
    return v;
}

void cy_cart_p2(cybiko_t *m, uint8_t level, uint8_t ddr)
{
    cy_cart_t *c = &m->cart;
    uint8_t changed = (uint8_t)((level ^ c->p2_out) | (ddr ^ c->p2_ddr));
    c->p2_out = level;
    c->p2_ddr = ddr;
    if (changed & ~(SCL | SDA)) logf_(m, "P2 %02x ddr %02x", level, ddr);
    if (!c->present) return;
    /* open drain: a released line is high unless the decoder pulls it */
    bool sda = (level & SDA) && !(c->i2c_drive && !c->i2c_out);
    i2c_pins(m, level & SCL, sda);
}

void cy_cart_p3(cybiko_t *m, uint8_t level, uint8_t ddr)
{
    cy_cart_t *c = &m->cart;
    if (level != c->p3_out || ddr != c->p3_ddr)
        logf_(m, "P3 %02x ddr %02x", level, ddr);
    c->p3_out = level;
    c->p3_ddr = ddr;
}

/* ---- the SD card slot: SPI mode on SCI1, chip select P2.2 ---- */

static uint8_t crc7(const uint8_t *p, int n)
{
    uint8_t crc = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = p[i];
        for (int k = 0; k < 8; k++) {
            crc <<= 1;
            if ((b ^ crc) & 0x80) crc ^= 0x09;
            b <<= 1;
        }
    }
    return (uint8_t)((crc << 1) | 1);
}

static uint16_t crc16(const uint8_t *p, int n)
{
    uint16_t crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void put_bits(uint8_t *reg, int len, int hi, int lo, uint32_t v)
{
    for (int b = lo; b <= hi; b++, v >>= 1) {
        int byte = len - 1 - b / 8, bit = b % 8;
        if (v & 1) reg[byte] |= (uint8_t)(1 << bit);
        else reg[byte] &= (uint8_t)~(1 << bit);
    }
}

/* A CSD (version 1.0) for a card of `blocks` 512-byte blocks. */
static void make_csd(uint8_t *csd, uint32_t blocks)
{
    memset(csd, 0, 16);
    int mult = 0;
    uint32_t c_size = blocks;
    while (c_size > 4096 && mult < 7) { c_size >>= 1; mult++; }
    c_size >>= 2;                       /* C_SIZE_MULT counts from x4 */
    if (mult < 7 && c_size > 4096) c_size = 4096;
    put_bits(csd, 16, 127, 126, 0);     /* CSD structure 1.0 */
    put_bits(csd, 16, 119, 112, 0x0e);  /* TAAC */
    put_bits(csd, 16, 111, 104, 0);     /* NSAC */
    put_bits(csd, 16, 103, 96, 0x32);   /* TRAN_SPEED 25 MHz */
    put_bits(csd, 16, 95, 84, 0x5b5);   /* CCC */
    put_bits(csd, 16, 83, 80, 9);       /* READ_BL_LEN 512 */
    put_bits(csd, 16, 73, 62, c_size ? c_size - 1 : 0);
    put_bits(csd, 16, 61, 59, 7); put_bits(csd, 16, 58, 56, 7);
    put_bits(csd, 16, 55, 53, 7); put_bits(csd, 16, 52, 50, 7);
    put_bits(csd, 16, 49, 47, (uint32_t)mult);
    put_bits(csd, 16, 46, 46, 1);       /* ERASE_BLK_EN */
    put_bits(csd, 16, 45, 39, 0x7f);    /* SECTOR_SIZE */
    put_bits(csd, 16, 28, 26, 2);       /* R2W_FACTOR */
    put_bits(csd, 16, 25, 22, 9);       /* WRITE_BL_LEN 512 */
    csd[15] = crc7(csd, 15);
}

static void make_cid(uint8_t *cid)
{
    static const uint8_t base[15] = { 0x03, 'S', 'D', 'C', 'Y', 'B', 'K', 'O', 0x10,
                                      0x12, 0x34, 0x56, 0x78, 0x01, 0x1a };
    memcpy(cid, base, 15);
    cid[15] = crc7(cid, 15);
}

static void sd_queue(cy_cart_t *c, const uint8_t *p, int n)
{
    for (int i = 0; i < n && c->sd_out_len < (int)sizeof c->sd_out; i++)
        c->sd_out[c->sd_out_len++] = p[i];
}

static void sd_r1(cy_cart_t *c, uint8_t r1)
{
    uint8_t b[2] = { 0xff, r1 };
    sd_queue(c, b, 2);
}

/* A data block: start token, data, CRC16 */
static void sd_data(cy_cart_t *c, const uint8_t *p, int n)
{
    uint8_t t = 0xfe;
    sd_queue(c, &t, 1);
    sd_queue(c, p, n);
    uint16_t crc = crc16(p, n);
    uint8_t cb[2] = { (uint8_t)(crc >> 8), (uint8_t)crc };
    sd_queue(c, cb, 2);
}

static void sd_read_block(cybiko_t *m)
{
    cy_cart_t *c = &m->cart;
    uint8_t buf[512];
    bool ok = c->sd_block < c->card_blocks && m->host.card_read &&
              m->host.card_read(m->host.user, c->sd_block, buf);
    if (!ok) {
        uint8_t err[2] = { 0xff, 0x08 };    /* data error token: out of range */
        sd_queue(c, err, 2);
        c->sd_multi = false;
        return;
    }
    uint8_t gap = 0xff;
    sd_queue(c, &gap, 1);
    sd_data(c, buf, 512);
    c->sd_block++;
}

static void sd_command(cybiko_t *m)
{
    cy_cart_t *c = &m->cart;
    uint8_t cmd = c->sd_cmd[0] & 0x3f;
    uint32_t arg = (uint32_t)c->sd_cmd[1] << 24 | (uint32_t)c->sd_cmd[2] << 16 |
                   (uint32_t)c->sd_cmd[3] << 8 | c->sd_cmd[4];
    bool app = c->sd_app;
    c->sd_app = false;
    uint8_t idle = c->sd_idle ? 0x01 : 0x00;
    logf_(m, "sd %sCMD%d %08lx", app ? "A" : "", cmd, (unsigned long)arg);
    if (app) {
        switch (cmd) {
        case 41: c->sd_idle = false; sd_r1(c, 0x00); return;
        case 13: { sd_r1(c, idle); uint8_t z[2] = { 0, 0 }; sd_queue(c, z, 1); sd_queue(c, z, 1); return; }
        case 51: {
            sd_r1(c, idle);
            uint8_t scr[8] = { 0x00, 0x25, 0, 0, 0, 0, 0, 0 };
            uint8_t gap = 0xff;
            sd_queue(c, &gap, 1);
            sd_data(c, scr, 8);
            return;
        }
        default: sd_r1(c, idle); return;
        }
    }
    switch (cmd) {
    case 0: c->sd_idle = true; c->sd_multi = false; sd_r1(c, 0x01); break;
    case 1: c->sd_idle = false; sd_r1(c, 0x00); break;
    case 9: case 10: {
        uint8_t reg[16];
        if (cmd == 9) make_csd(reg, c->card_blocks); else make_cid(reg);
        sd_r1(c, idle);
        uint8_t gap = 0xff;
        sd_queue(c, &gap, 1);
        sd_data(c, reg, 16);
        break;
    }
    case 12: {
        c->sd_multi = false;
        c->sd_out_len = c->sd_out_pos = 0;  /* drop the block in flight */
        uint8_t b[3] = { 0xff, idle, 0xff };
        sd_queue(c, b, 3);
        break;
    }
    case 13: { sd_r1(c, idle); uint8_t z = 0; sd_queue(c, &z, 1); break; }
    case 16: sd_r1(c, arg == 512 ? idle : 0x40); break;
    case 17: case 18:
        sd_r1(c, idle);
        c->sd_block = arg / 512;
        c->sd_multi = cmd == 18;
        sd_read_block(m);
        break;
    case 24: case 25:
        sd_r1(c, idle);
        c->sd_block = arg / 512;
        c->sd_wmode = cmd == 24 ? 1 : 2;
        break;
    case 55: c->sd_app = true; sd_r1(c, idle); break;
    case 58: { sd_r1(c, idle); uint8_t ocr[4] = { 0x80, 0xff, 0x80, 0x00 }; sd_queue(c, ocr, 4); break; }
    case 59: sd_r1(c, idle); break;
    default: sd_r1(c, (uint8_t)(idle | 0x04)); break;   /* illegal command */
    }
}

/* A byte the host clocks into the card; returns the card's byte. */
static uint8_t sd_byte(cybiko_t *m, uint8_t in)
{
    cy_cart_t *c = &m->cart;
    uint8_t out = 0xff;
    /* While the card sends, it ignores what comes in (except CMD12 during
     * a multiple block read). */
    bool sending = c->sd_out_pos < c->sd_out_len;
    if (sending) {
        out = c->sd_out[c->sd_out_pos++];
        if (c->sd_out_pos == c->sd_out_len) {
            c->sd_out_pos = c->sd_out_len = 0;
            if (c->sd_multi) sd_read_block(m);
        }
    }
    if (c->sd_wmode) {
        if (c->sd_wpos == 0) {                  /* waiting for a start token */
            if (in == 0xfe || (in == 0xfc && c->sd_wmode == 2)) c->sd_wpos = 1;
            else if (in == 0xfd && c->sd_wmode == 2) {
                c->sd_wmode = 0;
                uint8_t busy[2] = { 0x00, 0xff };
                sd_queue(c, busy, 2);
            }
            return out;
        }
        if (c->sd_wpos <= 512) c->sd_wbuf[c->sd_wpos - 1] = in;
        if (++c->sd_wpos == 512 + 3) {          /* data and CRC in */
            bool ok = c->sd_block < c->card_blocks && m->host.card_write &&
                      m->host.card_write(m->host.user, c->sd_block, c->sd_wbuf);
            c->sd_block++;
            c->sd_wpos = 0;
            if (c->sd_wmode == 1) c->sd_wmode = 0;
            uint8_t resp[3] = { ok ? 0x05 : 0x0d, 0x00, 0xff };
            sd_queue(c, resp, 3);
        }
        return out;
    }
    if (sending && !c->sd_multi) return out;
    if (c->sd_cmd_len == 0 && (in & 0xc0) != 0x40) return out;
    c->sd_cmd[c->sd_cmd_len++] = in;
    if (c->sd_cmd_len == 6) {
        c->sd_cmd_len = 0;
        if (!c->sd_multi || (c->sd_cmd[0] & 0x3f) == 12) {
            if ((c->sd_cmd[0] & 0x3f) != 12) c->sd_out_len = c->sd_out_pos = 0;
            sd_command(m);
        }
    }
    return out;
}

/* ---- the decoder's MPEG data input and its DEMAND line ---- */

/* DEMAND (P6.6, and IRQ2 on its rising edge) is high while the decoder can
 * take data. CyOS sends 32-byte chunks while it is high, so it drops while a
 * chunk still fits and comes back once the host has taken half the buffer. */
#define MP3_CHUNK 32

static bool demand(const cy_cart_t *c)
{
    return c->present && c->mas_on && c->mas_demand;
}

static void update_demand(cy_cart_t *c)
{
    if (c->mas_demand && CY_MP3_FIFO - c->mp3_count < 2 * MP3_CHUNK) c->mas_demand = false;
    else if (!c->mas_demand && c->mp3_count <= CY_MP3_FIFO / 2) c->mas_demand = true;
}

uint8_t cy_cart_irq_pins(const cybiko_t *m)
{
    return demand(&m->cart) ? 0 : 0x04;     /* bit set = pin low */
}

uint8_t cy_cart_p6_input(const cybiko_t *m)
{
    return demand(&m->cart) ? 0xff : 0xbf;
}

static void mp3_put(cybiko_t *m, uint8_t b)
{
    cy_cart_t *c = &m->cart;
    if (c->mp3_count >= CY_MP3_FIFO) { logf_(m, "mp3 overrun"); return; }
    uint8_t old = cy_irq_pins(m);
    c->mp3[(c->mp3_head + c->mp3_count) % CY_MP3_FIFO] = b;
    c->mp3_count++;
    update_demand(c);
    cy_irq_pins_changed(m, old);
}

uint8_t cybiko_mp3_volume(const cybiko_t *m)
{
    return (uint8_t)(m->cart.codec[0x10] >> 8);
}

size_t cybiko_mp3_take(cybiko_t *m, uint8_t *buf, size_t max)
{
    cy_cart_t *c = &m->cart;
    uint8_t old = cy_irq_pins(m);
    size_t n = 0;
    while (n < max && c->mp3_count) {
        buf[n++] = c->mp3[c->mp3_head];
        c->mp3_head = (uint16_t)((c->mp3_head + 1) % CY_MP3_FIFO);
        c->mp3_count--;
    }
    update_demand(c);
    cy_irq_pins_changed(m, old);
    return n;
}

uint8_t cy_cart_spi(cybiko_t *m, int port, uint8_t out)
{
    cy_cart_t *c = &m->cart;
    if (!c->present) return 0xff;
    if (port == 1 && !(c->p2_out & SD_CS) && c->card_blocks) return sd_byte(m, out);
    if (port == 0) { mp3_put(m, out); return 0xff; }
    logf_(m, "SCI%d > %02x (P2 %02x)", port, out, c->p2_out);
    return 0xff;
}
