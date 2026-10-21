#include "dragonball.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

dragonball_state_t db;
uint8_t  db_pwm_read8(uint16_t offset);
uint16_t db_pwm_read16(uint16_t offset);
void     db_pwm_write8(uint16_t offset, uint8_t val);
void     db_pwm_write16(uint16_t offset, uint16_t val);

static float last_freq = -1, last_duty = -1;
static unsigned calls;
static void sink(float freq, float duty) { last_freq = freq; last_duty = duty; calls++; }

int main(void)
{
    db_pwm_set_sink(sink);
    db_pwm_reset();
    assert(calls == 1 && last_freq == 0);
    assert(db_pwm_read16(REG_PWMC) == 0x0020 && db_pwm_read8(REG_PWMP) == 0xFE);

    /* SYSCLK / (prescaler 3+1) / (2 << clksel 1) / (period 98+2) ~= 10.36 kHz,
     * nothing sounds until the sample is written. */
    db_pwm_write8(REG_PWMP, 98);
    db_pwm_write16(REG_PWMC, 0x0300 | 0x0010 | 0x0001);
    assert(last_freq == 0);
    db_pwm_write16(REG_PWMS, 49);
    assert(fabsf(last_freq - 16580608.0f / 4 / 4 / 100) < 1.0f);
    assert(fabsf(last_duty - 0.5f) < 0.001f);

    /* Byte writes: retune while active, then the low sample byte. */
    db_pwm_write8(REG_PWMC, 0x07);                  /* prescaler 8 */
    assert(fabsf(last_freq - 16580608.0f / 8 / 4 / 100) < 1.0f);
    db_pwm_write8(REG_PWMS + 1, 25);
    assert(fabsf(last_duty - 25.0f / 98) < 0.001f);

    /* Clearing PWMEN silences; a sample write while disabled stays silent. */
    db_pwm_write16(REG_PWMC, 0x0701);
    assert(last_freq == 0);
    unsigned n = calls;
    db_pwm_write16(REG_PWMS, 49);
    assert(calls == n && !db.pwm_active);

    /* CLK32 source, ultrasonic tones are reported as silence. */
    db_pwm_write8(REG_PWMP, 14);
    db_pwm_write16(REG_PWMC, 0x8000 | 0x0010);
    db_pwm_write16(REG_PWMS, 8);
    assert(fabsf(last_freq - 32768.0f / 2 / 16) < 0.1f);
    db_pwm_write16(REG_PWMC, 0x0010);
    assert(last_freq == 0 && db.pwm_active);

    puts("pwm: ok");
    return 0;
}
