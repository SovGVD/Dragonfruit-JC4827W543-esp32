#include "dragonball.h"
#include <time.h>
#include "esp_log.h"

static const char *TAG = "rtc";

/* Emulated time-of-day, driven by the HOST's real wall clock rather than by
 * emulated CPU cycles.  The DragonBall RTC ticks at a true 1 Hz off a 32 kHz
 * crystal, independent of CPU speed; tying our "1 second" to a fixed number of
 * emulated cycles (the old RTC_HZ_PERIOD) made the Palm clock run slow whenever
 * the ESP32 executes 68k cycles slower than real silicon.  We instead advance
 * one TOD second per real elapsed second (time()), so the clock keeps real
 * time at any emulation speed.  rtc_tod is seconds since midnight; rtc_day is
 * days since 1904 (the Palm epoch).  Both are seeded from the host clock and
 * are writable via HMSR/DAYR so Palm's "set time" still works. */
static uint32_t rtc_tod = 0;        /* seconds since midnight (0..86399) */
static uint32_t rtc_day = 0;        /* days since Jan 1 1904 */
static time_t   rtc_last_real = 0;
static int      rtc_init = 0;
static uint32_t rtc_check_accum = 0;
static int32_t  rtc_last_alarm_sec = -1;   /* time-of-day at the previous alarm check */

static void db_rtc_update_interrupt(void);

/* Shared HH:MM:SS field layout for the EZ328 RTCTIME (HMSR, $B00) and RTCALRM registers:
 * hours in bits [28:24] (mask 0x1F), minutes in [21:16] (0x3F), SECONDS in [5:0] (0x3F) —
 * NOT [15:8]. The ROM reads each field with exactly these masks (e.g. TimGetSeconds does
 * `val & 0x3F` for seconds), so seconds must sit in the low six bits; placing them at [15:8]
 * left HH:MM correct but made every seconds read return 0, hanging any code that waits for
 * the RTC second to tick over (a benchmark's 1-second calibration loop). Single-sourced here
 * so a future field correction can't land in one decoder and miss the others. */
#define RTC_HMS_HOUR_SHIFT   24
#define RTC_HMS_HOUR_MASK    0x1Fu
#define RTC_HMS_MIN_SHIFT    16
#define RTC_HMS_MIN_MASK     0x3Fu
#define RTC_HMS_SEC_SHIFT    0
#define RTC_HMS_SEC_MASK     0x3Fu

/* Decode an HMSR/RTCALRM register value to a seconds-of-day count. */
static inline uint32_t rtc_hms_to_seconds(uint32_t reg)
{
    uint32_t h = (reg >> RTC_HMS_HOUR_SHIFT) & RTC_HMS_HOUR_MASK;
    uint32_t m = (reg >> RTC_HMS_MIN_SHIFT)  & RTC_HMS_MIN_MASK;
    uint32_t s = (reg >> RTC_HMS_SEC_SHIFT)  & RTC_HMS_SEC_MASK;
    return h * 3600 + m * 60 + s;
}

/* Pack a seconds-of-day count into the HMSR/RTCALRM field layout (h/m/s are in range, so
 * no masking is needed before the shifts). */
static inline uint32_t rtc_seconds_to_hms(uint32_t tod)
{
    uint32_t h = tod / 3600;
    uint32_t m = (tod / 60) % 60;
    uint32_t s = tod % 60;
    return (h << RTC_HMS_HOUR_SHIFT) | (m << RTC_HMS_MIN_SHIFT) | (s << RTC_HMS_SEC_SHIFT);
}

static void rtc_seed(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    rtc_tod = (uint32_t)(t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
    int year = t->tm_year + 1900;
    rtc_day = (uint32_t)((year - 1904) * 365 + (year - 1904) / 4 + t->tm_yday);
    rtc_last_real = now;
    rtc_init = 1;
}

void db_rtc_advance(uint32_t cpu_cycles)
{
    if (!rtc_init)
        rtc_seed();

    /* Throttle the (cheap) time() poll: checking ~hundreds of times a second
     * is far more than enough to land each 1 Hz boundary. */
    rtc_check_accum += cpu_cycles;
    if (rtc_check_accum < 50000)
        return;
    rtc_check_accum = 0;

    time_t now = time(NULL);

    /* Advance one emulated second per real elapsed second.  Each RTC status
     * bit asserts only at its own true period (e.g. the 24Hr day-rollover bit
     * 0x08 fires only at the midnight wrap, never every second — asserting it
     * every second would wedge the OS's RTC handler ~1s after start).
     *
     * The periodic Second/Minute/Hour interrupts are suppressed while the device
     * is in PLL-off deep sleep. This ROM leaves the RTC UNMASKED in its sleep
     * IMR (0xFFFEF0AF), so a Minute interrupt would wake the CPU every 60s — an
     * inauthentic spurious wake. We therefore raise Sec/Minute/Hour only while
     * awake; time-of-day (rtc_tod) still advances either way, so the clock stays
     * correct. The 24Hr midnight rollover is left enabled — it's a legitimate,
     * ~daily wake. */
    bool awake = !db_is_asleep();
    while (rtc_last_real < now) {
        rtc_last_real++;
        rtc_tod++;
        if (rtc_tod >= 86400) {
            rtc_tod -= 86400;
            rtc_day++;
            if (db.rtcienr & 0x08) db.rtcisr |= 0x08;          /* 24Hr   */
        }
        if (awake) {
            if (db.rtcienr & 0x10)                       db.rtcisr |= 0x10;  /* Sec    */
            if ((db.rtcienr & 0x02) && rtc_tod % 60 == 0)   db.rtcisr |= 0x02;  /* Minute */
            if ((db.rtcienr & 0x20) && rtc_tod % 3600 == 0) db.rtcisr |= 0x20;  /* Hour   */
        }
    }

    /* RTC alarm: fire once when the time-of-day crosses the programmed alarm.
     * This is a LEGITIMATE wake source (scheduled alarms/appointments), so it
     * runs whether awake or asleep — rtc_last_alarm_sec advances every call,
     * including during sleep (db_rtc_advance runs from the STOP-loop tick), so a
     * future alarm fires the moment the clock reaches it and wakes the device.
     * RTCALRM layout (EZ328): hours bits 24-28, minutes 16-21, seconds 0-5 —
     * distinct from HMSR. Enable = RTCIENR 0x04, status = RTCISR 0x04; the ROM
     * clears the status and re-arms after it fires.
     *
     * Edge-detected (prev check < alarm <= now) so it triggers exactly once per
     * crossing, matching CloudpilotEmu's EmRegsEZ::CycleSlowly (which keys off
     * its lastRtcAlarmCheck the same way). This is deliberately the edge form,
     * NOT POSE's pure `alarm <= now` level compare: the edge form cannot re-fire
     * every second on a stale past-time alarm the ROM left armed. The only case
     * it would miss — an alarm enabled for a time already <= now at the instant
     * of arming — does not arise in practice, as Palm arms alarms for the
     * future. */
    if ((db.rtcienr & 0x04) && !(db.rtcisr & 0x04)) {
        int32_t alm = (int32_t)rtc_hms_to_seconds(db.alarm);
        int32_t now_s = (int32_t)rtc_tod;
        if (rtc_last_alarm_sec >= 0 && rtc_last_alarm_sec < alm && alm <= now_s)
            db.rtcisr |= 0x04;   /* Alarm */
    }
    rtc_last_alarm_sec = (int32_t)rtc_tod;

    db_rtc_update_interrupt();
}

/* The RTC pending bit reflects (status & enable). When the OS clears
 * RTCISR (write-1-to-clear) or changes RTCIENR, the pending interrupt
 * must de-assert in IPR — otherwise it re-fires forever. Mirrors
 * CloudPilot's EmRegsEZ::UpdateRTCInterrupts. */
static void db_rtc_update_interrupt(void)
{
    if (db.rtcisr & db.rtcienr)
        db_interrupt_raise(INT_RTC);
    else
        db_interrupt_clear(INT_RTC);
}

/* RTCTIME (HMSR, $B00). Derived live from the real-time-driven rtc_tod so reads always
 * reflect current time; the field layout is single-sourced via rtc_seconds_to_hms (above). */
static uint32_t get_hmsr(void)
{
    if (!rtc_init)
        rtc_seed();
    return rtc_seconds_to_hms(rtc_tod);
}

static uint16_t get_dayr(void)
{
    if (!rtc_init)
        rtc_seed();
    return (uint16_t)rtc_day;
}

/* Palm "set time/date" writes HMSR/DAYR; honour it by re-basing our TOD. */
static void set_hmsr(uint32_t v)
{
    if (!rtc_init) rtc_seed();
    rtc_tod = rtc_hms_to_seconds(v) % 86400;
}

static uint16_t db_rtc_read16_impl(uint16_t offset);

uint8_t db_rtc_read8(uint16_t offset)
{
    uint16_t w = db_rtc_read16_impl(offset & ~1u);
    /* Big-endian (68k): an even byte offset selects the high byte, odd the low byte.
     * The old code returned the low byte for every offset, so an even-offset read
     * returned the wrong field (latent — no current path byte-reads HMSR). */
    return (offset & 1u) ? (uint8_t)w : (uint8_t)(w >> 8);
}

uint16_t db_rtc_read16(uint16_t offset)
{
    return db_rtc_read16_impl(offset);
}

static uint16_t db_rtc_read16_impl(uint16_t offset)
{
    switch (offset) {
    case REG_HMSR:     return (uint16_t)(get_hmsr() >> 16);
    case REG_HMSR + 2: return (uint16_t)get_hmsr();
    case REG_RTCCTL:   return db.rtcctl;
    case REG_RTCISR:   return db.rtcisr;
    case REG_RTCIENR:  return db.rtcienr;
    case REG_DAYR:     return get_dayr();
    case REG_ALARM:    return (uint16_t)(db.alarm >> 16);
    case REG_ALARM + 2: return (uint16_t)db.alarm;
    default: return 0;
    }
}

uint32_t db_rtc_read32(uint16_t offset)
{
    switch (offset) {
    case REG_HMSR:  return get_hmsr();
    case REG_ALARM: return db.alarm;
    default: return ((uint32_t)db_rtc_read16(offset) << 16) |
                    db_rtc_read16(offset + 2);
    }
}

void db_rtc_write8(uint16_t offset, uint8_t val)
{
    (void)offset; (void)val;
}

void db_rtc_write16(uint16_t offset, uint16_t val)
{
    switch (offset) {
    case REG_RTCCTL:  db.rtcctl = val; break;
    case REG_RTCISR:  db.rtcisr &= ~val; db_rtc_update_interrupt(); break;
    case REG_RTCIENR:
        if (val != db.rtcienr)
            ESP_LOGD(TAG, "RTCIENR <- %04X (Sample=%04X Sec=%d Min=%d Alm=%d) RTCCTL=%04X",
                     val, val & 0xFF00, !!(val&0x10), !!(val&0x02), !!(val&0x04), db.rtcctl);
        db.rtcienr = val; db_rtc_update_interrupt(); break;
    case REG_HMSR:     set_hmsr((get_hmsr() & 0x0000FFFF) | ((uint32_t)val << 16)); break;
    case REG_HMSR + 2: set_hmsr((get_hmsr() & 0xFFFF0000) | val); break;
    case REG_DAYR:    rtc_day = val; break;
    case REG_ALARM:     db.alarm = ((uint32_t)val << 16) | (db.alarm & 0xFFFF); break;
    case REG_ALARM + 2: db.alarm = (db.alarm & 0xFFFF0000) | val; break;
    }
}

void db_rtc_write32(uint16_t offset, uint32_t val)
{
    switch (offset) {
    case REG_ALARM: db.alarm = val; break;
    case REG_HMSR:  set_hmsr(val); break;
    default:
        db_rtc_write16(offset, (uint16_t)(val >> 16));
        db_rtc_write16(offset + 2, (uint16_t)val);
    }
}
