/* digitizer.cfg file format + GT911 config-block mapping. Pure C (no ESP/I2C
 * deps) so the host build can unit-test it; input.c does the actual I2C I/O. See
 * touch_config.h for the block geometry and the exposed-knob rationale. */
#include "touch_config.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Single source of truth for the exposed knobs: file key, where it lives in the
 * gt911_sens_t struct, its byte offset in the GT911 config block, and the doc
 * string serialize() emits as a trailing comment. */
static const struct {
    const char *key;
    size_t      field_off;   /* offsetof() into gt911_sens_t */
    int         block_off;   /* byte offset within the 186-byte config block */
    const char *doc;
} FIELDS[] = {
    { "shake_count",     offsetof(gt911_sens_t, shake_count),     8,  "samples to confirm a touch (debounce)" },
    { "filter",          offsetof(gt911_sens_t, filter),          9,  "coordinate smoothing" },
    { "large_touch",     offsetof(gt911_sens_t, large_touch),     10, "large-area touch count" },
    { "noise_reduction", offsetof(gt911_sens_t, noise_reduction), 11, "noise floor" },
    { "touch_level",     offsetof(gt911_sens_t, touch_level),     12, "press threshold; lower = more sensitive" },
    { "leave_level",     offsetof(gt911_sens_t, leave_level),     13, "release threshold; must be < touch_level" },
};
#define NFIELDS ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))

static uint8_t *field_ptr(gt911_sens_t *s, size_t off) { return (uint8_t *)s + off; }

void gt911_sens_from_block(const uint8_t *block, gt911_sens_t *out)
{
    for (int i = 0; i < NFIELDS; i++)
        *field_ptr(out, FIELDS[i].field_off) = block[FIELDS[i].block_off];
}

void gt911_sens_to_block(const gt911_sens_t *s, uint8_t *block)
{
    for (int i = 0; i < NFIELDS; i++)
        block[FIELDS[i].block_off] = *field_ptr((gt911_sens_t *)s, FIELDS[i].field_off);
}

uint8_t gt911_cfg_checksum(const uint8_t *block)
{
    uint8_t sum = 0;
    for (int i = 0; i < GT911_CFG_CKSUM; i++)
        sum += block[i];
    return (uint8_t)(~sum + 1);          /* two's-complement negation (per GT911) */
}

int gt911_sens_serialize(const gt911_sens_t *s, char *buf, size_t cap)
{
    static const char *header =
        "# M5Paper GT911 digitizer sensitivity\n"
        "# Auto-created from the panel's current values. Edit a value and reboot\n"
        "# to apply it; delete this file and reboot to restore the factory config\n"
        "# (the file is then regenerated). All values are single bytes (0-255).\n"
        "# Lower touch_level = more sensitive.\n"
        "\n";

    int n = snprintf(buf, cap, "%s", header);
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t len = (size_t)n;

    for (int i = 0; i < NFIELDS; i++) {
        uint8_t v = *field_ptr((gt911_sens_t *)s, FIELDS[i].field_off);
        n = snprintf(buf + len, cap - len, "%-15s = %-3u  # %s\n",
                     FIELDS[i].key, v, FIELDS[i].doc);
        if (n < 0 || (size_t)n >= cap - len) return -1;
        len += (size_t)n;
    }
    return (int)len;
}

/* --- tiny INI helpers (mirrors layout.c's idiom; kept local for independence) --- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

/* Strict integer: true only for a clean number with no trailing garbage. */
static bool parse_int(const char *v, int *out)
{
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0') return false;
    *out = (int)n;
    return true;
}

static void report(gt911_sens_log_fn log, void *ctx, const char *fmt, ...)
{
    if (!log) return;
    char msg[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    log(ctx, msg);
}

int gt911_sens_parse(const char *text, gt911_sens_t *s,
                     gt911_sens_log_fn log, void *log_ctx)
{
    int applied = 0;
    const uint8_t orig_touch = s->touch_level;
    const uint8_t orig_leave = s->leave_level;

    /* Walk the text line by line into a small local buffer (no writable copy of
     * the whole file, no large stack frame), stripping comments and whitespace. */
    for (const char *p = text; *p; ) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char line[96];
        size_t n = (size_t)(eol - p);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = (*eol) ? eol + 1 : eol;

        char *hash = strpbrk(line, "#;");
        if (hash) *hash = '\0';
        char *t = trim(line);
        if (!*t) continue;

        char *eq = strchr(t, '=');
        if (!eq) { report(log, log_ctx, "ignored (no '='): %s", t); continue; }
        *eq = '\0';
        char *key = trim(t);
        char *val = trim(eq + 1);

        int fi = -1;
        for (int i = 0; i < NFIELDS; i++)
            if (strcmp(key, FIELDS[i].key) == 0) { fi = i; break; }
        if (fi < 0) { report(log, log_ctx, "skipped unknown key '%s'", key); continue; }

        int iv;
        if (!parse_int(val, &iv) || iv < 0 || iv > 255) {
            report(log, log_ctx, "skipped %s='%s' (want 0-255)", key, val);
            continue;
        }

        uint8_t *fp = field_ptr(s, FIELDS[fi].field_off);
        if (*fp != (uint8_t)iv)
            report(log, log_ctx, "applied %s %u->%u", key, *fp, (unsigned)iv);
        *fp = (uint8_t)iv;
        applied++;
    }

    /* Hysteresis guard: the GT911 needs touch_level (press) above leave_level
     * (release). If a hand edit inverted them, revert whichever value changed
     * this pass back to the chip's known-good current value. */
    if (s->touch_level <= s->leave_level) {
        if (s->touch_level != orig_touch) {
            report(log, log_ctx, "touch_level %u <= leave_level %u; reverted to %u",
                   s->touch_level, s->leave_level, orig_touch);
            s->touch_level = orig_touch;
            applied--;
        }
        if (s->leave_level != orig_leave) {
            report(log, log_ctx, "leave_level %u >= touch_level %u; reverted to %u",
                   s->leave_level, s->touch_level, orig_leave);
            s->leave_level = orig_leave;
            applied--;
        }
    }
    return applied < 0 ? 0 : applied;
}
