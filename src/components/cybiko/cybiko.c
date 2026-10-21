/* Cybiko Xtreme: memory map, interrupt controller, I/O ports, system
 * registers and the run loop. See cybiko.h for the overall map. */
#include <string.h>
#include "cy_internal.h"

#define FLASH_BASE 0x600000u
#define RAM_BASE   0x400000u
#define IRAM_BASE  0xffdc00u
#define IO_BASE    0xfffe80u

/* ------------------------------------------------------------------ */
/* Interrupt controller                                                */
/* ------------------------------------------------------------------ */

/* IPR slot of each vector (MAME h8s_intc_device::vector_to_slot). The IPR
 * byte is slot / 2, high nibble for even slots. */
static const int8_t vector_slot[92] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     0,  1,  2,  2,  3,  3,  4,  4,  5,  6,  7,  8,  9,  9,  9,  9,
    10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12,
    13, 13, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15,
    16, 16, 16, 16, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18,
    19, 19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21,
};

static bool irq_level_type(const cybiko_t *m, int n)
{
    return ((m->iscr >> (2 * n)) & 3) == 0;
}

/* IRQ0-7 pin inputs; bit set = pin asserted (low). */
static uint8_t irq_pins(const cybiko_t *m)
{
    /* The On/Off button is on PA7, which doubles as IRQ7 on the H8S/2323;
     * pressing it is what can wake the CPU from software standby. */
    bool onoff = (m->keys[CYK_ONOFF >> 4] >> (CYK_ONOFF & 15)) & 1;
    return onoff ? 0x80 : 0x00;
}

static void irq_check_levels(cybiko_t *m)
{
    uint8_t in = irq_pins(m);
    for (int n = 0; n < 8; n++)
        if (irq_level_type(m, n) && (in >> n & 1)) m->isr |= (uint8_t)(1 << n);
    m->cpu.irq_check = true;
}

static void irq_pins_changed(cybiko_t *m, uint8_t old_in)
{
    uint8_t in = irq_pins(m);
    for (int n = 0; n < 8; n++) {
        bool cur = old_in >> n & 1, now = in >> n & 1;
        bool set = false;
        switch ((m->iscr >> (2 * n)) & 3) {
        case 0: set = now; break;               /* low level */
        case 1: set = now && !cur; break;       /* falling edge */
        case 2: set = !now && cur; break;       /* rising edge */
        case 3: set = now != cur; break;        /* both edges */
        }
        if (set) m->isr |= (uint8_t)(1 << n);
    }
    m->cpu.irq_check = true;
}

static int irq_ack(void *ctx, h8s_cpu_t *cpu, int *level)
{
    cybiko_t *m = ctx;
    uint32_t words[3] = {
        m->pend[0] | (uint32_t)(m->isr & m->ier) << CY_VEC_IRQ0,
        m->pend[1], m->pend[2],
    };
    if (m->standby) {
        /* Clocks are stopped; only NMI and the IRQ pins can wake the CPU. */
        words[0] &= (1u << H8S_VEC_NMI) | 0xffu << CY_VEC_IRQ0;
        words[1] = words[2] = 0;
    }
    if (!(words[0] | words[1] | words[2])) return 0;

    bool mode2 = (m->syscr & 0x30) == 0x20;
    int best = 0, best_level = -1;
    for (int w = 0; w < 3; w++) {
        uint32_t p = words[w];
        while (p) {
            int b = __builtin_ctz(p);
            p &= p - 1;
            int v = w * 32 + b;
            int lvl;
            if (v == H8S_VEC_NMI) {
                lvl = 8;
            } else {
                int slot = v < 92 ? vector_slot[v] : -1;
                int ipr = slot < 0 ? 0 : (m->ipr[slot >> 1] >> (slot & 1 ? 0 : 4)) & 7;
                if (mode2) {
                    if (ipr <= (cpu->exr & 7)) continue;
                    lvl = ipr;
                } else {
                    if (cpu->ccr & H8S_CCR_I) continue;
                    lvl = 0;                /* fixed order: lowest vector wins */
                }
            }
            if (lvl > best_level) { best = v; best_level = lvl; }
        }
    }
    if (!best) return 0;

    if (best >= CY_VEC_IRQ0 && best < CY_VEC_IRQ0 + 8) {
        int n = best - CY_VEC_IRQ0;
        if (!irq_level_type(m, n) || !(irq_pins(m) >> n & 1)) m->isr &= (uint8_t)~(1 << n);
    } else {
        m->pend[best >> 5] &= ~(1u << (best & 31));
    }
    if (m->standby) m->standby = false;
    *level = best_level;
    return best;
}

/* ------------------------------------------------------------------ */
/* Ports                                                               */
/* ------------------------------------------------------------------ */

/* Pins the port does not have read as 1 (MAME h8_port masks). */
static uint8_t port_mask(int p)
{
    switch (p) {
    case 2: return 0xc0;      /* port 3 */
    case 4: return 0xf0;      /* port 5 */
    case 15: return 0xe0;     /* port G */
    default: return 0x00;
    }
}

static uint8_t port_input(cybiko_t *m, int p)
{
    switch (p) {
    case 9: {                 /* port A: bit 7 On/Off released, bit 6 charged */
        bool onoff = (m->keys[CYK_ONOFF >> 4] >> (CYK_ONOFF & 15)) & 1;
        return onoff ? 0x40 : 0xc0;
    }
    case 14:                  /* port F: bit 6 RTC SDA */
        return cy_rtc_sda(m) ? 0x40 : 0x00;
    default:
        return 0xff;
    }
}

static uint8_t port_read(cybiko_t *m, int p)
{
    uint8_t mask = port_mask(p);
    uint8_t ddr = m->ddr[p];
    return (uint8_t)(mask | (m->dr[p] & ddr) | (port_input(m, p) & ~ddr & ~mask));
}

static void port_output(cybiko_t *m, int p)
{
    uint8_t data = (uint8_t)((m->dr[p] | ~m->ddr[p]) & ~port_mask(p));
    if (p == 14) {
        /* PF1 drives the RTC clock; SDA follows MAME's wiring: the line is
         * pulled low while PF6 outputs 1 or floats. */
        cy_rtc_pins(m, data & 0x02, !(data & 0x40));
    }
}

/* ------------------------------------------------------------------ */
/* On-chip registers fe80-ffff outside the peripherals                 */
/* ------------------------------------------------------------------ */

static uint8_t io_read(cybiko_t *m, uint32_t a)
{
    uint8_t v;
    cy_sync(m);
    if (cy_periph_read(m, a, &v)) return v;
    switch (a) {
    case 0xffff2c: return (uint8_t)(m->iscr >> 8);
    case 0xffff2d: return (uint8_t)m->iscr;
    case 0xffff2e: return m->ier;
    case 0xffff2f: return m->isr;
    case 0xffff38: return m->sbycr;
    case 0xffff39: return m->syscr;
    case 0xffff3a: return m->sckcr;
    case 0xffff3b: return 0x87;       /* MDCR: mode 7, as MAME */
    case 0xffff3c: return m->mstpcr[0];
    case 0xffff3d: return m->mstpcr[1];
    }
    if (a >= 0xfffec4 && a <= 0xfffece) return m->ipr[a - 0xfffec4];
    if (a >= 0xfffeb0 && a <= 0xfffebf) return 0xff;              /* DDR: write-only */
    if (a >= 0xffff50 && a <= 0xffff5f) return port_read(m, (int)(a - 0xffff50));
    if (a >= 0xffff60 && a <= 0xffff6f) {
        int p = (int)(a - 0xffff60);
        return (uint8_t)(m->dr[p] | port_mask(p));
    }
    if (a >= 0xffff70 && a <= 0xffff74) return m->pcr[a - 0xffff70];
    if (a == 0xffff76 || a == 0xffff77) return m->odr[a - 0xffff76];
    return m->misc[a - IO_BASE];
}

static void io_write(cybiko_t *m, uint32_t a, uint8_t v)
{
    cy_sync(m);
    if (cy_periph_write(m, a, v)) return;
    switch (a) {
    case 0xffff2c: m->iscr = (uint16_t)((m->iscr & 0x00ff) | v << 8); irq_check_levels(m); return;
    case 0xffff2d: m->iscr = (uint16_t)((m->iscr & 0xff00) | v); irq_check_levels(m); return;
    case 0xffff2e: m->ier = v; m->cpu.irq_check = true; return;
    case 0xffff2f: m->isr &= v; irq_check_levels(m); return;
    case 0xffff38: m->sbycr = (uint8_t)(v & 0xf9); return;
    case 0xffff39:
        m->syscr = v;
        m->cpu.intm2 = (v & 0x30) == 0x20;
        /* RAME: on-chip RAM can be switched off, exposing external space. */
        m->cpu.fine_size = (v & 1) ? sizeof m->iram : 0;
        m->cpu.irq_check = true;
        return;
    case 0xffff3a: m->sckcr = v; return;
    case 0xffff3b: return;
    case 0xffff3c: m->mstpcr[0] = v; return;
    case 0xffff3d: m->mstpcr[1] = v; return;
    }
    if (a >= 0xfffec4 && a <= 0xfffece) { m->ipr[a - 0xfffec4] = v; m->cpu.irq_check = true; return; }
    if (a >= 0xfffeb0 && a <= 0xfffebf) { int p = (int)(a - 0xfffeb0); m->ddr[p] = v; port_output(m, p); return; }
    if (a >= 0xffff60 && a <= 0xffff6f) { int p = (int)(a - 0xffff60); m->dr[p] = v; port_output(m, p); return; }
    if (a >= 0xffff70 && a <= 0xffff74) { m->pcr[a - 0xffff70] = v; return; }
    if (a == 0xffff76 || a == 0xffff77) { m->odr[a - 0xffff76] = v; return; }
    if (a >= 0xffff30 && a <= 0xffff37 && v) m->dtc_dma_use++;     /* DTC enables */
    if (a >= 0xffff06 && a <= 0xffff07 && v) m->dtc_dma_use++;     /* DMABCR */
    m->misc[a - IO_BASE] = v;
}

/* ------------------------------------------------------------------ */
/* Slow-path bus                                                       */
/* ------------------------------------------------------------------ */

static uint16_t keyboard_read(cybiko_t *m, uint32_t a)
{
    uint32_t sel = (a - 0xe00000u) >> 1;
    uint16_t v = 0xffff;
    for (int row = 0; row < 15; row++)
        if (!(sel >> row & 1)) v &= (uint16_t)~m->keys[row];
    return v;
}

static uint16_t bus_read16(void *ctx, uint32_t a)
{
    cybiko_t *m = ctx;
    if (a >= 0xfffe80u) return (uint16_t)(io_read(m, a) << 8 | io_read(m, a + 1));
    if (a >= FLASH_BASE && a < 0x800000u) return cy_flash_read16(m, (a - FLASH_BASE) & (CY_FLASH_SIZE - 1));
    if (a >= 0xe00000u && a < 0xf00000u) return keyboard_read(m, a);
    if (a == 0x100000u) return (uint16_t)(cy_lcd_read(&m->lcd, 0) << 8 | cy_lcd_read(&m->lcd, 1));
    if (a < 0x40000u) return (uint16_t)(m->rom[a & 0x7fff] << 8 | m->rom[(a & 0x7fff) + 1]);
    m->unmapped_reads++;
    return 0;
}

static uint8_t bus_read8(void *ctx, uint32_t a)
{
    cybiko_t *m = ctx;
    if (a >= 0xfffe80u) return io_read(m, a);
    if (a == 0x100000u || a == 0x100001u) return cy_lcd_read(&m->lcd, (int)(a & 1));
    uint16_t w = bus_read16(ctx, a & ~1u);
    return (uint8_t)(a & 1 ? w : w >> 8);
}

static void bus_write16(void *ctx, uint32_t a, uint16_t v)
{
    cybiko_t *m = ctx;
    if (a >= 0xfffe80u) {
        cy_sync(m);
        if (cy_periph_write16(m, a, v)) return;
        io_write(m, a, (uint8_t)(v >> 8));
        io_write(m, a + 1, (uint8_t)v);
        return;
    }
    if (a >= FLASH_BASE && a < 0x800000u) { cy_flash_write16(m, (a - FLASH_BASE) & (CY_FLASH_SIZE - 1), v); return; }
    if (a == 0x100000u) {
        cy_lcd_write(&m->lcd, 0, (uint8_t)(v >> 8));
        cy_lcd_write(&m->lcd, 1, (uint8_t)v);
        return;
    }
    if (a >= 0x200000u && a < 0x200004u) return;        /* USB controller */
    if (a < 0x40000u) return;                            /* ROM */
    m->unmapped_writes++;
}

static void bus_write8(void *ctx, uint32_t a, uint8_t v)
{
    cybiko_t *m = ctx;
    if (a >= 0xfffe80u) { io_write(m, a, v); return; }
    if (a == 0x100000u || a == 0x100001u) { cy_lcd_write(&m->lcd, (int)(a & 1), v); return; }
    if (a >= FLASH_BASE && a < 0x800000u) {
        /* Commands are written a byte at a time on some paths; the chip
         * sees the byte on its data lines with the word address. */
        cy_flash_write16(m, (a - FLASH_BASE) & (CY_FLASH_SIZE - 1) & ~1u, v);
        return;
    }
    if (a >= 0x200000u && a < 0x200004u) return;
    if (a < 0x40000u) return;
    m->unmapped_writes++;
}

static void bus_illegal(void *ctx, h8s_cpu_t *cpu, uint16_t op)
{
    (void)cpu; (void)op;
    ((cybiko_t *)ctx)->illegal_ops++;
}

/* ------------------------------------------------------------------ */
/* Machine                                                             */
/* ------------------------------------------------------------------ */

void cy_sync(cybiko_t *m)
{
    uint64_t t = h8s_time(&m->cpu);
    if (t <= m->synced) return;
    uint64_t d = t - m->synced;
    m->synced = t;
    if (m->standby) return;            /* clocks stopped */
    while (d) {
        uint32_t step = d > 0x40000000u ? 0x40000000u : (uint32_t)d;
        cy_periph_advance(m, step);
        d -= step;
    }
}

void cybiko_reset(cybiko_t *m)
{
    memset(m->pend, 0, sizeof m->pend);
    memset(m->ipr, 0x77, sizeof m->ipr);
    m->ier = m->isr = 0;
    m->iscr = 0;
    m->syscr = 0x01;
    m->sbycr = 0x08;
    m->sckcr = 0;
    m->mstpcr[0] = 0x3f;
    m->mstpcr[1] = 0xff;
    memset(m->misc, 0, sizeof m->misc);
    memset(m->dr, 0, sizeof m->dr);
    memset(m->ddr, 0, sizeof m->ddr);
    memset(m->pcr, 0, sizeof m->pcr);
    memset(m->odr, 0, sizeof m->odr);
    m->standby = false;
    m->reset_pending = false;

    cy_periph_reset(m);
    cy_lcd_reset(&m->lcd);
    cy_rtc_reset(m);
    cy_flash_reset(m);

    m->cpu.fine_base = IRAM_BASE;
    m->cpu.fine_size = sizeof m->iram;
    m->cpu.fine_mem = m->iram;
    m->cpu.intm2 = false;
    h8s_reset(&m->cpu);
    m->synced = h8s_time(&m->cpu);
}

void cybiko_init(cybiko_t *m, const cy_host_t *host, uint8_t *rom,
                 uint8_t *flash, uint8_t *ram)
{
    memset(m, 0, sizeof *m);
    if (host) m->host = *host;
    m->rom = rom;
    m->ram = ram;
    m->flash.mem = flash;
    memcpy(rom + CY_ROM_SIZE, rom, CY_ROM_SIZE);

    h8s_init(&m->cpu);
    m->cpu.ctx = m;
    m->cpu.bus.read8 = bus_read8;
    m->cpu.bus.read16 = bus_read16;
    m->cpu.bus.write8 = bus_write8;
    m->cpu.bus.write16 = bus_write16;
    m->cpu.bus.irq_ack = irq_ack;
    m->cpu.bus.illegal = bus_illegal;

    h8s_map(&m->cpu, 0x000000, 0x40000, rom, CY_ROM_BUF, false);
    h8s_map(&m->cpu, RAM_BASE, CY_RAM_SIZE, ram, CY_RAM_SIZE, true);
    h8s_map(&m->cpu, FLASH_BASE, 0x200000, flash, CY_FLASH_SIZE, false);
    cybiko_reset(m);
}

int32_t cybiko_run(cybiko_t *m, int32_t states)
{
    int32_t done = 0;
    while (done < states) {
        if (m->cpu.sleeping && (m->sbycr & 0x80) && !m->standby) {
            cy_sync(m);
            m->standby = true;         /* software standby: the OS switched off */
        }
        int32_t slice = states - done;
        if (!m->standby) {
            uint32_t next = cy_periph_next_event(m);
            if (next < (uint32_t)slice) slice = next ? (int32_t)next : 1;
        }
        done += h8s_run(&m->cpu, slice);
        cy_sync(m);
        if (m->reset_pending) {
            m->wdt_resets++;
            uint8_t rst = m->wdt_rst;
            cybiko_reset(m);
            m->wdt_rst = rst;      /* WOVF survives the reset it caused */
        }
    }
    return done;
}

void cybiko_key(cybiko_t *m, int key, bool down)
{
    int row = key >> 4, bit = key & 15;
    if (row >= 16) return;
    uint8_t old_in = irq_pins(m);
    if (down) m->keys[row] |= (uint16_t)(1 << bit);
    else m->keys[row] &= (uint16_t)~(1 << bit);
    if (irq_pins(m) != old_in) irq_pins_changed(m, old_in);
}

bool cybiko_serial_rx(cybiko_t *m, int port, uint8_t byte)
{
    if (port < 0 || port > 2) return false;
    cy_sci_t *s = &m->sci[port];
    if (s->rx_count >= sizeof s->rx_fifo) return false;
    s->rx_fifo[(s->rx_head + s->rx_count) % sizeof s->rx_fifo] = byte;
    s->rx_count++;
    cy_sci_kick(m);
    return true;
}

uint32_t cybiko_lcd_render(const cybiko_t *m, uint8_t *lum)
{
    const cy_lcd_t *l = &m->lcd;
    if (!(l->reg[0] & 0x40)) {                  /* display off */
        memset(lum, 255, CY_LCD_W * CY_LCD_H);
        return l->version;
    }
    uint8_t pen[4];
    for (int i = 0; i < 4; i++) {
        int t = 31 - (l->reg[0x0c + i] - l->reg[0x10] + 3);
        if (t < 0) t = 0;
        if (t > 31) t = 31;
        pen[i] = (uint8_t)(255 * t / 31);
    }
    /* RAM row 0 is the bottom line of the screen. */
    for (int y = 0; y < CY_LCD_H; y++) {
        const uint8_t *src = l->ram + (CY_LCD_H - 1 - y) * (CY_LCD_W / 4);
        uint8_t *dst = lum + y * CY_LCD_W;
        for (int x = 0; x < CY_LCD_W / 4; x++) {
            uint8_t b = src[x];
            dst[4 * x + 0] = pen[b >> 6 & 3];
            dst[4 * x + 1] = pen[b >> 4 & 3];
            dst[4 * x + 2] = pen[b >> 2 & 3];
            dst[4 * x + 3] = pen[b & 3];
        }
    }
    return l->version;
}
