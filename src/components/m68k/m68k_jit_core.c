/* m68k_jit_core.c — architecture-neutral JIT driver.
 *
 * Owns the interpreter-primary dispatch loop, profile-driven hot-PC sampling, the
 * async (core-0) compiler plumbing, and install orchestration. All ISA-specific
 * codegen lives behind the m68k_jit_backend.h interface; the block tables + ring
 * live in m68k_jit_cache.c; the executable memory lives behind jit_execmem.h. The
 * seam to the backend is crossed only at compile/install time — running a native
 * block is a plain function-pointer call, and the per-op presence check in the
 * cold burst is inlined in m68k.c — so this split has no per-instruction cost.
 *
 * Device-only (FreeRTOS): the host build runs the pure interpreter and never
 * compiles this file (same as the original m68k_jit.c). */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "m68k.h"
#include "bus.h"
#include "m68k_jit_backend.h"
#include "m68k_jit_cache.h"
#include "jit_execmem.h"

static const char *TAG = "m68kjit";

static uint64_t g_jit_blk_runs, g_jit_fb_ops;   /* telemetry: native vs fallback cycles */
static bool     g_jit_inited, g_jit_failed;

/* Profile-guided compilation: a direct-mapped saturating hit counter per PC. Only
 * compile a block once its PC has been seen JIT_HOT_THRESHOLD times, so cold
 * (run-once) code never pays the compile cost. */
#define JIT_HOT_SIZE      32768
#define HOT_MASK          (JIT_HOT_SIZE - 1)
#define JIT_HOT_THRESHOLD 3     /* samples (1 per burst) before compiling a PC */
#define JIT_HOT_INFLIGHT  0xFF  /* sentinel: a compile request for this PC is queued */
static uint8_t *g_hot;
#define HOT_SLOT(pc) (&g_hot[(((pc) >> 1) * 2654435761u) & HOT_MASK])

/* ---- Asynchronous compiler (core 0) ----
 * Codegen is the expensive part of a compile and it is PURE (reads only the
 * immutable ROM + compile-scratch). So we run it on a task pinned to core 0 and
 * keep the cpu task (core 1) interpreting meanwhile. The cpu task enqueues a hot
 * PC; the core-0 task assembles the block's BYTES into a slot and hands it back;
 * the cpu task — the sole owner of the exec cache + tables — writes, isyncs, and
 * publishes it. Core 0 never touches executable memory or the tables, so there is
 * no cross-core code-coherency hazard: the queue's barrier orders the byte
 * handoff, and the install (finalize + publish) is single-core on core 1. The
 * slot carries the assembled block by value; backend_block_t is POD (no internal
 * pointers), so the queue copy is self-contained — no fix-up after the copy. */
typedef struct {
    uint32_t        pc, end_pc;     /* neutral routing fields */
    backend_block_t blk;            /* opaque assembled-block payload */
} compile_slot_t;

#define JIT_REQ_DEPTH  64
#define JIT_DONE_DEPTH 8
static QueueHandle_t   g_req_q, g_done_q;
static compile_slot_t *g_cc_slot;     /* core-0 compiler's working slot (PSRAM) */
static compile_slot_t *g_drain_slot;  /* cpu-task drain receive buffer (PSRAM) */
static bool            g_jit_async;   /* true once the core-0 compiler is running */
static uint32_t        g_jit_req_drops; /* telemetry: req-queue-full retries */

/* Install half — runs ONLY on the cpu task (the sole owner of the exec cache and
 * the block/present tables). Lays the assembled block into the segmented ring
 * (evicting a segment if full), isyncs, and publishes it. */
static jit_block_t *jit_install(uint32_t pc, uint32_t end, backend_block_t *blk)
{
    void *entry = backend_finalize(blk, jit_cache_alloc);   /* reserve + patch + write + isync */
    if (!entry) return NULL;          /* only fails if a block exceeds a whole segment */
    jit_insert(pc, end, entry);
    return jit_lookup(pc);
}

/* Synchronous compile (used as the fallback when the async compiler isn't up). */
static jit_block_t *jit_compile(uint32_t pc)
{
    backend_block_t blk; uint32_t end;
    if (!backend_codegen(pc, &blk, &end)) return NULL;
    return jit_install(pc, end, &blk);
}

/* Allocate the exec cache + tables on first use (also called up front by
 * m68k_jit_init). Returns false if the JIT can't run (caller falls back to the
 * pure interpreter). */
static bool jit_ensure_ready(void)
{
    if (g_jit_inited) return true;
    if (g_jit_failed) return false;
    if (!backend_has_codegen()) { g_jit_failed = true; return false; }  /* interp-only backend */

    jit_exec_region_t reg = jit_exec_alloc((size_t)JIT_NSEG * BACKEND_MAX_BLOCK_BYTES);
    if (!reg.base) { g_jit_failed = true; return false; }
    if (!jit_cache_setup(reg.base, reg.size, BACKEND_MAX_BLOCK_BYTES)) {
        g_jit_failed = true;
        return false;
    }

    g_hot = heap_caps_calloc(JIT_HOT_SIZE, 1, MALLOC_CAP_SPIRAM);
    /* The presence bitmap is byte-accessed every op, so it MUST be in
     * byte-addressable DRAM — MALLOC_CAP_INTERNAL alone can hand back IRAM
     * (32-bit-access-only; a byte load faults with LoadStoreError). Require
     * 8BIT internal DRAM, falling back to PSRAM (still byte-accessible). */
    g_jit_present = heap_caps_calloc(JIT_PRESENT_BITS / 8, 1,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_jit_present)
        g_jit_present = heap_caps_calloc(JIT_PRESENT_BITS / 8, 1, MALLOC_CAP_SPIRAM);
    if (!g_hot || !g_jit_present || !esp_ptr_byte_accessible(g_jit_present)) {
        g_jit_failed = true;
        return false;
    }
    ESP_LOGI(TAG, "JIT present-bitmap @ %p (%s)", g_jit_present,
             esp_ptr_internal(g_jit_present) ? "internal" : "psram");
    g_jit_inited = true;
    return true;
}

/* Core-0 compiler: assemble each requested PC's block bytes into a slot and hand
 * it back for the cpu task to publish. Touches no exec memory and no tables. */
static void jit_compiler_task(void *arg)
{
    (void)arg;
    compile_slot_t *slot = g_cc_slot;    /* PSRAM working buffer (off this task's stack) */
    for (;;) {
        uint32_t pc;
        if (xQueueReceive(g_req_q, &pc, portMAX_DELAY) != pdTRUE) continue;
        slot->pc = pc;
        /* Always hand the slot back so the cpu task clears the in-flight sentinel,
         * even when codegen produced nothing (an unsupported leader op). Otherwise
         * that PC's HOT_SLOT stays stuck at JIT_HOT_INFLIGHT forever and it — plus
         * any PC that hashes into the same slot — can never be compiled. end_pc==pc
         * is the "no block" signal (a real block always advances >= 1 op). */
        if (!backend_codegen(pc, &slot->blk, &slot->end_pc))
            slot->end_pc = pc;
        xQueueSend(g_done_q, slot, portMAX_DELAY);    /* copies the slot by value */
    }
}

/* Drain finished blocks and publish them — runs on the cpu task only. */
static void jit_drain_compiled(void)
{
    if (!g_done_q) return;
    compile_slot_t *r = g_drain_slot;
    while (xQueueReceive(g_done_q, r, 0) == pdTRUE) {
        if (r->end_pc != r->pc)          /* skip the "no block" sentinel (codegen failed) */
            jit_install(r->pc, r->end_pc, &r->blk);
        *HOT_SLOT(r->pc) = 0;            /* always clear the in-flight sentinel */
    }
}

/* Profile a candidate block-leader PC: count it, and once hot, compile it (which
 * sets its presence bit so the burst hands it back and it runs native). Used both
 * where a burst stops and at every native block's exit target — the latter chains
 * discovery along the hot path (a block's exit is the next block's leader).
 * With the async compiler up, a hot PC is QUEUED to core 0 (and marked in-flight)
 * instead of compiled inline, so the cpu task keeps running. */
static inline void jit_sample(uint32_t pc)
{
    uint32_t mpc = pc & BUS_ADDR_MASK;
    if (mpc < BUS_ROM_LO || mpc >= g_bus_rom_end || jit_present_bit(mpc)) return;
    uint8_t *h = HOT_SLOT(pc);
    if (g_jit_async) {
        if (*h == JIT_HOT_INFLIGHT) return;             /* already queued */
        if (*h >= JIT_HOT_THRESHOLD) {
            if (xQueueSend(g_req_q, &pc, 0) == pdTRUE) *h = JIT_HOT_INFLIGHT;
            else g_jit_req_drops++;                     /* queue full -> retry next sample */
        } else (*h)++;
    } else {
        if (*h >= JIT_HOT_THRESHOLD) jit_compile(pc);
        else (*h)++;
    }
}

/* Set up the cache/tables and start the core-0 async compiler. Call once at boot
 * (after the ROM is loaded) when the JIT is enabled. Safe to call when disabled —
 * it just makes the JIT sync-capable. */
void m68k_jit_init(void)
{
    if (!jit_ensure_ready()) return;
    g_cc_slot    = heap_caps_malloc(sizeof(compile_slot_t), MALLOC_CAP_SPIRAM);
    g_drain_slot = heap_caps_malloc(sizeof(compile_slot_t), MALLOC_CAP_SPIRAM);
    g_req_q = xQueueCreate(JIT_REQ_DEPTH, sizeof(uint32_t));
    uint8_t *done_storage = heap_caps_malloc(JIT_DONE_DEPTH * sizeof(compile_slot_t),
                                             MALLOC_CAP_SPIRAM);
    static StaticQueue_t done_qcb;
    if (g_req_q && done_storage)
        g_done_q = xQueueCreateStatic(JIT_DONE_DEPTH, sizeof(compile_slot_t),
                                      done_storage, &done_qcb);
    if (g_cc_slot && g_drain_slot && g_req_q && g_done_q &&
        xTaskCreatePinnedToCore(jit_compiler_task, "jitcc", 8192, NULL, 2, NULL, 0) == pdPASS) {
        g_jit_async = true;
        ESP_LOGI(TAG, "async JIT compiler on core 0 (slot=%u B, req=%d done=%d)",
                 (unsigned)sizeof(compile_slot_t), JIT_REQ_DEPTH, JIT_DONE_DEPTH);
    } else {
        ESP_LOGW(TAG, "async compiler setup failed; using synchronous compile");
    }
}

/* Interpreter-primary dispatcher. The fast inlined interpreter (the "burst")
 * runs everything cold, returning here only when it reaches a PC that may have a
 * native block. We run the block, then re-check the next PC directly so hot
 * loops stay native back-to-back. Compilation is profile-driven by sampling the
 * PC where each burst stops. The counters track *cycles* (not ops) spent native
 * vs interpreted — the meaningful ratio for the speed goal. */
void m68k_execute_jit(m68k_cpu_t *cpu, int target_cycles)
{
    if (!g_jit_inited && !jit_ensure_ready()) { m68k_execute_interp(cpu, target_cycles); return; }

    /* Publish any blocks the core-0 compiler finished since the last call. */
    jit_drain_compiled();

    int cyc = 0;
    uint32_t last_periph_seq = g_bus_periph_seq;
    while (cyc < target_cycles) {
        if ((cpu->regs.stopped || cpu->regs.pending_irq) &&
            m68k_service_pending(cpu, &cyc)) continue;

        uint32_t pc = cpu->regs.pc, mpc = pc & BUS_ADDR_MASK;

        if (mpc >= BUS_ROM_LO && mpc < g_bus_rom_end && jit_present_bit(mpc)) {
            jit_block_t *b = jit_lookup(pc);
            if (b && b->entry) {
                int (*fn)(m68k_cpu_t *) = (int (*)(m68k_cpu_t *))b->entry;
                int bc = fn(cpu);              /* block writes cpu->regs.pc itself */
                cyc += bc;
                /* IRQ recognition is deferred to this block boundary: a native
                 * block runs straight through (up to JIT_MAX_BLOCK_OPS ops) and we
                 * recompute pending_irq only here, whereas the interpreter rechecks
                 * after every peripheral-touching op. So an interrupt unmasked mid-
                 * block is taken up to one block late. That is well within 68k IRQ
                 * latency tolerance, and blocks already end at branches/SR-writes,
                 * which bounds the slip in practice. */
                if (g_bus_periph_seq != last_periph_seq && cpu->irq_callback) {
                    last_periph_seq = g_bus_periph_seq;
                    int level = cpu->irq_callback();
                    cpu->regs.pending_irq = (level > 0) ? level : 0;
                }
                g_jit_blk_runs += bc;
                continue;
            }
            /* Presence bit set but no block: a hash false-positive. Single-step
             * one op so we make progress without spinning here. */
            m68k_jit_fallback(cpu);
            cyc += cpu->regs.cycles;
            /* m68k_jit_fallback() is interp_one(), which does NOT recompute
             * pending_irq. If that single op touched a peripheral register
             * (changing db.ipr/db.imr), recompute the level here — exactly like
             * the native-block path above. Just stamping last_periph_seq without
             * recomputing would swallow the seq edge, so neither this path nor
             * the next iteration would notice the new IRQ state until an
             * unrelated later peripheral access bumped the seq again. */
            if (g_bus_periph_seq != last_periph_seq && cpu->irq_callback) {
                last_periph_seq = g_bus_periph_seq;
                int level = cpu->irq_callback();
                cpu->regs.pending_irq = (level > 0) ? level : 0;
            }
            g_jit_fb_ops += cpu->regs.cycles;
            continue;
        }

        /* Cold region: run the inlined interpreter burst at full speed. It stops
         * at the next present PC, a small op budget, or the cycle target. */
        int bc = m68k_jit_interp_burst(cpu, target_cycles - cyc);
        cyc += bc;
        g_jit_fb_ops += bc;
        last_periph_seq = g_bus_periph_seq;   /* burst already serviced IRQs */

        /* Profile the PC the burst stopped on. */
        jit_sample(cpu->regs.pc);
    }
    cpu->regs.cycles_executed = cyc;

    /* Periodic JIT telemetry: native vs interpreted cycle split + live block count.
     * Pairs with the "EMU x.xx MHz" readout in cpu_task. */
    static uint32_t dbg_n;
    if ((++dbg_n & 0x3FF) == 0) {
        uint64_t tot = g_jit_blk_runs + g_jit_fb_ops;
        ESP_LOGI(TAG, "native_cyc=%llu interp_cyc=%llu native=%u%% blocks=%d evict=%lu %s drops=%lu",
                 g_jit_blk_runs, g_jit_fb_ops,
                 (unsigned)(tot ? (g_jit_blk_runs * 100 / tot) : 0), jit_cache_block_count(),
                 (unsigned long)jit_cache_evictions(), g_jit_async ? "async" : "sync",
                 (unsigned long)g_jit_req_drops);
    }
}
