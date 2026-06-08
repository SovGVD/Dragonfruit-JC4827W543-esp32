/* Standalone self-test for components/input/touch_config.c (the digitizer.cfg
 * file-format + GT911 config-block mapping). Pure C, no ESP deps.
 *
 * Compile (from src/), e.g.:
 *   gcc -I components/input/include host/test_touch_config.c \
 *       components/input/touch_config.c -o host/test_touch_config && \
 *       host/test_touch_config
 *
 * Verifies the checksum algorithm against the real M5Paper factory config, the
 * block<->struct round-trip, serialize/parse round-trip, lenient skipping of
 * bad/unknown keys, and the touch>leave hysteresis guard. */
#include "touch_config.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

/* The M5Paper's stock GT911 config block (0x8047..0x8100), as shipped by the
 * board's factory touch firmware. Byte 184 (0xCE) is the factory checksum and
 * byte 185 (0x01) the config-fresh flag — so this doubles as a known-answer
 * vector for gt911_cfg_checksum(). */
static const uint8_t FACTORY[GT911_CFG_LEN] = {
    0x43, 0x1C, 0x02, 0xC0, 0x03, 0x02, 0x05, 0x00, 0x01, 0x18, 0x28, 0x0F, 0x50, 0x32,
    0x03, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x1A, 0x1E, 0x14, 0x87,
    0x29, 0x0A, 0x21, 0x23, 0xB2, 0x04, 0x00, 0x00, 0x00, 0x1A, 0x02, 0x1C, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x64, 0x32, 0x00, 0x00, 0x00, 0x19, 0x41, 0x94, 0xC5, 0x02,
    0x07, 0x00, 0x00, 0x04, 0x98, 0x1B, 0x00, 0x7F, 0x21, 0x00, 0x6A, 0x28, 0x00, 0x58,
    0x31, 0x00, 0x4A, 0x3B, 0x00, 0x4A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16, 0x18, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x28, 0x26, 0x24, 0x22, 0x21, 0x20, 0x1F, 0x1E, 0x1D, 0x00, 0x02, 0x04,
    0x06, 0x08, 0x0A, 0x0C, 0x0F, 0x10, 0x12, 0x13, 0x14, 0x16, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xCE, 0x01
};

static int sens_eq(const gt911_sens_t *a, const gt911_sens_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

int main(void)
{
    /* --- checksum known-answer (the factory block's own byte 184) --- */
    CHECK(gt911_cfg_checksum(FACTORY) == 0xCE,
          "checksum %02X != factory 0xCE", gt911_cfg_checksum(FACTORY));

    /* --- from_block extracts the documented offsets --- */
    gt911_sens_t cur;
    gt911_sens_from_block(FACTORY, &cur);
    CHECK(cur.shake_count == 1,     "shake_count %u != 1", cur.shake_count);
    CHECK(cur.filter == 24,         "filter %u != 24", cur.filter);
    CHECK(cur.large_touch == 40,    "large_touch %u != 40", cur.large_touch);
    CHECK(cur.noise_reduction == 15,"noise_reduction %u != 15", cur.noise_reduction);
    CHECK(cur.touch_level == 80,    "touch_level %u != 80", cur.touch_level);
    CHECK(cur.leave_level == 50,    "leave_level %u != 50", cur.leave_level);

    /* --- to_block writes only its fields, leaving the rest of the block intact - */
    uint8_t blk[GT911_CFG_LEN];
    memcpy(blk, FACTORY, sizeof(blk));
    gt911_sens_t mod = cur;
    mod.touch_level = 40;
    gt911_sens_to_block(&mod, blk);
    CHECK(blk[12] == 40, "to_block touch_level offset 12 = %u != 40", blk[12]);
    blk[12] = FACTORY[12];                 /* undo the one change we made... */
    CHECK(memcmp(blk, FACTORY, sizeof(blk)) == 0,
          "to_block disturbed bytes outside the knob offsets");

    /* --- serialize -> parse round-trips the current values --- */
    char text[1024];
    int n = gt911_sens_serialize(&cur, text, sizeof(text));
    CHECK(n > 0, "serialize returned %d", n);
    gt911_sens_t parsed = {0};
    gt911_sens_parse(text, &parsed, NULL, NULL);
    CHECK(sens_eq(&parsed, &cur), "serialize/parse round-trip mismatch");

    /* --- lenient parse: good keys apply, bad/unknown skip, others unchanged --- */
    {
        gt911_sens_t s = cur;
        const char *cfg =
            "touch_level = 999\n"        /* out of range -> skip, keep 80 */
            "fooo = 5\n"                 /* unknown key  -> skip */
            "leave_level = 40\n"         /* valid        -> apply */
            "# filter = 99\n"            /* comment      -> ignored */
            "noise_reduction = 7  ; trailing comment\n";
        gt911_sens_parse(cfg, &s, NULL, NULL);
        CHECK(s.touch_level == 80,     "lenient: touch_level changed to %u", s.touch_level);
        CHECK(s.leave_level == 40,     "lenient: leave_level %u != 40", s.leave_level);
        CHECK(s.noise_reduction == 7,  "lenient: noise_reduction %u != 7", s.noise_reduction);
        CHECK(s.filter == cur.filter,  "lenient: commented filter applied (%u)", s.filter);
    }

    /* --- hysteresis: an inverted pair reverts the changed value(s) --- */
    {
        gt911_sens_t s = cur;            /* 80 / 50 */
        gt911_sens_parse("touch_level = 30\n", &s, NULL, NULL);   /* 30 <= 50 */
        CHECK(s.touch_level == 80, "hysteresis: touch_level kept %u, want 80", s.touch_level);

        s = cur;
        gt911_sens_parse("leave_level = 90\n", &s, NULL, NULL);   /* 90 >= 80 */
        CHECK(s.leave_level == 50, "hysteresis: leave_level kept %u, want 50", s.leave_level);

        s = cur;
        gt911_sens_parse("touch_level = 40\nleave_level = 60\n", &s, NULL, NULL); /* 40<=60 */
        CHECK(s.touch_level == 80 && s.leave_level == 50,
              "hysteresis: both should revert, got %u/%u", s.touch_level, s.leave_level);
    }

    if (failures == 0) printf("test_touch_config: all checks passed\n");
    else               printf("test_touch_config: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
