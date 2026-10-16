#pragma once
#include <stddef.h>
#include <stdint.h>

/* Portable RAM checkpoints. No CPU/peripheral pointers are serialized: restore
 * the RAM, then warm-reset Palm OS. Caller must stop all RAM writers while
 * loading/saving and own the mounted filesystem for the duration. */
typedef enum {
    RAM_STATE_OK, RAM_STATE_UNCHANGED, RAM_STATE_MISSING,
    RAM_STATE_INVALID, RAM_STATE_IO
} ram_state_result_t;

typedef enum {
    RAM_STATE_CHECK_SLOT_0, RAM_STATE_CHECK_SLOT_1, RAM_STATE_READING,
    RAM_STATE_HASHING, RAM_STATE_WRITING, RAM_STATE_SYNCING
} ram_state_phase_t;

typedef struct {
    uint32_t rom_crc, ram_bytes, generation, data_crc;
    int active_slot;
    void (*yield)(void);
    /* Optional observer, called at phase boundaries and every 64 KB. It must
     * not modify RAM or start another checkpoint. SYNCING completes only after
     * flush, sync and close succeed. Other phases report bytes, not success. */
    void (*progress)(ram_state_phase_t phase, uint32_t done, uint32_t total);
} ram_state_t;

uint32_t ram_state_crc(const void *data, size_t bytes);
void ram_state_init(ram_state_t *s, uint32_t rom_crc, uint32_t ram_bytes);
/* Files are PREFIX.0 and PREFIX.1. Validate both and fall back to the older
 * valid slot if the newest is truncated/corrupt. On any load error the caller
 * must clear RAM before cold boot (a final read can fail after partial copy). */
ram_state_result_t ram_state_load(ram_state_t *s, const char *prefix, void *ram);
/* Write only the inactive slot; flush and sync before promoting it. A failed
 * or interrupted save leaves the previously active slot untouched. */
ram_state_result_t ram_state_save(ram_state_t *s, const char *prefix, const void *ram);
const char *ram_state_result_str(ram_state_result_t r);
