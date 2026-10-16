#include "ram_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0700)
#define RMDIR(p) rmdir(p)
#endif

#define BYTES 65537
static const char *prefix = "test-palm-ram";
static uint8_t original[BYTES], restored[BYTES], changed[BYTES];
static unsigned phase_starts[6], phase_ends[6], progress_calls;
static uint32_t last_done[6];
static ram_state_t *observed;

static void on_progress(ram_state_phase_t phase, uint32_t done, uint32_t total)
{
    assert(phase >= RAM_STATE_CHECK_SLOT_0 && phase <= RAM_STATE_SYNCING);
    assert(total && done <= total);
    if (!done) { phase_starts[phase]++; last_done[phase] = 0; }
    assert(done >= last_done[phase]);
    last_done[phase] = done;
    if (done == total) phase_ends[phase]++;
    if (phase == RAM_STATE_SYNCING && done == total) {
        /* Completion is reported only after successful commit/promotion. */
        assert(observed->active_slot >= 0 && observed->generation > 0);
    }
    progress_calls++;
}

static void observe(ram_state_t *s)
{
    memset(phase_starts, 0, sizeof(phase_starts));
    memset(phase_ends, 0, sizeof(phase_ends));
    memset(last_done, 0, sizeof(last_done));
    progress_calls = 0;
    observed = s;
    s->progress = on_progress;
}

static void corrupt(const char *path, long off)
{
    FILE *f = fopen(path, "r+b"); assert(f);
    assert(!fseek(f, off, SEEK_SET));
    int c = fgetc(f); assert(c != EOF);
    assert(!fseek(f, off, SEEK_SET));
    assert(fputc(c ^ 0x80, f) != EOF);
    assert(!fclose(f));
}

int main(void)
{
    remove("test-palm-ram.0"); remove("test-palm-ram.1");
    assert(ram_state_crc("123456789", 9) == 0xCBF43926u);
    for (size_t i = 0; i < BYTES; i++) original[i] = (uint8_t)(i * 37 + 13);
    memcpy(changed, original, BYTES); changed[BYTES - 1] ^= 1;
    ram_state_t s;
    ram_state_init(&s, 0x12345678, BYTES);
    memset(restored, 0xA5, BYTES);
    assert(ram_state_load(&s, prefix, restored) == RAM_STATE_MISSING);
    assert(restored[0] == 0xA5);
    observe(&s);
    assert(ram_state_save(&s, prefix, original) == RAM_STATE_OK);
    assert(phase_starts[RAM_STATE_HASHING] == 1 && phase_ends[RAM_STATE_HASHING] == 1);
    assert(phase_starts[RAM_STATE_WRITING] == 1 && phase_ends[RAM_STATE_WRITING] == 1);
    assert(phase_starts[RAM_STATE_SYNCING] == 1 && phase_ends[RAM_STATE_SYNCING] == 1);
    assert(progress_calls < 20);  /* chunked notifications, never per-byte */
    assert(s.active_slot == 0 && s.generation == 1);
    observe(&s);
    assert(ram_state_save(&s, prefix, original) == RAM_STATE_UNCHANGED);
    assert(!phase_starts[RAM_STATE_WRITING] && !phase_starts[RAM_STATE_SYNCING]);
    assert(s.generation == 1);
    assert(ram_state_save(&s, prefix, changed) == RAM_STATE_OK);
    assert(s.active_slot == 1 && s.generation == 2);

    ram_state_init(&s, 0x12345678, BYTES);
    observe(&s);
    assert(ram_state_load(&s, prefix, restored) == RAM_STATE_OK);
    assert(phase_ends[RAM_STATE_CHECK_SLOT_0] && phase_ends[RAM_STATE_CHECK_SLOT_1]);
    assert(phase_starts[RAM_STATE_READING] == 1 && phase_ends[RAM_STATE_READING] == 1);
    assert(s.active_slot == 1 && !memcmp(restored, changed, BYTES));
    /* Damaged newest save, including its final partial block: use older RAM. */
    corrupt("test-palm-ram.1", 32 + BYTES - 1);
    assert(ram_state_load(&s, prefix, restored) == RAM_STATE_OK);
    assert(s.active_slot == 0 && !memcmp(restored, original, BYTES));
    FILE *f = fopen("test-palm-ram.1", "wb"); assert(f);
    assert(fwrite("interrupted", 1, 11, f) == 11); assert(!fclose(f));
    assert(ram_state_load(&s, prefix, restored) == RAM_STATE_OK);
    assert(!memcmp(restored, original, BYTES));

    /* Files for another ROM or RAM capacity must never be used. */
    ram_state_init(&s, 0x87654321, BYTES);
    assert(ram_state_load(&s, prefix, restored) == RAM_STATE_INVALID);
    ram_state_init(&s, 0x12345678, BYTES - 1);
    assert(ram_state_load(&s, prefix, restored) == RAM_STATE_INVALID);
    ram_state_init(&s, 0x12345678, BYTES);
    assert(ram_state_load(&s, prefix, restored) == RAM_STATE_OK);

    /* Fail opening the inactive destination. Previously committed RAM survives. */
    assert(!remove("test-palm-ram.1")); assert(!MKDIR("test-palm-ram.1"));
    observe(&s);
    assert(ram_state_save(&s, prefix, changed) == RAM_STATE_IO);
    assert(!phase_ends[RAM_STATE_SYNCING]);
    assert(s.active_slot == 0 && s.generation == 1);
    assert(!RMDIR("test-palm-ram.1"));
    assert(ram_state_load(&s, prefix, restored) == RAM_STATE_OK);
    assert(!memcmp(restored, original, BYTES));

    /* A damaged generation/header must not make an old payload appear newest. */
    assert(ram_state_save(&s, prefix, changed) == RAM_STATE_OK);
    corrupt("test-palm-ram.1", 20);
    assert(ram_state_load(&s, prefix, restored) == RAM_STATE_OK);
    assert(!memcmp(restored, original, BYTES));
    corrupt("test-palm-ram.0", 0);
    assert(ram_state_load(&s, prefix, restored) == RAM_STATE_INVALID);
    remove("test-palm-ram.0"); remove("test-palm-ram.1");
    puts("ALL RAM STATE TESTS PASSED");
    return 0;
}
