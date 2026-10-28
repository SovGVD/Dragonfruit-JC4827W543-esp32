/* Cybiko Classic parts that the Xtreme does not have: the AT45DB081 serial
 * DataFlash on SCI1 that holds CyOS and the files, its empty-block format,
 * and the Cybiko ID.
 *
 * The flash follows Atmel's AT45DB041/081 datasheets and MAME's at45dbxx
 * device (BSD-3-Clause, Tim Schuerewegen); the commands CyOS 1.2 uses (52
 * page read, 57 status, 60 compare, 82 program through buffer 1) are the
 * ones MAME and ReBiko (MIT, Dan Berkowitz) model, the others are here
 * from the datasheet. The block format in cybiko_at45_format_free is
 * ReBiko's (at45_format_free), worked out from flash_v1246.bin's empty
 * pages, and so is where CyOS 1.2.46 keeps the Cybiko ID in RAM. */
#include <string.h>
#include "cy_internal.h"

#define ST_READY 0x80
#define ST_MISMATCH 0x40
#define ST_DB081 0x20              /* density code 4 in bits 5-3, as MAME */

static uint32_t at45_addr(const cy_at45_t *f)
{
    return (uint32_t)f->cmd[1] << 16 | (uint32_t)f->cmd[2] << 8 | f->cmd[3];
}
static uint32_t at45_page(const cy_at45_t *f) { return (at45_addr(f) >> 9) & (CY_AT45_PAGES - 1); }
static uint16_t at45_byte(const cy_at45_t *f) { return (uint16_t)(at45_addr(f) & 0x1ff) % CY_AT45_PAGE; }

static void at45_program(cybiko_t *m, uint32_t page, const uint8_t *src)
{
    cy_at45_t *f = &m->at45;
    uint8_t *p = f->mem + page * CY_AT45_PAGE;
    f->writes++;
    if (memcmp(p, src, CY_AT45_PAGE) == 0) return;
    memcpy(p, src, CY_AT45_PAGE);
    f->dirty[page / 32] |= 1u << (page % 32);
}

/* Chip select going high ends a command; the program commands write their
 * buffer into the page then. */
static void at45_end(cybiko_t *m)
{
    cy_at45_t *f = &m->at45;
    if (f->cmd_len >= 4) {
        uint32_t page = at45_page(f);
        uint8_t *p = f->mem + page * CY_AT45_PAGE;
        switch (f->cmd[0]) {
        case 0x82: case 0x83: case 0x88: at45_program(m, page, f->buffer[0]); break;
        case 0x85: case 0x86: case 0x89: at45_program(m, page, f->buffer[1]); break;
        case 0x81: {                                  /* page erase */
            uint8_t blank[CY_AT45_PAGE];
            memset(blank, 0xff, sizeof blank);
            at45_program(m, page, blank);
            break;
        }
        case 0x50: {                                  /* block erase: 8 pages */
            uint8_t blank[CY_AT45_PAGE];
            memset(blank, 0xff, sizeof blank);
            for (uint32_t i = 0; i < 8; i++) at45_program(m, (page & ~7u) + i, blank);
            break;
        }
        case 0x58: case 0x59: break;                  /* auto page rewrite: no change */
        default: (void)p; break;
        }
    }
    f->cmd_len = 0;
    f->output = false;
    f->out = NULL;
    f->programming = false;
}

void cy_at45_cs(cybiko_t *m, bool selected)
{
    cy_at45_t *f = &m->at45;
    if (f->selected == selected) return;
    if (!selected) at45_end(m);
    f->selected = selected;
}

static void at45_unknown(cy_at45_t *f, uint8_t op)
{
    f->unknown++;
    f->last_unknown = op;
}

/* One byte each way on SCI1 (clocked sync). */
uint8_t cy_at45_spi(cybiko_t *m, uint8_t in)
{
    cy_at45_t *f = &m->at45;
    if (!f->selected) return 0xff;
    if (f->output) {
        if (!f->out) return f->status;
        uint8_t v = f->out[f->out_pos];
        if (++f->out_pos >= CY_AT45_PAGE) {
            f->out_pos = 0;
            /* a continuous array read (68, e8) goes on into the next page */
            if (f->cmd[0] == 0x68 || f->cmd[0] == 0xe8) {
                size_t next = (size_t)(f->out - f->mem) + CY_AT45_PAGE;
                f->out = f->mem + (next < CY_AT45_SIZE ? next : 0);
            }
        }
        return v;
    }
    if (f->programming) {
        f->buffer[f->cmd[0] == 0x87 || f->cmd[0] == 0x85][f->prog_pos] = in;
        if (++f->prog_pos >= CY_AT45_PAGE) f->prog_pos = 0;
        return 0xff;
    }
    if (f->cmd_len >= sizeof f->cmd) return 0xff;     /* command done, waiting for CS */
    f->cmd[f->cmd_len++] = in;
    const uint8_t op = f->cmd[0], n = f->cmd_len;
    switch (op) {
    case 0x57: case 0xd7:                             /* status */
        f->status = (uint8_t)((f->status & ST_MISMATCH) | ST_READY | ST_DB081);
        f->output = true;
        f->out = NULL;
        break;
    case 0x52: case 0xd2: case 0x68: case 0xe8:       /* page / continuous read */
        if (n == 8) {
            f->out = f->mem + at45_page(f) * CY_AT45_PAGE;
            f->out_pos = at45_byte(f);
            f->output = true;
        }
        break;
    case 0x54: case 0xd4: case 0x56: case 0xd6:       /* buffer read */
        if (n == 5) {
            f->out = f->buffer[op == 0x56 || op == 0xd6];
            f->out_pos = at45_byte(f);
            f->output = true;
        }
        break;
    case 0x53: case 0x55:                             /* page to buffer */
        if (n == 4) memcpy(f->buffer[op == 0x55], f->mem + at45_page(f) * CY_AT45_PAGE, CY_AT45_PAGE);
        break;
    case 0x60: case 0x61:                             /* compare page with buffer */
        if (n == 4) {
            bool same = !memcmp(f->buffer[op == 0x61], f->mem + at45_page(f) * CY_AT45_PAGE, CY_AT45_PAGE);
            f->status = (uint8_t)(same ? f->status & ~ST_MISMATCH : f->status | ST_MISMATCH);
        }
        break;
    case 0x82: case 0x85:                             /* program through buffer (with erase) */
        if (n == 4) {
            memset(f->buffer[op == 0x85], 0xff, CY_AT45_PAGE);
            f->prog_pos = at45_byte(f);
            f->programming = true;
        }
        break;
    case 0x84: case 0x87:                             /* buffer write */
        if (n == 4) {
            f->prog_pos = at45_byte(f);
            f->programming = true;
        }
        break;
    case 0x83: case 0x86: case 0x88: case 0x89:       /* buffer to page, done at CS high */
    case 0x81: case 0x50: case 0x58: case 0x59:       /* erase, rewrite */
        break;
    default:
        at45_unknown(f, op);
        f->cmd_len = sizeof f->cmd;                   /* ignore the rest until CS goes high */
        break;
    }
    return 0xff;
}

void cy_at45_reset(cybiko_t *m)
{
    cy_at45_t *f = &m->at45;
    f->selected = false;
    f->cmd_len = 0;
    f->output = f->programming = false;
    f->out = NULL;
    f->status = ST_READY | ST_DB081;
}

int cybiko_at45_take_dirty(cybiko_t *m, uint32_t *bits)
{
    int n = 0;
    for (size_t i = 0; i < CY_AT45_PAGES / 32; i++) {
        bits[i] = m->at45.dirty[i];
        n += __builtin_popcount(bits[i]);
        m->at45.dirty[i] = 0;
    }
    return n;
}

/* The Cybiko ID. CyOS 1.2.46 reads it at start into a word in RAM, which
 * its radio uses from then on; where the Classic keeps it in the flash is not
 * known, so a given ID goes into that word, again whenever CyOS changes it.
 * The address and the version string that tells this CyOS apart are
 * ReBiko's (sci0_v1.c); the word is written only once CyOS's radio object
 * exists, since the boot loader uses that RAM before CyOS runs. Other CyOS
 * versions keep the ID of their flash. */
#define V1246_VERSION_AT 0x20364au
#define V1246_RADIO_OBJ  0x21f216u
#define V1246_CYID       0x21f9dcu

static uint32_t ram32(const cybiko_t *m, uint32_t a)
{
    const uint8_t *p = m->ram + (a - 0x200000u);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool cyos_1246_running(const cybiko_t *m)
{
    if (memcmp(m->ram + (V1246_VERSION_AT - 0x200000u), "1.2.46", 6)) return false;
    uint32_t obj = ram32(m, V1246_RADIO_OBJ);
    return obj >= 0x200000u && obj < 0x200000u + CY_CLASSIC_RAM_SIZE;
}

uint32_t cy_classic_cyos_id(const cybiko_t *m)
{
    return cyos_1246_running(m) ? ram32(m, V1246_CYID) : 0;
}

void cy_classic_poll(cybiko_t *m)
{
    uint32_t id = m->rf.classic_id;
    if (!id || !cyos_1246_running(m) || ram32(m, V1246_CYID) == id) return;
    uint8_t *p = m->ram + (V1246_CYID - 0x200000u);
    p[0] = (uint8_t)(id >> 24); p[1] = (uint8_t)(id >> 16); p[2] = (uint8_t)(id >> 8); p[3] = (uint8_t)id;
}

/* An empty CyOS block: an 8-byte header (CRC-32 of the 254 bytes after it,
 * big-endian; a 16-bit write count; a check word over the first six header
 * bytes, stored low byte first), 256 bytes of block data with bit 7 of the
 * first byte clear, and a 2-byte trailer. */
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

void cybiko_at45_format_free(uint8_t *flash, uint32_t first)
{
    uint8_t page[CY_AT45_PAGE];
    memset(page, 0xff, sizeof page);
    page[8] = 0x7f;
    page[4] = page[5] = 0;
    uint32_t crc = crc32(page + 8, CY_AT45_PAGE - 10);
    page[0] = (uint8_t)(crc >> 24);
    page[1] = (uint8_t)(crc >> 16);
    page[2] = (uint8_t)(crc >> 8);
    page[3] = (uint8_t)crc;
    unsigned v = 0xaf17u ^ ((unsigned)page[0] << 8 | page[1]) ^ ((unsigned)page[2] << 8 | page[3]) ^
                 ((unsigned)page[4] << 8 | page[5]);
    page[6] = (uint8_t)v;
    page[7] = (uint8_t)(v >> 8);
    for (uint32_t p = first; p < CY_AT45_PAGES; p++) memcpy(flash + p * CY_AT45_PAGE, page, CY_AT45_PAGE);
}
