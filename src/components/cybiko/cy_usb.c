/* The Xtreme's USB device controller, a National USBN9604, on the bus at
 * 200000-200003 in its non-multiplexed mode: a write with A0 set (200003)
 * picks a register, A0 clear (200000) reads or writes it. Its interrupt
 * line is IRQ1. Register map and behaviour from the USBN9603/9604
 * datasheet, as far as CyOS 1.5.08's driver uses them:
 *
 *   00 MCNTRL  01 CCONF  03 RID  04 FAR  05 NFSR
 *   06 MAEV  07 MAMSK  08 ALTEV  09 ALTMSK  0a TXEV  0b TXMSK
 *   0c RXEV  0d RXMSK  0e NAKEV  0f NAKMSK  10 FWEV  11 FWMSK
 *   12 FNH  13 FNL  14-19 DMA  1b WKUP
 *   20 EPC0  21 TXD0  22 TXS0  23 TXC0  25 RXD0  26 RXS0  27 RXC0
 *   28 EPC1  29 TXD1  2a TXS1  2b TXC1      TX FIFO 1, for EPC1
 *   2c EPC2  2d RXD1  2e RXS1  2f RXC1      RX FIFO 1, for EPC2
 *   30 EPC3  31-33  TX FIFO 2   34 EPC4  35-37  RX FIFO 2
 *   38 EPC5  39-3b  TX FIFO 3   3c EPC6  3d-3f  RX FIFO 3
 *
 * The host side (cybiko_usb_*) works a transaction at a time: a SETUP, an
 * OUT or an IN to an endpoint, answered ACK, NAK or STALL the way the chip
 * would. DMA, isochronous endpoints, FIFO warnings and suspend are not
 * modelled; CyOS doesn't use them. See usb.md in cybiko-docs. */
#include <string.h>
#include "cy_internal.h"

enum {
    MCNTRL = 0x00, RID = 0x03, FAR = 0x04, NFSR = 0x05, MAEV = 0x06, MAMSK = 0x07,
    ALTEV = 0x08, ALTMSK = 0x09, TXEV = 0x0a, TXMSK = 0x0b, RXEV = 0x0c, RXMSK = 0x0d,
    NAKEV = 0x0e, NAKMSK = 0x0f, FWEV = 0x10, FWMSK = 0x11, FNH = 0x12, FNL = 0x13,
    EPC0 = 0x20,
};

/* TXSx / RXSx bits */
#define TX_URUN  0x80
#define ACK_STAT 0x40
#define TX_DONE  0x20
#define RX_ERR   0x80
#define SETUP    0x40
#define RX_TOGGLE 0x20
#define RX_LAST  0x10
/* TXCx / RXCx bits */
#define TX_EN    0x01
#define TX_TOGGLE 0x04
#define FLUSH    0x08
#define RX_EN    0x01
#define IGN_OUT  0x02
#define IGN_SETUP 0x04
/* EPCx */
#define EP_EN    0x10
#define EP_STALL 0x80
#define EPC0_DEF 0x40
/* ALTEV */
#define ALT_RESET 0x40

static int fifo_size(int f) { return f ? 64 : 8; }

static bool int_line(const cybiko_t *m);

/* MAEV is made from the other event registers and their masks. */
static uint8_t maev(const cy_usb_t *u)
{
    uint8_t v = u->maev & 0x28;                 /* ULD, FRAME: latched */
    if (u->r[RXEV] & u->r[RXMSK]) v |= 0x40;
    if (u->r[NAKEV] & u->r[NAKMSK]) v |= 0x10;
    if (u->r[TXEV] & u->r[TXMSK]) v |= 0x04;
    if (u->r[ALTEV] & u->r[ALTMSK]) v |= 0x02;
    if (u->r[FWEV] & u->r[FWMSK]) v |= 0x01;
    return v;
}

static bool int_line(const cybiko_t *m)
{
    const cy_usb_t *u = &m->usb;
    if (!(u->r[MCNTRL] & 0xc0) || !(u->r[MAMSK] & 0x80)) return false;
    return (maev(u) & u->r[MAMSK] & 0x7f) != 0;
}

uint8_t cy_usb_irq_pins(const cybiko_t *m)
{
    return int_line(m) ? 0x02 : 0x00;           /* IRQ1 */
}

void cy_usb_reset(cybiko_t *m)
{
    cy_usb_t *u = &m->usb;
    bool vbus = u->vbus;
    memset(u, 0, sizeof *u);
    u->vbus = vbus;
    u->r[RID] = 0x02;
    u->r[0x01] = 0x0b;                          /* CCONF: 4 MHz clock out */
    u->r[FNH] = 0xc0;
    u->r[0x1b] = 0x0f;                          /* WKUP */
}

/* Software reset (MCNTRL.SRST): everything but CCONF. */
static void soft_reset(cybiko_t *m)
{
    cy_usb_t *u = &m->usb;
    uint8_t cconf = u->r[0x01];
    cy_usb_reset(m);
    u->r[0x01] = cconf;
}

/* Registers 20-27 are endpoint 0's; 28-3f hold blocks 1-6 of four
 * (EPCx, data, status, command). Odd blocks transmit through TX FIFO
 * (b + 1) / 2, even ones receive through RX FIFO b / 2. */
#define EPC(b) (0x24 + 4 * (b))
static int ep_block(uint8_t reg) { return ((reg - 0x28) >> 2) + 1; }
static int block_fifo(int b) { return (b + 1) >> 1; }

static uint8_t txs(const cy_usb_t *u, int f)
{
    int free = fifo_size(f) - u->tx_len[f];
    if (free > 31) free = 31;
    return (uint8_t)(u->txs[f] | free);
}

static uint8_t rxs(const cy_usb_t *u, int f)
{
    int n = u->rx_len[f] - u->rx_pos[f];
    if (n > 15) n = 15;
    return (uint8_t)(u->rxs[f] | n);
}

static uint8_t tx_status_read(cy_usb_t *u, int f)
{
    uint8_t v = txs(u, f);
    u->txs[f] &= (uint8_t)~(ACK_STAT | TX_DONE);
    u->r[TXEV] &= (uint8_t)~(1 << f);
    return v;
}

static uint8_t rx_status_read(cy_usb_t *u, int f)
{
    uint8_t v = rxs(u, f);
    u->rxs[f] &= (uint8_t)~(SETUP | RX_TOGGLE | RX_LAST | RX_ERR);
    u->r[RXEV] &= (uint8_t)~(1 << f);
    return v;
}

static uint8_t rx_data_read(cy_usb_t *u, int f)
{
    if (u->rx_pos[f] < u->rx_len[f]) return u->rx[f][u->rx_pos[f]++];
    return u->rx_len[f] ? u->rx[f][u->rx_len[f] - 1] : 0;   /* repeats the last byte */
}

static uint8_t reg_read(cybiko_t *m, uint8_t a)
{
    cy_usb_t *u = &m->usb;
    uint8_t v;
    if (a >= 0x20 && a < 0x28) {
        switch (a) {
        case 0x22: return tx_status_read(u, 0);
        case 0x25: return rx_data_read(u, 0);
        case 0x26: return rx_status_read(u, 0);
        }
        return u->r[a];
    }
    if (a >= 0x28) {
        int b = ep_block(a), f = block_fifo(b);
        bool tx = b & 1;
        switch (a & 3) {
        case 1: return tx ? u->r[a] : rx_data_read(u, f);
        case 2: return tx ? tx_status_read(u, f) : rx_status_read(u, f);
        }
        return u->r[a];
    }
    switch (a) {
    case MAEV: v = maev(u); u->maev = 0; return v;
    case ALTEV: v = u->r[ALTEV]; u->r[ALTEV] &= 0x06; return v;
    case RXEV: v = u->r[RXEV]; u->r[RXEV] &= 0x0f; return v;
    case NAKEV: v = u->r[NAKEV]; u->r[NAKEV] = 0; return v;
    case FNL: u->fnh_latch = (uint8_t)((u->r[FNH] & 0xc0) | (u->frame >> 8 & 7)); return (uint8_t)u->frame;
    case FNH: return u->fnh_latch;
    }
    return u->r[a];
}

static void tx_data_write(cy_usb_t *u, int f, uint8_t v)
{
    if (u->tx_len[f] < fifo_size(f)) u->tx[f][u->tx_len[f]++] = v;
    else u->overflows++;
}

static void reg_write(cybiko_t *m, uint8_t a, uint8_t v)
{
    cy_usb_t *u = &m->usb;
    if (a >= 0x20) {
        bool tx;
        int f;
        if (a < 0x28) { tx = a < 0x24; f = 0; }
        else { int b = ep_block(a); tx = b & 1; f = block_fifo(b); }
        switch (a & 3) {
        case 1: if (tx) tx_data_write(u, f, v); break;
        case 2: return;                         /* status: read only */
        case 3:
            if (v & FLUSH) {
                if (tx) { u->tx_len[f] = 0; u->txs[f] = 0; }
                else u->rx_len[f] = u->rx_pos[f] = 0;
                v &= (uint8_t)~FLUSH;
            }
            break;
        }
        u->r[a] = v;
        return;
    }
    switch (a) {
    case MCNTRL:
        if (v & 0x01) { soft_reset(m); return; }
        u->r[a] = v;
        return;
    case RID: case MAEV: case TXEV: case RXEV: case FNL: return;
    case ALTEV: case NAKEV: case FWEV: return;
    case FNH: if (v & 0x20) u->frame = 0; return;
    }
    u->r[a] = v;
}

void cy_usb_bus_write(cybiko_t *m, uint32_t a, uint8_t v)
{
    uint8_t old = cy_irq_pins(m);
    if (a & 1) m->usb.addr = v & 0x3f;
    else reg_write(m, m->usb.addr, v);
    cy_irq_pins_changed(m, old);
}

uint8_t cy_usb_bus_read(cybiko_t *m, uint32_t a)
{
    if (a & 1) return 0xff;                     /* reserved */
    uint8_t old = cy_irq_pins(m);
    uint8_t v = reg_read(m, m->usb.addr);
    cy_irq_pins_changed(m, old);
    return v;
}

/* ---- the host's side ---------------------------------------------- */

bool cybiko_usb_attached(const cybiko_t *m)
{
    /* MCNTRL.NAT (or the regulator, VGE, which CyOS sets with it) puts
     * the pull-up on D+. */
    return m->usb.vbus && (m->usb.r[MCNTRL] & 0x18) != 0;
}

void cybiko_usb_vbus(cybiko_t *m, bool on)
{
    m->usb.vbus = on;
}

void cybiko_usb_bus_reset(cybiko_t *m)
{
    cy_usb_t *u = &m->usb;
    uint8_t old = cy_irq_pins(m);
    u->r[ALTEV] |= ALT_RESET;
    cy_irq_pins_changed(m, old);
}

void cybiko_usb_frame(cybiko_t *m)
{
    m->usb.frame = (uint16_t)((m->usb.frame + 1) & 0x7ff);
}

static bool addressed(const cy_usb_t *u, int addr)
{
    if ((u->r[NFSR] & 3) != 2) return false;    /* NodeOperational */
    if (addr == 0 && (u->r[EPC0] & EPC0_DEF)) return true;
    return (u->r[FAR] & 0x80) && (u->r[FAR] & 0x7f) == addr;
}

/* The endpoint block (1-6) that serves endpoint number ep in this
 * direction: odd blocks transmit (IN), even ones receive (OUT). */
static int find_block(const cy_usb_t *u, int ep, bool in)
{
    for (int b = in ? 1 : 2; b <= 6; b += 2) {
        uint8_t epc = u->r[EPC(b)];
        if ((epc & EP_EN) && (epc & 0x0f) == ep) return b;
    }
    return -1;
}

static void nak(cybiko_t *m, int fifo, bool in)
{
    m->usb.r[NAKEV] |= (uint8_t)(1 << (fifo + (in ? 0 : 4)));
}

int cybiko_usb_setup(cybiko_t *m, int addr, const uint8_t setup[8])
{
    cy_usb_t *u = &m->usb;
    if (!cybiko_usb_attached(m) || !addressed(u, addr)) return CY_USB_TIMEOUT;
    if (u->r[0x27] & IGN_SETUP) return CY_USB_TIMEOUT;
    uint8_t old = cy_irq_pins(m);
    /* A SETUP always lands: it clears a stall and ends the last control
     * transfer's stages. */
    u->r[EPC0] &= (uint8_t)~EP_STALL;
    memcpy(u->rx[0], setup, 8);
    u->rx_len[0] = 8;
    u->rx_pos[0] = 0;
    u->rxs[0] = SETUP | RX_LAST;
    u->r[0x23] &= (uint8_t)~TX_EN;
    u->r[0x27] &= (uint8_t)~RX_EN;
    u->r[RXEV] |= 0x01;
    cy_irq_pins_changed(m, old);
    return CY_USB_ACK;
}

int cybiko_usb_out(cybiko_t *m, int addr, int ep, const uint8_t *data, int len, int toggle)
{
    cy_usb_t *u = &m->usb;
    if (!cybiko_usb_attached(m) || !addressed(u, addr)) return CY_USB_TIMEOUT;
    uint8_t old = cy_irq_pins(m);
    int res;
    if (ep == 0) {
        uint8_t rxc = u->r[0x27];
        if (rxc & IGN_OUT) res = CY_USB_TIMEOUT;
        else if (u->r[EPC0] & EP_STALL) res = CY_USB_STALL;
        else if (!(rxc & RX_EN) || len > 8) { nak(m, 0, false); res = CY_USB_NAK; }
        else {
            memcpy(u->rx[0], data, (size_t)len);
            u->rx_len[0] = (uint8_t)len;
            u->rx_pos[0] = 0;
            u->rxs[0] = (uint8_t)(RX_LAST | (toggle ? RX_TOGGLE : 0));
            u->r[0x27] &= (uint8_t)~RX_EN;
            u->r[RXEV] |= 0x01;
            res = CY_USB_ACK;
        }
    } else {
        int b = find_block(u, ep, false);
        if (b < 0) res = CY_USB_TIMEOUT;
        else {
            int f = block_fifo(b);
            uint8_t epc = u->r[EPC(b)], rxc = u->r[EPC(b) + 3];
            if (epc & EP_STALL) res = CY_USB_STALL;
            else if (!(rxc & RX_EN) || u->rx_pos[f] < u->rx_len[f] || len > 64) { nak(m, f, false); res = CY_USB_NAK; }
            else {
                memcpy(u->rx[f], data, (size_t)len);
                u->rx_len[f] = (uint8_t)len;
                u->rx_pos[f] = 0;
                u->rxs[f] = (uint8_t)(RX_LAST | (toggle ? RX_TOGGLE : 0));
                u->r[RXEV] |= (uint8_t)(1 << f);
                res = CY_USB_ACK;
            }
        }
    }
    cy_irq_pins_changed(m, old);
    return res;
}

int cybiko_usb_in(cybiko_t *m, int addr, int ep, uint8_t *data, int max, int *len, int *toggle)
{
    cy_usb_t *u = &m->usb;
    *len = 0;
    if (!cybiko_usb_attached(m) || !addressed(u, addr)) return CY_USB_TIMEOUT;
    uint8_t old = cy_irq_pins(m);
    int res, f = 0, txc_reg = 0x23;
    uint8_t epc = u->r[EPC0];
    bool nak_only = false;
    if (ep == 0) nak_only = (u->r[0x27] & RX_EN) != 0;     /* RX_EN overrides TX_EN */
    else {
        int b = find_block(u, ep, true);
        if (b < 0) { cy_irq_pins_changed(m, old); return CY_USB_TIMEOUT; }
        f = block_fifo(b);
        txc_reg = EPC(b) + 3;
        epc = u->r[EPC(b)];
    }
    if (epc & EP_STALL) res = CY_USB_STALL;
    else if (nak_only || !(u->r[txc_reg] & TX_EN)) { nak(m, f, true); res = CY_USB_NAK; }
    else {
        int n = u->tx_len[f] < max ? u->tx_len[f] : max;
        memcpy(data, u->tx[f], (size_t)n);
        *len = n;
        *toggle = (u->r[txc_reg] & TX_TOGGLE) != 0;
        u->tx_len[f] = 0;
        u->r[txc_reg] &= (uint8_t)~TX_EN;
        u->txs[f] = TX_DONE | ACK_STAT;
        u->r[TXEV] |= (uint8_t)(1 << f);
        /* EPC0.DEF ends after an IN (the SET_ADDRESS status stage). */
        if (f == 0) u->r[EPC0] &= (uint8_t)~EPC0_DEF;
        res = CY_USB_ACK;
    }
    cy_irq_pins_changed(m, old);
    return res;
}
