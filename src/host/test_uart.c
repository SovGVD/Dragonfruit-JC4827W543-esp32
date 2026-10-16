#include "dragonball.h"
#include <assert.h>
#include <stdio.h>

dragonball_state_t db;
static bool irq;
void db_interrupt_raise(int source) { assert(source == INT_UART); irq = true; }
void db_interrupt_clear(int source) { assert(source == INT_UART); irq = false; }

int main(void)
{
    db_uart_set_cpu_transport(true);
    db_uart_reset();
    uint32_t gen = db_uart_generation();
    db.ustcnt = USTCNT_UARTEN | USTCNT_RXEN | USTCNT_TXEN | USTCNT_RXRDYEN;
    uint8_t data[300];
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
    assert(db_uart_rx_free() == 255);
    db_uart_rx_push(data, db_uart_rx_free());
    assert(db_uart_rx_free() == 0);
    db_uart_update_irq();
    assert(irq);
    for (unsigned i = 0; i < 255; i++) {
        uint16_t v = db_uart_read16(REG_URX);
        assert(v & URX_DATARDY);
        assert(!(v & URX_OVRUN));
        assert((uint8_t)v == (uint8_t)i);
    }
    assert(!irq && db_uart_rx_free() == 255);
    for (unsigned i = 0; i < 255; i++) {
        assert(db_uart_read16(REG_UTX) & UTX_AVAIL);
        db_uart_write16(REG_UTX, (uint16_t)i);
    }
    assert(!(db_uart_read16(REG_UTX) & UTX_AVAIL));
    assert(db_uart_tx_pop(data, 128) == 128);
    for (unsigned i = 0; i < 128; i++) assert(data[i] == (uint8_t)i);
    assert(db_uart_read16(REG_UTX) & UTX_AVAIL);
    assert(db_uart_tx_pop(data, sizeof(data)) == 127);
    for (unsigned i = 0; i < 127; i++) assert(data[i] == (uint8_t)(i + 128));
    db.ustcnt |= USTCNT_TXAVAILEN;
    db_uart_update_irq();
    assert(irq);
    db_uart_rx_push(data, 40);
    db_uart_write16(REG_UTX, 0x7e);
    db_uart_reset();
    assert(db_uart_generation() != gen);
    assert(!db_uart_rx_ring_nonempty());
    assert(db_uart_tx_pop(data, sizeof(data)) == 0);
    assert(db_uart_rx_free() == 255);
    db_uart_set_loopback(true); /* Cannot add a second producer to PPP's ring. */
    db_uart_write16(REG_UTX, 0x7e);
    assert(!db_uart_rx_ring_nonempty());

    /* 8-bit UART register access tests */
    db_uart_reset();
    db_uart_set_cpu_transport(true);
    db.ustcnt = USTCNT_UARTEN | USTCNT_RXEN | USTCNT_TXEN;
    uint8_t one = 0x42;
    db_uart_rx_push(&one, 1);
    /* High byte read of URX ($904) reports DATARDY status without popping the byte */
    uint8_t st = db_uart_read8(REG_URX);
    assert((st & (URX_DATARDY >> 8)) != 0);
    assert(db_uart_rx_ring_nonempty());
    /* Low byte read of URX ($905) pops the byte */
    uint8_t byte_read = db_uart_read8(REG_URX + 1);
    assert(byte_read == 0x42);
    assert(!db_uart_rx_ring_nonempty());

    /* 8-bit write to high byte of UTX ($906) sets control flags without transmitting */
    db_uart_write8(REG_UTX, (uint8_t)(UTX_IGNORECTS >> 8));
    assert((db_uart_read16(REG_UTX) & UTX_IGNORECTS) != 0);
    assert(db_uart_tx_pop(data, sizeof(data)) == 0);

    /* 8-bit write to low byte of UTX ($907) transmits a byte */
    db_uart_write8(REG_UTX + 1, 0x55);
    assert(db_uart_tx_pop(data, sizeof(data)) == 1 && data[0] == 0x55);

    puts("UART PPP buffering, 8-bit access and reset tests passed");
    return 0;
}
