/* The radio co-processor: an AT90S2313 AVR between SCI2 and the RF chip.
 *
 * Worked out from CyOS 1.5.08 (its RF driver object "rfdriver" sits at
 * 49b900-49c760 in RAM once the kernel runs). SCI2 runs at 36 kbaud, 8N1;
 * CyOS feeds it with the DTC. From the H8S to the AVR:
 *
 *   01 02 ch          tune to channel ch (CyOS uses 2)
 *   01 03 xx xx       a setting whose meaning is unknown (CyOS sends 00 00)
 *   30 f <51 bytes>   send a short packet: 50 bytes plus one spare byte the
 *                     DTC count includes; f is 0 or 1 (meaning unknown)
 *   cf f <201 bytes>  send a long packet: 200 bytes plus the spare byte
 *
 * From the AVR to the H8S:
 *
 *   03                the packet went out (CyOS waits up to 50 ms for a
 *                     short packet, 100 ms for a long one, from the start)
 *   32 <50 bytes>     a short packet was heard
 *   c8 <200 bytes>    a long packet was heard
 *   13 / 11           XOFF / XON while CyOS is sending (not used here)
 *
 * The status LED. The Xtreme's manual says it blinks
 * red while sending and green while receiving. Which pins drive it is not
 * known (CyOS changes no pin while it sends; it raises P17 while a heard
 * packet comes in, maybe the green LED, maybe a line to the AVR), so the
 * LED follows the packets here: a packet lights it from its first byte until
 * CY_LED_BLINK_MS after its last.
 *
 * Packets start with the 4-byte destination (ffffffff for everyone) and the
 * 4-byte sender ID; the rest (CyOS's own network protocol) is passed through
 * untouched, so emulating the AVR is enough for CyOS to talk to other
 * machines. The host carries packets between machines (ESP-NOW on the
 * board) with cybiko_rf_receive() and the rf_send hook.
 *
 * Each Cybiko's ID is stored in its flash at 7f818, in a record at 7f800
 * (magic ba0bab15) that a CRC-32 at 7fffc covers; the boot ROM and the
 * kernel both check it. cybiko_set_unit_id() writes a new ID and CRC, so
 * every board can have its own ID from one flash dump. */
#include <string.h>
#include "cy_internal.h"

#define REC_OFFSET 0x7f800u
#define ID_OFFSET 0x7f818u
#define CRC_OFFSET 0x7fffcu
#define SPARE 1                       /* extra byte at the end of each TX frame */
#define ACK_DELAY (CY_CLOCK_HZ / 500) /* 2 ms from the last byte to the 03 */
/* A heard packet reaches the H8S at the speed it comes over the air, not
 * at the SCI's: CyOS reads the signal strength 15 ms after the first byte
 * and expects the last one within 50 ms (short) or 115 ms (long). */
#define AIR_BYTE (CY_CLOCK_HZ / 2400)

enum { P_IDLE, P_CMD, P_ARGS, P_DATA };

static uint32_t now(const cybiko_t *m) { return (uint32_t)m->synced; }

static bool sci2_listening(const cybiko_t *m)
{
    /* SCI2 module running (MSTPCRL bit 7 clear) with the receiver on */
    return !(m->mstpcr[1] & 0x80) && (m->sci[2].scr & 0x10);
}

void cy_rf_reset(cybiko_t *m)
{
    cy_rf_t *r = &m->rf;
    r->parse = P_IDLE;
    r->have = r->need = 0;
    r->ack_due = false;
    r->inject_len = r->inject_pos = 0;
    r->led_red = r->led_green = 0;
}

static void frame_done(cybiko_t *m)
{
    cy_rf_t *r = &m->rf;
    if (r->cmd == 0x01) {
        if (r->buf[0] == 0x02 && r->have >= 2) r->channel = r->buf[1];
    } else {
        uint16_t len = r->cmd == 0x30 ? CY_RF_SHORT : CY_RF_LONG;
        r->sent++;
        r->led_red = m->synced;
        /* buf[0] is the flag byte; the packet follows */
        if (m->host.rf_send) m->host.rf_send(m->host.user, r->buf + 1, len, r->channel);
        r->ack_due = true;
        r->ack_at = now(m) + ACK_DELAY;
    }
    r->parse = P_IDLE;
}

/* A byte the H8S finished sending on SCI2. */
void cy_rf_h8_byte(cybiko_t *m, uint8_t b)
{
    cy_rf_t *r = &m->rf;
    switch (r->parse) {
    case P_IDLE:
        r->cmd = b;
        r->have = 0;
        if (b == 0x01) {
            r->parse = P_CMD;
        } else if (b == 0x30 || b == 0xcf) {
            r->need = 1 + (b == 0x30 ? CY_RF_SHORT : CY_RF_LONG) + SPARE;
            r->parse = P_DATA;
            r->led_red = m->synced;
            /* A real AVR cannot hear while it sends: drop a packet still
             * being handed to the H8S. */
            r->inject_len = r->inject_pos = 0;
        }
        break;
    case P_CMD:
        r->buf[r->have++] = b;
        r->need = b == 0x02 ? 2 : b == 0x03 ? 3 : 1;
        r->parse = r->have < r->need ? P_ARGS : P_IDLE;
        if (r->parse == P_IDLE) frame_done(m);
        break;
    case P_ARGS:
    case P_DATA:
        if (r->have < sizeof r->buf) r->buf[r->have] = b;
        if (++r->have >= r->need) frame_done(m);
        break;
    }
}

bool cybiko_rf_receive(cybiko_t *m, const uint8_t *pkt, size_t len, uint8_t channel)
{
    cy_rf_t *r = &m->rf;
    if ((len != CY_RF_SHORT && len != CY_RF_LONG) || r->queued >= CY_RF_QUEUE) return false;
    int slot = (r->q_head + r->queued) % CY_RF_QUEUE;
    memcpy(r->q[slot], pkt, len);
    r->q_len[slot] = (uint8_t)len;
    r->q_channel[slot] = channel;
    r->queued++;
    return true;
}

/* A/D channel 0 is the radio's signal strength. CyOS samples it while a
 * packet comes in and picks the gate it hears best, so a packet coming in
 * shows a good signal and the quiet air a low one. */
uint16_t cy_rf_rssi(const cybiko_t *m)
{
    return m->rf.inject_len ? 450 : 30;
}

/* Hand packets to the H8S when it is listening and not busy sending, and
 * acknowledge sent ones. */
void cy_rf_advance(cybiko_t *m)
{
    cy_rf_t *r = &m->rf;
    if (r->ack_due && (int32_t)(now(m) - r->ack_at) >= 0) {
        r->ack_due = false;
        cybiko_serial_rx(m, 2, 0x03);
    }
    if (!r->inject_len && r->queued && r->parse == P_IDLE && !r->ack_due &&
        m->sci[2].rx_count == 0 && sci2_listening(m)) {
        int slot = r->q_head;
        r->q_head = (r->q_head + 1) % CY_RF_QUEUE;
        r->queued--;
        if (r->q_channel[slot] == r->channel) {
            uint16_t len = r->q_len[slot];
            r->inject[0] = len == CY_RF_SHORT ? 0x32 : 0xc8;
            memcpy(r->inject + 1, r->q[slot], len);
            r->inject_len = (uint16_t)(len + 1);
            r->inject_pos = 0;
            r->inject_at = now(m);
            r->heard++;
        }
    }
    while (r->inject_pos < r->inject_len && (int32_t)(now(m) - r->inject_at) >= 0 &&
           cybiko_serial_rx(m, 2, r->inject[r->inject_pos])) {
        r->inject_pos++;
        r->inject_at += AIR_BYTE;
        r->led_green = m->synced;
    }
    if (r->inject_pos >= r->inject_len) r->inject_len = r->inject_pos = 0;
}

int cybiko_led(const cybiko_t *m)
{
    const cy_rf_t *r = &m->rf;
    uint64_t t = m->synced, hold = (uint64_t)CY_CLOCK_HZ * CY_LED_BLINK_MS / 1000;
    bool red = r->parse == P_DATA || (r->led_red && t - r->led_red < hold);
    bool green = r->inject_len || (r->led_green && t - r->led_green < hold);
    if (red && green) return r->led_red >= r->led_green ? CY_LED_RED : CY_LED_GREEN;
    return red ? CY_LED_RED : green ? CY_LED_GREEN : CY_LED_OFF;
}

uint32_t cy_rf_next(const cybiko_t *m)
{
    const cy_rf_t *r = &m->rf;
    if (r->ack_due) {
        int32_t d = (int32_t)(r->ack_at - now(m));
        return d > 0 ? (uint32_t)d : 1;
    }
    if (r->inject_len) {
        int32_t d = (int32_t)(r->inject_at - now(m));
        return d > 0 ? (uint32_t)d : 1;
    }
    /* Look again every millisecond while a packet waits for its turn. */
    if (r->queued) return CY_CLOCK_HZ / 1000;
    return 0xffffffffu;
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

void cybiko_set_unit_id(cybiko_t *m, uint32_t id)
{
    cy_rf_t *r = &m->rf;
    uint8_t *f = m->flash.mem;
    if (!id || id == cybiko_unit_id(m)) return;
    if (!r->id_applied) {
        memcpy(r->id_orig, f + ID_OFFSET, 4);
        memcpy(r->crc_orig, f + CRC_OFFSET, 4);
        r->id_applied = true;
    }
    put32(f + ID_OFFSET, id);
    put32(f + CRC_OFFSET, crc32(f + REC_OFFSET, CRC_OFFSET - REC_OFFSET));
}

uint32_t cybiko_unit_id(const cybiko_t *m)
{
    const uint8_t *p = m->flash.mem + ID_OFFSET;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void cybiko_flash_original(const cybiko_t *m, uint8_t *copy)
{
    if (!m->rf.id_applied) return;
    memcpy(copy + ID_OFFSET, m->rf.id_orig, 4);
    memcpy(copy + CRC_OFFSET, m->rf.crc_orig, 4);
}
