#pragma once
/* Shared between the Cybiko machine's source files. */
#include "cybiko.h"

/* Interrupt vectors of the H8S/2323 */
#define CY_VEC_IRQ0   16
#define CY_VEC_WOVI   25
#define CY_VEC_ADI    28
#define CY_VEC_TMR0   64     /* CMIA0, CMIB0, OVI0; TMR1 at 68 */
#define CY_VEC_SCI0   80     /* ERI, RXI, TXI, TEI; 4 per channel */

/* Raise an internal interrupt request. Requests are latched until the CPU
 * takes them, as in MAME's H8 interrupt controller. */
bool cy_dma_trigger(cybiko_t *m, int vec);
bool cy_dtc_trigger(cybiko_t *m, int vec);
static inline void cy_irq(cybiko_t *m, int vec)
{
    /* A DMA transfer started by this interrupt may swallow it (DTA), and so
     * may the DTC when its DTCER bit is set. */
    if (m->dma_waiting && cy_dma_trigger(m, vec)) return;
    if (vec >= 16 && cy_dtc_trigger(m, vec)) return;
    m->pend[vec >> 5] |= 1u << (vec & 31);
    m->cpu.irq_check = true;
}

/* cy_dma.c: DMA controller. cy_dma_control() acts on a DMABCR write. */
void cy_dma_reset(cybiko_t *m);
void cy_dma_control(cybiko_t *m);

/* Bring every peripheral up to the CPU's current time. */
void cy_sync(cybiko_t *m);
void cy_bus_timing(cybiko_t *m);

/* cy_hle.c: an opcode the H8S leaves undefined marks a native routine */
#define CY_HLE_OP 0x0001
void cy_hle_patch_rom(cybiko_t *m);
void cy_hle_patch_kernel(cybiko_t *m);
bool cy_hle(cybiko_t *m);
void cy_hle_probe(cybiko_t *m, uint32_t pc);
bool cy_irq_deliverable(cybiko_t *m);     /* cybiko.c: an interrupt would be taken now */

/* cy_disk.c */
void cy_disk_console(cybiko_t *m, uint8_t b);

/* cy_rf.c: the radio co-processor on SCI2 */
void cy_rf_reset(cybiko_t *m);
void cy_rf_h8_byte(cybiko_t *m, uint8_t b);
void cy_rf_advance(cybiko_t *m);
uint32_t cy_rf_next(const cybiko_t *m);
void cy_disk_write8(cybiko_t *m, uint32_t off, uint8_t v);

/* cy_cart.c: the MP3 player cartridge */
void cy_cart_reset(cybiko_t *m);
uint16_t cy_cart_adc(cybiko_t *m, int ch);
uint8_t cy_cart_p2_input(cybiko_t *m);
void cy_cart_p2(cybiko_t *m, uint8_t level, uint8_t ddr);
void cy_cart_p3(cybiko_t *m, uint8_t level, uint8_t ddr);
uint8_t cy_cart_spi(cybiko_t *m, int port, uint8_t out);
uint8_t cy_cart_irq_pins(const cybiko_t *m);
uint8_t cy_cart_p6_input(const cybiko_t *m);
void cy_irq_pins_changed(cybiko_t *m, uint8_t old_in);
uint8_t cy_irq_pins(const cybiko_t *m);

/* cy_periph.c: TPU, TMR, WDT, SCI and A/D converter */
void cy_periph_reset(cybiko_t *m);
/* Start the timers' lazy catch-up from m->synced (after a reset). */
void cy_timers_restart(cybiko_t *m);
/* Bring the timers up to `now`, e.g. before the clocks stop for standby. */
void cy_timers_sync(cybiko_t *m, uint64_t now);
void cy_periph_advance(cybiko_t *m, uint32_t states);
uint32_t cy_periph_next_event(const cybiko_t *m);
/* Register access for addresses the peripherals own; return false when the
 * address is not theirs. */
bool cy_periph_read(cybiko_t *m, uint32_t a, uint8_t *v);
bool cy_periph_write(cybiko_t *m, uint32_t a, uint8_t v);
bool cy_periph_write16(cybiko_t *m, uint32_t a, uint16_t v);
void cy_sci_kick(cybiko_t *m);
/* Tone the TPU drives on TIOCB1 (the speaker pin), 0 if it is not driving
 * a periodic wave there. */
float cy_tpu_speaker_tone(const cybiko_t *m, float *duty);

/* cy_devices.c: HD66421 LCD, PCF8593 RTC, SST39VF400A flash */
void cy_lcd_reset(cy_lcd_t *l);
uint8_t cy_lcd_read(cy_lcd_t *l, int data);
void cy_lcd_write(cy_lcd_t *l, int data, uint8_t v);

void cy_rtc_reset(cybiko_t *m);
void cy_rtc_pins(cybiko_t *m, bool scl, bool sda);
bool cy_rtc_sda(const cybiko_t *m);

void cy_flash_reset(cybiko_t *m);
uint16_t cy_flash_read16(cybiko_t *m, uint32_t off);
void cy_flash_write16(cybiko_t *m, uint32_t off, uint16_t v);
