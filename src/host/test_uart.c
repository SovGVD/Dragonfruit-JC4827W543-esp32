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
    puts("UART PPP buffering and reset tests passed");
    return 0;
}
