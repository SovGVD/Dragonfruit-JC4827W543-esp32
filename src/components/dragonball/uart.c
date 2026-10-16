#include "dragonball.h"
#include "esp_log.h"

static const char *TAG = "palm_uart";

/*
 * MC68EZ328 UART model (HotSync groundwork).
 *
 * The UART is a byte pipe: a producer (debug console today, an ESP32 UART/TCP
 * transport later) pushes inbound bytes into the RX ring; the ROM's serial
 * driver reads them out via URX. Outbound bytes the ROM writes to UTX land in
 * the TX ring for a consumer to drain. INT_UART is raised/cleared from the CPU
 * task only (db_uart_update_irq), gated on the USTCNT enable bits, so the
 * interrupt-controller state stays single-threaded like every other source.
 *
 * Register bit layout follows the EZ328 User's Manual (see dragonball.h).
 */

#define UART_RING  256
#define UART_MASK  (UART_RING - 1)

/* Single-producer / single-consumer rings (lock-free; volatile head/tail with a
 * fence before publishing the new head). RX: producer = transport task, consumer
 * = CPU task. TX: producer = CPU task, consumer = transport task. */
static volatile uint16_t rx_head, rx_tail;
static uint8_t           rx_buf[UART_RING];
static volatile uint16_t tx_head, tx_tail;
static uint8_t           tx_buf[UART_RING];

static volatile bool     rx_overrun = false;
static bool              loopback   = false;
static bool              cpu_transport;
static uint32_t          generation;

void db_uart_set_cpu_transport(bool on)
{
    cpu_transport = on;
    if (on) loopback = false;
}

void db_uart_flush_cpu_transport(void)
{
    /* Safe only when BOTH ring endpoints belong to the emulation task. */
    if (cpu_transport) {
        rx_head = rx_tail = tx_head = tx_tail = 0;
        rx_overrun = false;
    }
}

void db_uart_reset(void)
{
    generation++;
    db_uart_flush_cpu_transport();
}

uint32_t db_uart_generation(void) { return generation; }

static inline uint16_t ring_used(uint16_t head, uint16_t tail)
{
    return (uint16_t)((head - tail) & UART_MASK);
}

/* --- Producer/consumer API (callable from any task) --- */

void db_uart_rx_push(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint16_t nh = (uint16_t)((rx_head + 1) & UART_MASK);
        if (nh == rx_tail) { rx_overrun = true; break; }  /* ring full */
        rx_buf[rx_head] = data[i];
        __sync_synchronize();
        rx_head = nh;
    }
    /* No db_uart_update_irq() here: that touches the interrupt controller and
     * must run on the CPU task. The CPU task re-evaluates the IRQ each batch. */
}

uint32_t db_uart_tx_pop(uint8_t *out, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && tx_tail != tx_head) {
        out[n++] = tx_buf[tx_tail];
        __sync_synchronize();
        tx_tail = (uint16_t)((tx_tail + 1) & UART_MASK);
    }
    return n;
}

/* Free slots in the RX ring. A transport reads at most this many bytes from its
 * source per pass so db_uart_rx_push never has to drop (which sets rx_overrun and
 * silently loses data — fatal to a HotSync stream). The unread bytes stay buffered
 * in the source (ESP UART driver ring + HW FIFO): that is our flow control on a
 * 3-wire link with no physical RTS/CTS. One slot is always kept free so a full
 * ring stays distinguishable from an empty one (head==tail). */
uint32_t db_uart_rx_free(void)
{
    return (uint32_t)(UART_MASK - ring_used(rx_head, rx_tail));
}

bool db_uart_rx_ring_nonempty(void)
{
    return rx_head != rx_tail;
}

void db_uart_set_loopback(bool on)
{
    if (cpu_transport) return;
    loopback = on;
    ESP_LOGI(TAG, "loopback %s", on ? "ON" : "OFF");
}

/* --- Interrupt evaluation (CPU task only) --- */

void db_uart_update_irq(void)
{
    bool en   = db.ustcnt & USTCNT_UARTEN;
    bool rxen = db.ustcnt & USTCNT_RXEN;
    bool txen = db.ustcnt & USTCNT_TXEN;

    uint16_t rx_n = ring_used(rx_head, rx_tail);
    uint16_t tx_n = ring_used(tx_head, tx_tail);

    bool rx_irq = en && rxen && (
        ((db.ustcnt & USTCNT_RXRDYEN)  && rx_n > 0) ||
        ((db.ustcnt & USTCNT_RXHALFEN) && rx_n >= UART_RING / 2) ||
        ((db.ustcnt & USTCNT_RXFULLEN) && rx_n >= UART_MASK));

    bool tx_irq = en && txen && (
        ((db.ustcnt & USTCNT_TXEMPTYEN) && tx_n == 0) ||
        ((db.ustcnt & USTCNT_TXHALFEN)  && tx_n <  UART_RING / 2) ||
        ((db.ustcnt & USTCNT_TXAVAILEN) && tx_n <  UART_MASK));

    if (rx_irq || tx_irq)
        db_interrupt_raise(INT_UART);
    else
        db_interrupt_clear(INT_UART);
}

/* --- Register access (called by the bus, i.e. on the CPU task) --- */

static uint16_t urx_status_flags(uint16_t rx_n)
{
    uint16_t v = 0;
    if (rx_n > 0)              v |= URX_DATARDY;
    if (rx_n >= UART_RING / 2) v |= URX_FIFOHALF;
    if (rx_n >= UART_MASK)     v |= URX_FIFOFULL;
    if (rx_overrun)            v |= URX_OVRUN;
    return v;
}

uint16_t db_uart_read16(uint16_t offset)
{
    switch (offset) {
    case REG_USTCNT: return db.ustcnt;
    case REG_UBAUD:  return db.ubaud;

    case REG_URX: {
        if (rx_head == rx_tail)
            return 0;                       /* empty: no data-ready */
        uint8_t b = rx_buf[rx_tail];
        __sync_synchronize();
        rx_tail = (uint16_t)((rx_tail + 1) & UART_MASK);

        uint16_t v = URX_DATARDY | b;
        uint16_t rx_n = ring_used(rx_head, rx_tail);
        if (rx_n >= UART_RING / 2) v |= URX_FIFOHALF;
        if (rx_n >= UART_MASK)     v |= URX_FIFOFULL;
        if (rx_overrun) { v |= URX_OVRUN; rx_overrun = false; }
        db_uart_update_irq();               /* read drained a byte */
        return v;
    }

    case REG_UTX: {
        /* Status read (the ROM polls this before writing a byte). We "transmit"
         * instantly into the TX ring, so the FIFO is normally empty/available.
         * Report CTS asserted for a simple 3-wire link and preserve writable
         * control bits (SEND_BREAK, IGNORE_CTS). */
        uint16_t tx_n = ring_used(tx_head, tx_tail);
        uint16_t v = (db.utx & (UTX_SENDBREAK | UTX_IGNORECTS)) | UTX_CTSSTATUS;
        if (tx_n == 0)             v |= UTX_FIFOEMPTY;
        if (tx_n <  UART_RING / 2) v |= UTX_FIFOHALF;
        if (tx_n <  UART_MASK)     v |= UTX_AVAIL;
        return v;
    }

    case REG_UMISC: return db.umisc;
    default: return 0;
    }
}

uint8_t db_uart_read8(uint16_t offset)
{
    /* Reading the high byte of URX ($904, status flags, e.g. via BTST) must NOT
     * pop a byte from the RX FIFO; only reading $905 (RX_DATA) or a 16-bit read
     * of $904 consumes the queued byte. */
    if (offset == REG_URX)
        return (uint8_t)(urx_status_flags(ring_used(rx_head, rx_tail)) >> 8);
    if (offset == REG_URX + 1)
        return (uint8_t)(db_uart_read16(REG_URX) & 0xFF);

    uint16_t w = db_uart_read16((uint16_t)(offset & ~1u));
    return (offset & 1) ? (uint8_t)(w & 0xFF) : (uint8_t)(w >> 8);
}

void db_uart_write16(uint16_t offset, uint16_t val)
{
    switch (offset) {
    case REG_USTCNT:
        db.ustcnt = val;
        db_uart_update_irq();               /* enable bits changed */
        break;

    case REG_UBAUD:
        if (val != db.ubaud) {
            unsigned div   = (val & UBAUD_DIVIDER) >> 8;
            unsigned presc = (val & UBAUD_PRESCALER);
            ESP_LOGI(TAG, "UBAUD=%04X (divider=%u prescaler=%u)", val, div, presc);
        }
        db.ubaud = val;
        break;

    case REG_UTX:
        db.utx = val;
        if (db.ustcnt & USTCNT_TXEN) {       /* a byte to transmit */
            uint8_t b = (uint8_t)(val & UTX_DATA);
            uint16_t nh = (uint16_t)((tx_head + 1) & UART_MASK);
            if (nh != tx_tail) {             /* drop if ring full (back-pressure) */
                tx_buf[tx_head] = b;
                __sync_synchronize();
                tx_head = nh;
            }
            if (loopback)                    /* echo straight back into RX */
                db_uart_rx_push(&b, 1);
            db_uart_update_irq();
        }
        break;

    case REG_UMISC:
        db.umisc = val;
        /* Note: we deliberately do NOT drive `loopback` from the UMISC LOOP bit.
         * Loopback (internal TX->RX) and serial pass-through (TX->wire, wire->RX)
         * are mutually exclusive, and having the ROM enable loopback would add a
         * second producer to the RX ring while the bridge's console reader is the
         * other — corrupting the lock-free SPSC ring. `loopback` is owned solely by
         * the `serloop` debug command; the bridge forces it off while active. */
        break;
    }
}

void db_uart_write8(uint16_t offset, uint8_t val)
{
    /* Writing the high byte of UTX ($906, e.g. BSET/BCLR on IGNORE_CTS or
     * SEND_BREAK) updates control bits only and must NOT enqueue a byte into
     * the TX FIFO; writing the low byte ($907, TX_DATA) transmits the byte. */
    if (offset == REG_UTX) {
        db.utx = ((uint16_t)val << 8) | (db.utx & 0x00FF);
        return;
    }
    if (offset == REG_UTX + 1) {
        db_uart_write16(REG_UTX, (db.utx & 0xFF00) | val);
        return;
    }

    uint16_t base = (uint16_t)(offset & ~1u);
    uint16_t cur  = db_uart_read16(base);
    uint16_t next = (offset & 1)
                        ? ((cur & 0xFF00) | val)
                        : (((uint16_t)val << 8) | (cur & 0x00FF));
    db_uart_write16(base, next);
}
