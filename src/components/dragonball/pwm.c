#include "dragonball.h"

/* MC68EZ328 PWM, used by Palm OS as the piezo tone generator.
 *
 * Register map (EZ, not the original 68328 layout):
 *   $500 PWMC   16-bit  [15] CLKSRC (1 = CLK32) [14:8] PRESCALER [4] PWMEN [1:0] CLKSEL
 *   $502 PWMS   16-bit  sample FIFO; the ROM's tone driver uses the low byte
 *   $504 PWMP   8-bit   period
 *   $505 PWMCNT 8-bit   counter (read-only)
 *
 * The sound manager programs period/prescaler, sets PWMEN, then writes the
 * sample (pulse width). The output is a rectangle wave of
 *   clock / (PRESCALER+1) / (2 << CLKSEL) / (PWMP+2)
 * with duty PWMS/PWMP. Clearing PWMEN silences it. The FIFO and its interrupt
 * are not modeled: the tone is held until PWMEN drops, which is how the ROM
 * stops a SndDoCmd beep. Tone changes go to a sink installed by the firmware. */

#define PWMC_CLKSRC_CLK32 0x8000
#define PWMC_EN           0x0010
#define SYSCLK_HZ         16580608u   /* matches the PLL the timer path assumes */

static db_pwm_sink_t pwm_sink;

void db_pwm_set_sink(db_pwm_sink_t sink)
{
    pwm_sink = sink;
}

static void pwm_dispatch(void)
{
    if (!pwm_sink) return;
    if (!db.pwm_active) { pwm_sink(0.0f, 0.0f); return; }

    uint32_t base = (db.pwmc & PWMC_CLKSRC_CLK32) ? 32768u : SYSCLK_HZ;
    uint32_t prescaler = ((db.pwmc >> 8) & 0x7F) + 1;
    uint32_t clksel = 2u << (db.pwmc & 0x03);
    uint32_t period = (uint32_t)db.pwmp + 2;
    if (period > 256) period = 256;
    float freq = (float)base / (float)(prescaler * clksel * period);
    float duty = db.pwmp ? (float)(db.pwms & 0xFF) / (float)db.pwmp : 0.0f;
    if (duty > 1.0f) duty = 1.0f;
    if (freq >= 20000.0f) freq = 0.0f;   /* ultrasonic: nothing to play */
    pwm_sink(freq, duty);
}

void db_pwm_reset(void)
{
    /* dragonball_init has already zeroed db; apply EZ reset values. */
    db.pwmc = 0x0020;
    db.pwmp = 0xFE;
    db.pwm_active = false;
    if (pwm_sink) pwm_sink(0.0f, 0.0f);
}

static void pwm_control_written(void)
{
    if (db.pwm_active && !(db.pwmc & PWMC_EN)) db.pwm_active = false;
    pwm_dispatch();
}

static void pwm_sample_written(void)
{
    if (db.pwmc & PWMC_EN) {
        db.pwm_active = true;
        pwm_dispatch();
    }
}

uint8_t db_pwm_read8(uint16_t offset)
{
    switch (offset) {
    case REG_PWMC:     return (uint8_t)(db.pwmc >> 8);
    case REG_PWMC + 1: return (uint8_t)db.pwmc;
    case REG_PWMS:     return (uint8_t)(db.pwms >> 8);
    case REG_PWMS + 1: return (uint8_t)db.pwms;
    case REG_PWMP:     return db.pwmp;
    default:           return 0;   /* PWMCNT */
    }
}

uint16_t db_pwm_read16(uint16_t offset)
{
    return (uint16_t)(db_pwm_read8(offset) << 8) | db_pwm_read8(offset + 1);
}

void db_pwm_write8(uint16_t offset, uint8_t val)
{
    switch (offset) {
    case REG_PWMC:
        db.pwmc = (uint16_t)((db.pwmc & 0x00FF) | (val << 8));
        if (db.pwm_active) pwm_dispatch();
        break;
    case REG_PWMC + 1:
        db.pwmc = (uint16_t)((db.pwmc & 0xFF00) | val);
        pwm_control_written();
        break;
    case REG_PWMS:
        db.pwms = (uint16_t)((db.pwms & 0x00FF) | (val << 8));
        break;
    case REG_PWMS + 1:
        db.pwms = (uint16_t)((db.pwms & 0xFF00) | val);
        pwm_sample_written();
        break;
    case REG_PWMP:
        db.pwmp = val;
        if (db.pwm_active) pwm_dispatch();
        break;
    default:
        break;      /* PWMCNT is read-only */
    }
}

void db_pwm_write16(uint16_t offset, uint16_t val)
{
    switch (offset) {
    case REG_PWMC:
        db.pwmc = val;
        pwm_control_written();
        break;
    case REG_PWMS:
        db.pwms = val;
        pwm_sample_written();
        break;
    case REG_PWMP:
        db_pwm_write8(REG_PWMP, (uint8_t)(val >> 8));
        break;
    default:
        break;
    }
}
