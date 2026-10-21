#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "m68k.h"
#include "board.h"
#include "bus.h"
#include "dragonball.h"
#include "display.h"
#include "input.h"
#include "debug_console.h"
#include "serial_bridge.h"
#include "wifi_gateway.h"
#include "rom_loader.h"
#if CONFIG_BOARD_JC4827W543
#include "boot_menu.h"
#include "cybiko_app.h"
#include "display_ui.h"
#endif
#include "layout.h"
#include "palm_config.h"
#include "installer.h"
#include "backup.h"
#include "ram_state.h"
#include "m5_sd.h"
#include "palm_card.h"
#include "audio.h"

static const char *TAG = "emu";

m68k_cpu_t *cpup;
#define cpu (*cpup)
static volatile uint32_t total_cycles = 0;

/* M5Paper middle button = GPIO38 (active-low, external pull-up): the power/
 * wake-sleep button and the light-sleep wake source. */
#define PIN_BTN_POWER BOARD_BTN_POWER
#define POWER_HOLD_MS  1000     /* >=1s hold to wake from light sleep */

/* MC68EZ328 core clock. The IIIx clocks the DragonBall EZ at 16.58 MHz; the cpu
 * task runs CYCLES_PER_BATCH (~1 ms of emulated time) per iteration and reports
 * the live "% of real" readout against CPU_MHZ. */
#define CPU_HZ            16580000
#define CPU_MHZ           16.58
#define CYCLES_PER_BATCH  (CPU_HZ / 1000)   /* ~1 ms of emulated time */

/* Hold serial pass-through across a brief UARTEN drop so the ROM's mid-open
 * reconfigure blips (UARTEN low for ~tens of ms) don't tear the bridge down. */
#define SERIAL_OFF_DEBOUNCE_MS  250

/* Delivery rate for Wi-Fi PPP stream to Palm UART in bytes per emulated ms * 100.
 * ~8 bytes/ms corresponds to ~80 kbps, which matches Palm OS's serial driver
 * processing capacity without overrunning its internal software receive queue. */
#define WIFI_RX_BYTES_PER_MS_X100 800

/* Light-sleep coordination. The CPU task is the sole sleep driver; before it
 * calls esp_light_sleep_start() it asks the display + input tasks to park at a
 * loop boundary (so neither is mid SPI/I2C) and waits for both to acknowledge
 * via the event group. */
static volatile bool      esp32_powersave_requested = false;
static EventGroupHandle_t park_eg = NULL;

/* Panel-refresh gate (toggled by the `disp on|off` debug-console command). When
 * off, the display task blanks the e-ink once and then stops refreshing it — used
 * during long autonomous soak tests to spare e-ink wear while the emulator and
 * the serial console keep running. Default on. */
volatile bool g_display_enabled = true;

/* One-shot e-ink waveform timing benchmark (debug-console `wavetime`). Set by
 * the console; serviced by the display task (the sole IT8951 SPI owner) which
 * runs display_run_waveform_benchmark() and clears it. */
volatile bool g_wave_benchmark_request = false;
#define PARK_DISPLAY  (1 << 0)
#define PARK_INPUT    (1 << 1)
#define PARK_ALL      (PARK_DISPLAY | PARK_INPUT)

/* --- PDB/PRC auto-install (components/installer) ---------------------------
 * Files in /PalmOS/install on the SD are buffered into PSRAM at boot and installed by
 * the cpu task (via the shared installer_pump) once the OS is up. All of the
 * install runtime state lives in one struct (install_runtime_t, see installer.h):
 * the cpu task is the sole writer of every field except `force` (the console's
 * `install now`), which is why only `force` is volatile. The warm-reset request
 * is intentionally a separate flag — it is not install state (the `reset` console
 * command drives it too). */
install_runtime_t g_install = { .last = INSTALLER_OK };
volatile bool g_warm_reset_request = false;        /* console "reset" / batch reset-after-install */

/* --- Backup / restore (components/installer backup engine) -----------------
 * The console requests a backup or restore by writing g_backup.arg then setting
 * a request flag; the cpu task (sole CPU-state writer) services it between
 * batches, parking the display+input tasks for the SD window — exactly like the
 * warm reset. Only the two request booleans + arg cross threads; the rest is
 * cpu-task-private and read racily by `backup status`. */
backup_runtime_t g_backup = { .last = BACKUP_OK };

/* SD checkpoints contain RAM only; Palm warm-boots after loading one. */
static ram_state_t ram_state;
static char ram_state_prefix[64];
#if CONFIG_BOARD_JC4827W543
static bool ram_state_enabled = true;
#else
static bool ram_state_enabled;
#endif
static bool ram_restored;
static uint8_t sound_volume = 60;   /* palm.cfg sound.volume, 0..100; 0 = off */
#if CONFIG_BOARD_JC4827W543
static bool card_enabled = true, card_read_only;
/* Hold one mount reference for Palm's open file handles. */
static sdmmc_card_t *palm_sd_card;
static bool card_space(uint32_t *used, uint32_t *total)
{ return m5_sd_space(used, total) == ESP_OK; }
#endif
static bool ram_progress_available, ram_progress_restoring;
static int ram_progress_phase = -1, ram_progress_percent = -1;
static volatile bool ram_status_latched;
volatile bool g_ram_save_request;
volatile unsigned g_ram_save_count;
volatile int g_ram_last_result = RAM_STATE_MISSING;

static void ram_state_yield(void) { vTaskDelay(1); }

static void ram_progress_begin(bool restoring)
{
    ram_progress_restoring = restoring;
    ram_progress_phase = ram_progress_percent = -1;
    if (ram_progress_available && g_display_enabled)
        display_show_progress(restoring ? "RESTORING RAM" : "SAVING RAM", "MOUNTING SD", -1);
}

static void ram_progress_update(ram_state_phase_t phase, uint32_t done, uint32_t total)
{
    static const char *const labels[] = {
        "CHECK SAVE 1", "CHECK SAVE 2", "READING SD", "CHECKING RAM", "WRITING SD", "FINALIZING"
    };
    int percent = phase == RAM_STATE_SYNCING || !total ? -1 : (int)((uint64_t)done * 100 / total);
    if (phase == ram_progress_phase && (percent == ram_progress_percent ||
        (percent >= ram_progress_percent && percent < ram_progress_percent + 5 && percent != 100)))
        return;
    if (phase != ram_progress_phase)
        ESP_LOGI(TAG, "RAM %s: %s", ram_progress_restoring ? "restore" : "save", labels[phase]);
    ram_progress_phase = phase;
    ram_progress_percent = percent;
    if (ram_progress_available && g_display_enabled)
        display_show_progress(ram_progress_restoring ? "RESTORING RAM" : "SAVING RAM", labels[phase], percent);
}

/* Yield hook the installer calls every 32 records so a large .pdb install can't
 * starve the core-1 idle task past the 5 s task-watchdog window. */
static void installer_yield_cb(void) { vTaskDelay(1); }

/* Big-endian stores into the raw RAM image at a byte offset (wb16/wb32 from bus.h). */
static void ram_w32(uint8_t *r, uint32_t off, uint32_t v) { wb32(r + off, v); }
static void ram_w16(uint8_t *r, uint32_t off, uint16_t v) { wb16(r + off, v); }

#define SAFE_RTE_ADDR 0x001F0100

static void setup_exception_vectors(void)
{
    uint8_t *ram = bus_ram_ptr();
    ram_w16(ram, SAFE_RTE_ADDR, 0x4E73);
    for (int vec = 2; vec < 256; vec++)
        ram_w32(ram, vec * 4, SAFE_RTE_ADDR);
}

/* Enqueue a key via the ROM's EvtEnqueueKey trap (0xA12D), resolved live from
 * the trap table (globals + selector*4), and wake the CPU to drain it. MUST run
 * on the cpu_task — the sole writer of CPU state. Returns the resolved handler
 * address, or 0 if the trap table isn't ready. Shared by the Home/Power buttons
 * and the debug console's key injection. */
static uint32_t inject_key_event(uint16_t ascii, uint16_t keycode, uint16_t modifiers)
{
    uint8_t *r = bus_ram_ptr();
    #define RDK(off) (((uint32_t)r[(off)]<<24)|((uint32_t)r[(off)+1]<<16)| \
                      ((uint32_t)r[(off)+2]<<8)|r[(off)+3])
    uint32_t globals = RDK(0x122);
    if (!globals || globals + 0x12D*4 + 4 >= BUS_RAM_SIZE) return 0;
    uint32_t h = RDK(globals + 0x12D*4);   /* EvtEnqueueKey A12D */
    #undef RDK
    if (!bus_addr_in_rom(h)) return 0;
    cpu.regs.stopped = false;              /* ensure awake to drain the event */
    m68k_key_enqueue(&cpu, h, ascii, keycode, modifiers);
    return h;
}

/* Wake the emulated Palm from its PLL-off STOP by pulsing the scanned-matrix
 * POWER key — the only authentic wake source. Runs inline on the CPU task (the
 * sole writer of CPU state) and mirrors the cpu_task batch step.
 *
 * Crucially we do NOT clear cpu.regs.stopped ourselves: the m68k STOP idle loop
 * (m68k.c) must see INT_KB pending via irq_callback, clear 'stopped', run the
 * resume_callback (db_cpu_resume clears DISPLL), and TAKE the KB interrupt so
 * the ROM's wake ISR runs. Forcing 'stopped=false' here skips the ISR and runs
 * the post-STOP instruction raw, which crashes the ROM (bad PC). */
static void wake_emulated_device(void)
{
    db_kbd_set(KBD_KEY_POWER, true);
    /* Hold the matrix POWER key through the FULL ROM wake sequence (~60 ms,
     * matching the verified console power_matrix_pulse). Do NOT gate on
     * db_is_asleep(): DISPLL clears at the very first instant of wake, long
     * before HwrSleep finishes consuming the key. Releasing that early leaves a
     * stray vchrHardPower press that the now-awake OS reads as a sleep request,
     * which re-sleeps the device immediately (toggle loop). */
    for (int i = 0; i < 130; i++) {        /* ~520k cycles ~= 60 ms */
        m68k_execute(&cpu, 4000);          /* idle loop takes INT_KB */
        db_timer_advance(4000);
        m68k_set_irq(&cpu, db_interrupt_pending_level());
    }
    db_kbd_set(KBD_KEY_POWER, false);
    for (int i = 0; i < 20; i++) {         /* let the key-up register */
        m68k_execute(&cpu, 4000);
        db_timer_advance(4000);
        m68k_set_irq(&cpu, db_interrupt_pending_level());
    }
}

/* Ask the display + input tasks to park at a loop boundary and wait for both to
 * ack, so neither is mid SPI/I2C (nor mid-read of db) while the cpu task does
 * something disruptive. Returns true if both parked within the timeout; on
 * failure it clears the request so any partially-parked task resumes. Shared by
 * the light-sleep path and the warm reset. MUST be called from the cpu task. */
static bool park_io_tasks(uint32_t timeout_ms)
{
    if (!park_eg) return false;
    xEventGroupClearBits(park_eg, PARK_ALL);
    esp32_powersave_requested = true;
    EventBits_t b = xEventGroupWaitBits(park_eg, PARK_ALL, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(timeout_ms));
    if ((b & PARK_ALL) != PARK_ALL) {
        esp32_powersave_requested = false;     /* let a partially-parked task resume */
        return false;
    }
    return true;
}

/* Release the display + input tasks parked by park_io_tasks. */
static void unpark_io_tasks(void)
{
    esp32_powersave_requested = false;
    if (park_eg) xEventGroupClearBits(park_eg, PARK_ALL);
}

/* Called between CPU batches at an OS STOP boundary. Both I/O tasks must be
 * parked: input shares SPI2 with SD and display reads Palm RAM. A STOP outside
 * ROM subcalls gives the OS a consistent heap for the subsequent warm boot. */
static void maybe_save_ram(void)
{
    static bool was_asleep, sleep_save_pending;
    bool asleep = db_is_asleep();
    if (!asleep) {
        ram_status_latched = false;
        sleep_save_pending = false; /* never defer an automatic save into active use */
    }
    if (asleep && !was_asleep) sleep_save_pending = true;
    was_asleep = asleep;
    if (!ram_state_enabled) { g_ram_save_request = sleep_save_pending = false; return; }
    /* Automatic checkpoints happen only during Palm sleep, never an awake
     * event-loop STOP. An explicit console `state save` remains available. */
    if (!(g_ram_save_request || sleep_save_pending) || !cpu.regs.stopped || !m68k_ui_initialized() ||
        g_install.phase == INST_WAIT || g_install.phase == INST_RUN || g_backup.req != BR_REQ_NONE)
        return;
    if (!park_io_tasks(1000)) return;
    ram_progress_begin(false);
    static bool memory_reported;
    if (!memory_reported && !asleep) {
        uint32_t palm_ram, palm_free;
        if (installer_memory_info(&cpu, &palm_ram, &palm_free)) {
            ESP_LOGI(TAG, "Palm memory: RAM=%lu free=%lu bytes",
                     (unsigned long)palm_ram, (unsigned long)palm_free);
            memory_reported = true;
        }
    }
    sdmmc_card_t *card = NULL;
    ram_state_result_t r = RAM_STATE_IO;
    if (m5_sd_mount(&card) == ESP_OK) {
        if (palm_card_sync())
            r = ram_state_save(&ram_state, ram_state_prefix, bus_ram_ptr());
        else
            ESP_LOGW(TAG, "Virtual card flush failed; power-off save aborted");
        m5_sd_unmount(card);
    }
    g_ram_last_result = r;
    g_ram_save_request = sleep_save_pending = false;
    if (r == RAM_STATE_OK || r == RAM_STATE_UNCHANGED) {
        if (r == RAM_STATE_OK) g_ram_save_count++;
        ESP_LOGI(TAG, "RAM checkpoint: %s, generation=%lu (%u KB)", ram_state_result_str(r),
                 (unsigned long)ram_state.generation, BUS_RAM_SIZE / 1024);
    } else {
        ESP_LOGW(TAG, "RAM checkpoint failed: %s; previous save retained", ram_state_result_str(r));
    }
    if (ram_progress_available && g_display_enabled) {
        bool saved = r == RAM_STATE_OK || r == RAM_STATE_UNCHANGED;
        display_show_message(saved ? "RAM SAVED\n\nSAFE TO\nPOWER OFF" : "SAVE FAILED\n\nOLD SAVE KEPT");
        /* Keep completion visible while Palm sleeps. Explicit awake saves return to
         * the Palm framebuffer after a short acknowledgement. Neither overlay
         * writes into the emulated framebuffer or the checkpoint payload. */
        ram_status_latched = asleep;
        if (!asleep) vTaskDelay(pdMS_TO_TICKS(saved ? 900 : 2500));
    }
    unpark_io_tasks();
}

/* Warm ("soft") reset core, driven from the cpu task (the sole CPU-state writer):
 * re-init the EZ328 peripherals and re-run the 68k reset vector WITHOUT clearing RAM,
 * so storage databases survive and the ROM warm-boots. ASSUMES the display+input tasks
 * are already parked — dragonball_init memsets db (transiently zeroing the LCD
 * framebuffer address), so the display task must not be mid-read. The JIT exec cache
 * stays valid because the ROM is unchanged. */
static void warm_reset_core(void)
{
    ESP_LOGI(TAG, "warm reset: rebooting emulated Palm (RAM/databases preserved)");
    palm_card_reset();
    dragonball_init();
    m68k_warm_reset(&cpu);
    m68k_set_irq(&cpu, db_interrupt_pending_level());
}

/* Warm reset for callers that are NOT already parked: the `reset` console command and
 * the install reset-after-install path (both via g_warm_reset_request). Parks the I/O
 * tasks across the reset, then unparks; proceeds even on a park timeout — the user-
 * visible reset matters more than a rare torn read. (battery_task is not parked,
 * matching the sleep path; its db writes are benign and re-issued each cycle, and the
 * ADS7846 battery sample is a spi.c file-static dragonball_init does NOT clear, so the
 * warm reboot can't trip the low-battery shutdown.) The backup/restore paths instead
 * call warm_reset_core directly inside their existing park window (see run_br_engine),
 * which avoids an unpark-then-repark that the just-woken tasks can't ack in time. */
static void do_warm_reset(void)
{
    if (!park_io_tasks(1000))
        ESP_LOGW(TAG, "warm reset: I/O tasks did not park in time; proceeding anyway");
    warm_reset_core();
    unpark_io_tasks();
}

/* Readiness gate shared by the console (run_sd_op) and button (run_br_with_confirm)
 * backup/restore paths: both drive Palm ROM traps, so the OS must be up and the
 * dynamic heap functional. Backup verifies the enumerate/read traps (backup_os_ready),
 * restore the install traps (installer_os_ready); both then probe the heap. */
static bool run_br_ready(bool restore)
{
    bool ready = restore ? installer_os_ready() : backup_os_ready();
    return ready && installer_probe_heap(&cpu);
}

/* Run the backup/restore engine with the display+input tasks ALREADY parked (the SD
 * shares the display SPI bus), tally the result into g_backup, and decide whether the
 * emulated Palm must warm-reset. Shared by run_sd_op and run_br_with_confirm so the
 * load-bearing reset decision lives in exactly one place. Returns the engine result.
 *
 * A restore swaps databases the live OS may still reference by a cached LocalID (e.g.
 * Saved Preferences) -> dangling refs -> "Invalid chunk ptr"/"Chunk under-locked"
 * crashes on the next access. So after a restore that touched ANY database we warm-
 * reset (serviced just after, in cpu_task) to rebuild the dynamic heap and re-read
 * every DB fresh. We key on ok>0 || fail>0 (not just ok>0) because
 * installer_install_image deletes the existing DB BEFORE recreating it and only rolls
 * back the NEW db on failure — a delete-then-fail (counted in fail) still mutated
 * storage and must reset. The restore runs entirely in cpu-task trap subcalls, so the
 * OS event loop never executes between the DB swap and the reset. We're already parked,
 * so do the reset INLINE (warm_reset_core) rather than via g_warm_reset_request: that
 * deferred path would unpark then immediately re-park, which the just-woken I/O tasks
 * can't ack within do_warm_reset's 1 s window ("I/O tasks did not park in time"). */
static backup_result_t run_br_engine(bool restore, const char *name)
{
    const char *what = restore ? "restore" : "backup";
    g_backup.phase = BR_RUNNING;
    int ok = 0, fail = 0, skipped = 0;
    bool need_reset = false;
    backup_result_t r = restore
        ? backup_restore_from_sd(&cpu, name, &ok, &fail, &need_reset)
        : backup_run_to_sd(&cpu, name, &ok, &fail, &skipped);
    g_backup.ok = ok; g_backup.fail = fail; g_backup.skipped = skipped;
    g_backup.last = r; g_backup.phase = BR_DONE;
    ESP_LOGI(TAG, "%s complete: %d ok, %d failed, %d skipped (%s)",
             what, ok, fail, skipped, backup_result_str(r));
    if (restore && (need_reset || ok > 0 || fail > 0))
        warm_reset_core();             /* tasks already parked by the caller */
    return r;
}

/* Service a console-requested backup or restore (BR_REQ_BACKUP/BR_REQ_RESTORE),
 * filtered by `name` (a snapshot of g_backup.arg; NULL/"" = all). Runs on the cpu
 * task. Readiness-gated, then parks the display+input tasks across the SD window
 * (shared SPI bus); on a park timeout it ABORTS as busy rather than racing a
 * concurrent IT8951 transaction (mirroring maybe_enter_light_sleep). The engine call
 * + tally + warm-reset decision are shared with the button path via run_br_engine. */
static void run_sd_op(int kind, const char *name)
{
    const bool restore = (kind == BR_REQ_RESTORE);
    const char *what = restore ? "restore" : "backup";

    if (!run_br_ready(restore)) {
        g_backup.last = BACKUP_ERR_NOT_READY;
        g_backup.phase = BR_DONE;
        ESP_LOGW(TAG, "%s: OS not ready", what);
        return;
    }
    if (!park_io_tasks(1000)) {           /* shared SPI bus — do NOT proceed unparked */
        g_backup.last = BACKUP_ERR_BUSY;
        g_backup.phase = BR_DONE;
        ESP_LOGW(TAG, "%s: I/O tasks did not park; aborting (retry)", what);
        return;
    }
    run_br_engine(restore, name);
    unpark_io_tasks();
}

/* On-device backup/restore triggered by a >=1s hold of the UP (backup) / DOWN
 * (restore) side button (latched in input.c, drained in cpu_task). Like run_sd_op it
 * runs on the cpu task, but wraps run_br_engine in an on-panel confirmation UI inside a
 * SINGLE park window: a 3..1 countdown the user must keep HOLDING through (releasing
 * cancels — a dead-man's switch, since restore is destructive), then the engine, then a
 * result screen. Every e-ink draw and SD access happens with the display+input tasks
 * parked, so nothing races the shared SPI bus. */
static void run_br_with_confirm(bool restore)
{
    const char *what  = restore ? "RESTORE" : "BACKUP";
    const int   which = restore ? 1 : 0;            /* button to poll: 1=DOWN, 0=UP */

    /* Readiness — surface "not ready" on the panel (there's no serial reader in the
     * field), but only if we can park to draw safely. */
    if (!run_br_ready(restore)) {
        g_backup.last = BACKUP_ERR_NOT_READY;
        g_backup.phase = BR_DONE;
        ESP_LOGW(TAG, "%s: OS not ready", what);
        if (park_io_tasks(1000)) {
            char m[32]; snprintf(m, sizeof(m), "%s\nNOT READY", what);
            display_show_message(m);
            vTaskDelay(pdMS_TO_TICKS(1500));
            display_resume_after_wake();
            unpark_io_tasks();
        }
        return;
    }

    if (!park_io_tasks(1000)) {                     /* shared SPI bus — abort, don't race */
        g_backup.last = BACKUP_ERR_BUSY;
        g_backup.phase = BR_DONE;
        ESP_LOGW(TAG, "%s: I/O tasks did not park; aborting", what);
        return;
    }

    /* Confirm countdown: hold to confirm, release to cancel. Every digit frame uses the
     * fast A2 waveform so the cancel poll starts promptly and the ~3 s feels like 3 s (a
     * GC16 frame would block ~2-5 s before the first poll); the closing result/resume
     * GC16 clears any A2 ghosting. Require a few consecutive not-held samples before
     * cancelling so a contact bounce / momentary lift can't abort the op. */
    bool committed = true;
    for (int n = 3; n >= 1 && committed; n--) {
        char m[32]; snprintf(m, sizeof(m), "%s\n\nHOLD: %d", what, n);
        display_show_message_fast(m);
        int misses = 0;
        for (int t = 0; t < 50; t++) {              /* ~1 s dwell, polling the held button */
            if (!input_button_held(which)) {
                if (++misses >= 3) { committed = false; break; }   /* ~60 ms release debounce */
            } else {
                misses = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (!committed) {
        ESP_LOGI(TAG, "%s cancelled (button released)", what);
        g_backup.phase = BR_IDLE;                   /* not a completed op — keep `backup status` honest */
        display_show_message("CANCELLED");
        vTaskDelay(pdMS_TO_TICKS(1200));
        display_resume_after_wake();
        unpark_io_tasks();
        return;
    }

    /* Run the engine (it mounts/unmounts the SD itself). Show a holding message first
     * since the engine blocks for seconds with no panel update. */
    display_show_message(restore ? "RESTORING\n\nPLEASE WAIT" : "BACKING UP\n\nPLEASE WAIT");
    backup_result_t r = run_br_engine(restore, NULL);

    char m[48];
    if (r == BACKUP_OK) snprintf(m, sizeof(m), "%s DONE\n\n%d OK %d FAIL", what, g_backup.ok, g_backup.fail);
    else                snprintf(m, sizeof(m), "%s\nFAILED\n\n%s", what, backup_result_str(r));
    display_show_message(m);                        /* GC16 for clarity */
    vTaskDelay(pdMS_TO_TICKS(2500));

    /* Repaint and release the I/O tasks: a backup returns to the live Palm screen; a
     * restore that warm-reset inside run_br_engine boots fresh on the next batch. The
     * full GC16 forced here is fine — we unpark exactly once (no re-park to race). */
    display_resume_after_wake();
    unpark_io_tasks();
}

/* When the emulated device is in PLL-off sleep, power the ESP32 down into light
 * sleep (preserving RAM/CPU state and PSRAM). Quiesce the display+input tasks,
 * blank the panel, arm the power button as the wake source, and sleep. On a
 * deliberate >=1s button hold, repaint and wake the emulated device; a shorter
 * bump just re-sleeps. Called once per batch from the CPU task. */
static void maybe_enter_light_sleep(void)
{
#if !CONFIG_BOARD_JC4827W543
    if (!park_eg || !db_is_asleep() || !cpu.regs.stopped)
        return;

    /* Ask display + input to park at a loop boundary, then wait for both acks so
     * neither is mid SPI/I2C when we cut the clocks. */
    if (!park_io_tasks(3000)) {
        ESP_LOGW(TAG, "SLEEP ESP32 abort: tasks did not park");
        return;                            /* retry next batch */
    }

    display_blank_for_sleep();             /* tied to ESP32 power-down, not emu sleep */

    /* The same long-hold that triggered sleep is often still pressing GPIO38 when
     * we get here. Since GPIO38 is the wake source, wait for it to be released
     * first so the tail of the sleep-hold can't immediately bounce us awake.
     * Bounded so a stuck read still lets us sleep. */
    for (int i = 0; i < 150 && gpio_get_level(PIN_BTN_POWER) == 0; i++)
        vTaskDelay(pdMS_TO_TICKS(20));

    esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_ON);  /* keep PSRAM */
    gpio_wakeup_enable(PIN_BTN_POWER, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    for (;;) {
        ESP_LOGI(TAG, "SLEEP ESP32 ENTER");
        esp_light_sleep_start();           /* whole SoC halts; resumes here on wake */

        /* Woke because the power button (GPIO38) went low. Require a >=1s hold to
         * treat it as a deliberate wake; otherwise re-sleep (bump rejection). */
        bool held = true;
        for (int i = 0; i < POWER_HOLD_MS / 20; i++) {
            if (gpio_get_level(PIN_BTN_POWER) != 0) { held = false; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (held) break;
        ESP_LOGI(TAG, "SLEEP ESP32 wake bump (<1s) — back to sleep");
    }
    gpio_wakeup_disable(PIN_BTN_POWER);
    ESP_LOGI(TAG, "SLEEP ESP32 WAKE PC=%08lX", (unsigned long)cpu.regs.pc);

    /* Wait for the wake-hold to be released before waking the emulated device,
     * so a still-held button can't bleed into post-wake input polling or bounce
     * the device straight back into the sleep/wake loop. Bounded. */
    for (int i = 0; i < 250 && gpio_get_level(PIN_BTN_POWER) == 0; i++)
        vTaskDelay(pdMS_TO_TICKS(20));

    /* Repaint and wake the emulated device while the display task is still parked
     * (so the silkscreen redraw doesn't race the task's own SPI), then release. */
    display_resume_after_wake();
    wake_emulated_device();
    unpark_io_tasks();
#endif
    /* JC4827W543 stays awake so BOOT and the console can wake Palm OS. */
}

static void cpu_task(void *param)
{
    const int cycles_per_batch = CYCLES_PER_BATCH;
    uint32_t last_log_cycles = 0;

    ESP_LOGI(TAG, "CPU task started on core %d", xPortGetCoreID());

    /* Per-batch counters feeding the live "EMU x.xx MHz (% of real)" readout
     * below: meas_sb_total counts sub-batches, meas_sb_idle those entered while
     * the emulated CPU was stopped (the idle fraction). */
    static uint32_t meas_sb_total = 0, meas_sb_idle = 0;
    TickType_t meas_last_tick = xTaskGetTickCount();
    TickType_t last_yield_tick = meas_last_tick;

    for (;;) {
        const int sub_batch = 4000;
        int remaining = cycles_per_batch;
        while (remaining > 0) {
            int n = (remaining < sub_batch) ? remaining : sub_batch;
            bool was_stopped = cpu.regs.stopped;
            m68k_execute(&cpu, n);
            meas_sb_total++;
            if (was_stopped) meas_sb_idle++;

            /* Check interrupts BEFORE timer advance so that lower-priority
             * interrupts (e.g. PEN at level 5) can fire in the window after
             * the timer ISR clears INT_TMR but before timer re-raises it. */
            db_timer_advance(n);

            int irq = db_interrupt_pending_level();
            m68k_set_irq(&cpu, irq);

            remaining -= n;
        }
        total_cycles += cycles_per_batch;

        /* Inject queued pen taps via the ROM's EvtEnqueuePenPoint trap — the
         * device-independent path (the raw pen-IRQ digitizer path re-enters
         * the AMXHardwareEZ pen handler and fatal-errors on the IIIx). Resolve
         * the handlers from the live trap table (globals + selector*4).
         *
         * No SR-mask gate: EvtEnqueuePenPoint is exactly what the real pen ISR
         * calls, so it is safe at any interrupt level, and m68k_pen_tap saves/
         * restores the full CPU state around a self-contained subcall. Gating
         * on (SR mask == 0) blocked every tap on the Setup calibration screen,
         * which runs at IPL>=4. After enqueuing we force the CPU awake so its
         * event loop drains the point even if it was dozing in a STOP loop —
         * mimicking the wake the real pen IRQ would cause. This runs on the
         * CPU task, the sole writer of cpu state. */
        input_pen_event_t pev;
        if (input_pop_pen_event(&pev) && !db_is_asleep()) {
            /* A pen tap cannot wake a sleeping IIIx (authentic) — when asleep
             * the event is popped and dropped, never injected, so it can't
             * force the CPU out of STOP. Only the POWER key (matrix KB
             * interrupt) wakes from deep sleep. */
            uint8_t *r = bus_ram_ptr();
            #define RDRAM32(off) (((uint32_t)r[(off)]<<24)|((uint32_t)r[(off)+1]<<16)| \
                                  ((uint32_t)r[(off)+2]<<8)|r[(off)+3])
            uint32_t globals = RDRAM32(0x122);
            if (globals && globals + 0x273*4 + 4 < BUS_RAM_SIZE) {
                uint32_t evtEnq = RDRAM32(globals + 0x126*4);  /* EvtEnqueuePenPoint A126 */
                uint32_t penRaw = RDRAM32(globals + 0x273*4);  /* PenScreenToRaw     A273 */
                if (bus_addr_in_rom(evtEnq)) {
                    m68k_pen_tap(&cpu, evtEnq,
                                 bus_addr_in_rom(penRaw) ? penRaw : 0,
                                 pev.x, pev.y, pev.pen_up);
                    cpu.regs.stopped = false;   /* wake to drain the new event */
                    /* Only log stroke endpoints — pen-down (with full coords)
                     * is logged on the input side; logging every streamed move
                     * point here would flood UART0 and stall this task. */
                    if (pev.pen_up)
                        ESP_LOGI(TAG, "pen UP -> evtEnq=%08lX", (unsigned long)evtEnq);
                }
            }
            #undef RDRAM32
        }

        /* Power button (awake long-hold): inject vchrHardPower so the running OS
         * sleeps. The ROM's own matrix scan decodes this key but the running
         * event loop never acts on that event; the syscall path is the one that
         * is processed. Waking uses the matrix key instead. */
        if (input_pop_power_key()) {
            if (db_is_asleep()) {
                wake_emulated_device();
            } else {
                uint32_t h = inject_key_event(0x0208 /*vchrHardPower*/, 0, 0x0008 /*commandKeyMask*/);
                if (h) ESP_LOGI(TAG, "POWER key injected -> EvtEnqueueKey=%08lX", (unsigned long)h);
            }
        }

        /* On-device backup/restore: a >=1s hold of the UP (backup) / DOWN
         * (restore) side button latched a request in the input task; service it
         * here (sole CPU-state writer) with the confirm-countdown + engine. Runs
         * BEFORE the warm-reset check below so a restored reset-after-install DB
         * reboots the same loop, exactly like run_sd_op. */
        int br = input_pop_br_request();
        if (br != INPUT_BR_NONE)
            run_br_with_confirm(br == INPUT_BR_RESTORE);

        /* Debug-console key injection: same path, arbitrary key. */
        input_key_event_t kev;
        while (input_pop_key(&kev)) {
            uint32_t h = inject_key_event(kev.ascii, kev.keycode, kev.modifiers);
            if (h) ESP_LOGI(TAG, "console key %04X injected -> EvtEnqueueKey=%08lX",
                            kev.ascii, (unsigned long)h);
        }

        /* Serial pass-through auto-switch (honored only in AUTO mode). The bridge
         * follows the LEVEL of the ROM's serial-port-open state, asserted every
         * batch (serial_bridge_request self-dedups, so this is cheap). A level
         * drive — not an edge — is deliberate: the ROM toggles USTCNT_UARTEN
         * several times while opening the port (verified on device: 0000->E508,
         * E508->0508, 0508->E508 within ~50 ms as it reconfigures baud/bits), and
         * clears it (E508->6508) when the HotSync app closes. An edge trigger could
         * desync on those blips or across a sleep/wake; re-asserting the truth each
         * batch cannot. Gate on the UMISC IrDA bit so an IR beam (same UART in IrDA
         * mode) is not mistaken for a wired serial sync.
         *
         * INVARIANT: nothing on the host may write UART0 while the bridge is active
         * — the console forwards every received byte into the emulated serial port,
         * so stray host writes get injected into a live HotSync session and hold it
         * open (this stranded the console during bring-up: console polling kept
         * resetting HotSync's receive wait, so UARTEN never cleared). */
        {
            extern dragonball_state_t db;
            /* Don't bridge for an IrDA beam or a loopback self-test (both reuse the
             * UART): those are not a wired serial session. */
            bool serial_open = (db.ustcnt & USTCNT_UARTEN) &&
                               !(db.umisc & (UMISC_IRDA_EN | UMISC_LOOP));
            /* ON is immediate (lossless — flips before the first byte). OFF is
             * debounced: revert only once the port has stayed closed for
             * SERIAL_OFF_DEBOUNCE_MS, so a mid-open UARTEN blip is absorbed instead
             * of thrashing bridge_enter/exit (baud + log-gag flips, dropped bytes).
             * A real close is sustained, so it still reverts after the window. */
            static bool      off_armed = false;
            static TickType_t off_at   = 0;
            static bool wifi_wanted = false;
            static uint32_t wifi_generation;
            if (serial_bridge_mode() == SERIAL_MODE_WIFI && wifi_generation != db_uart_generation()) {
                wifi_generation = db_uart_generation();
                wifi_wanted = off_armed = false;
            }
            if (serial_open) {
                off_armed = false;
                if (serial_bridge_mode() == SERIAL_MODE_WIFI) wifi_wanted = true;
                else serial_bridge_request(true);
            } else if (!off_armed) {
                off_armed = true;
                off_at = xTaskGetTickCount();
            } else if (xTaskGetTickCount() - off_at >= pdMS_TO_TICKS(SERIAL_OFF_DEBOUNCE_MS)) {
                if (serial_bridge_mode() == SERIAL_MODE_WIFI) {
                    wifi_wanted = false;
                    db_uart_flush_cpu_transport();
                } else serial_bridge_request(false);
            }
            if (serial_bridge_mode() == SERIAL_MODE_WIFI)
                wifi_gateway_request(wifi_wanted, wifi_generation);
        }

        if (serial_bridge_mode() == SERIAL_MODE_WIFI && wifi_gateway_ready()) {
            uint8_t serial_data[128];
            size_t room = wifi_gateway_write_space();
            if (room > sizeof(serial_data)) room = sizeof(serial_data);
            uint32_t n = db_uart_tx_pop(serial_data, (uint32_t)room);
            if (n) wifi_gateway_write(serial_data, n);
            /* Deliver at a serial-line rate per emulated millisecond (one batch).
             * Unpaced, a whole 536-byte TCP segment lands at once and overruns
             * Palm OS's software receive queue: the frame fails its FCS, the Palm
             * never ACKs, and every segment waits for a TCP retransmit. */
            static uint32_t rx_credit_x100;
            rx_credit_x100 += WIFI_RX_BYTES_PER_MS_X100;
            if (rx_credit_x100 > 64 * 100) rx_credit_x100 = 64 * 100;
            room = db_uart_rx_free();
            if (room > rx_credit_x100 / 100) room = rx_credit_x100 / 100;
            if (room > sizeof(serial_data)) room = sizeof(serial_data);
            n = room ? (uint32_t)wifi_gateway_read(serial_data, room) : 0;
            if (n) { db_uart_rx_push(serial_data, n); rx_credit_x100 -= n * 100; }
        }
        /* A drained TX ring must also reassert TX-ready interrupts. */
        db_uart_update_irq();
        m68k_set_irq(&cpu, db_interrupt_pending_level());
        if (db_uart_rx_ring_nonempty()) {
            extern dragonball_state_t db;
            if (db.ustcnt & USTCNT_RXEN)
                cpu.regs.stopped = false;
        }

        /* PDB/PRC auto-install. Driven by the shared installer_pump (the same
         * driver the host harness uses): it waits for the OS to settle (boot->UI
         * framebuffer switch), functionally probes the dynamic heap, then installs
         * one buffered database per batch (a full emulation batch runs between
         * calls so the OS commits each). Source files are left in /PalmOS/install;
         * databases already in restored RAM are kept. Runs on the cpu task — the sole writer of
         * CPU state — like the pen/key injection above. */
        palm_card_pump(&cpu);
        if (g_install.phase == INST_WAIT || g_install.phase == INST_RUN) {
            installer_pump_result_t pr = installer_pump(
                &g_install.set, &cpu, installer_os_ready(),
                db_lcd_framebuffer_addr(), g_install.force, &g_install.pump);
            if (pr.attempted) {
                g_install.phase = INST_RUN;
                g_install.last  = pr.attempted->result;
                if (pr.attempted->status == IMG_SKIPPED) {
                    g_install.skipped++;
                    ESP_LOGI(TAG, "install %s: existing RAM database kept", pr.attempted->name);
                } else if (pr.attempted->status == IMG_INSTALLED) {
                    g_install.ok++;
                    ESP_LOGI(TAG, "install %s ('%s'): %s", pr.attempted->name,
                             pr.attempted->dbname, installer_result_str(pr.attempted->result));
                } else {
                    g_install.fail++;
                    ESP_LOGW(TAG, "install %s ('%s'): %s (giving up at step %d, dmErr=0x%04X)",
                             pr.attempted->name, pr.attempted->dbname,
                             installer_result_str(pr.attempted->result),
                             g_installer_last_fail_step, g_installer_last_dm_err);
                }
            }
            if (pr.status == 1) {
                ESP_LOGI(TAG, "auto-install complete: %d ok, %d kept, %d failed%s",
                         g_install.ok, g_install.skipped, g_install.fail,
                         pr.need_warm_reset ? " — warm reset" : "");
                /* A reset-after-install DB supersedes the launcher kick (the warm
                 * reboot repaints everything); both run on this cpu task. */
                if (pr.need_warm_reset)        g_warm_reset_request = true;
                else if (pr.need_launcher_kick) installer_refresh_launcher(&cpu);
                g_install.phase = INST_DONE;
                g_install.force = false;
            }
        }

        /* Console-requested backup / restore (one-shot). Serviced here on the cpu
         * task — the sole CPU-state writer — and BEFORE the warm-reset check so a
         * restored reset-after-install DB is rebooted the same loop. The console
         * publishes arg then req (with a barrier); we acquire req, barrier, snapshot
         * arg into a local (so the op can't read a torn/clobbered shared arg), run,
         * then clear req LAST — holding the busy-lock for the whole op. */
        int br_req = g_backup.req;
        if (br_req != BR_REQ_NONE) {
            __sync_synchronize();                       /* acquire: pair with console release */
            char name[sizeof(g_backup.arg)];
            snprintf(name, sizeof(name), "%s", g_backup.arg);
            run_sd_op(br_req, name[0] ? name : NULL);
            g_backup.req = BR_REQ_NONE;                 /* release the busy-lock */
        }

        /* Warm reset request (install reset-after-install, or the `reset` console
         * command). Serviced here on the cpu task between batches. */
        if (g_warm_reset_request) {
            do_warm_reset();
            g_warm_reset_request = false;
        }

        /* [sleep diag] Log PLL-off sleep transitions so on-device serial reveals
         * whether/how often the device wakes during deep sleep and what is
         * pending at the wake edge (IPR identifies the source). Edge-triggered,
         * checked once per batch — negligible overhead. */
        {
            static bool was_asleep = false;
            bool now_asleep = db_is_asleep();
            if (now_asleep != was_asleep) {
                extern dragonball_state_t db;
                ESP_LOGI(TAG, "SLEEP %s PC=%08lX IPR=%08lX IMR=%08lX cyc=%lu",
                         now_asleep ? "ENTER" : "WAKE",
                         (unsigned long)cpu.regs.pc, (unsigned long)db.ipr,
                         (unsigned long)db.imr, (unsigned long)total_cycles);
                was_asleep = now_asleep;
            }
        }

        /* If the emulated device is in PLL-off sleep, power the ESP32 down into
         * light sleep until the power button is held. Blocks here until wake. */
        maybe_save_ram();
        maybe_enter_light_sleep();

        if (total_cycles - last_log_cycles > cycles_per_batch * 2000) {
            uint32_t cyc_delta = total_cycles - last_log_cycles;
            last_log_cycles = total_cycles;
            TickType_t now_tick = xTaskGetTickCount();
            double secs = (double)((now_tick - meas_last_tick) * portTICK_PERIOD_MS) / 1000.0;
            uint32_t idle_pct = meas_sb_total ? (meas_sb_idle * 100 / meas_sb_total) : 0;
            uint32_t fb = db_lcd_framebuffer_addr();
            extern dragonball_state_t db;
            if (idle_pct >= 90) {
                /* The emulated Palm is dozing (idle event loop / pre-sleep). The
                 * MHz figure then tracks the slow idle clock, not JIT throughput,
                 * so report idleness rather than a misleading "% of real". */
                ESP_LOGI(TAG, "EMU idle (%lu%% dozing) | PC=%08lX SR=%04X cyc=%lu FB=%08lX",
                         (unsigned long)idle_pct, (unsigned long)cpu.regs.pc, cpu.regs.sr,
                         (unsigned long)total_cycles, (unsigned long)fb);
            } else {
                double mhz = secs > 0 ? (double)cyc_delta / secs / 1e6 : 0;
                ESP_LOGI(TAG, "EMU %.2f MHz (target %.2f, %.0f%% of real) idle=%lu%% | PC=%08lX SR=%04X cyc=%lu FB=%08lX",
                         mhz, CPU_MHZ, mhz / CPU_MHZ * 100.0, (unsigned long)idle_pct,
                         (unsigned long)cpu.regs.pc, cpu.regs.sr,
                         (unsigned long)total_cycles, (unsigned long)fb);
            }
            meas_last_tick = now_tick;
            meas_sb_total = 0;
            meas_sb_idle = 0;
        }

        /* Yield ~1ms only every ~250ms of wall-clock, not after every ~16k-cycle
         * batch. The CPU task is pinned to core 1 alone, so this just lets the
         * core-1 idle task (task watchdog) and any housekeeping run. A per-batch
         * 1ms delay previously spent ~1/3 of all wall-clock time sleeping at our
         * ~8 MHz raw speed; even a 25ms cadence cost ~4%. 250ms keeps the idle
         * task fed well within the 5s task-watchdog window while making the
         * yield tax negligible (~0.4%). */
        TickType_t now_y = xTaskGetTickCount();
        if ((now_y - last_yield_tick) >= pdMS_TO_TICKS(250)) {
            last_yield_tick = now_y;
            vTaskDelay(1);
        }
    }
}

static void input_task(void *param)
{
    ESP_LOGI(TAG, "Input task started on core %d", xPortGetCoreID());
    for (;;) {
        if (esp32_powersave_requested) {
            xEventGroupSetBits(park_eg, PARK_INPUT);
            while (esp32_powersave_requested) vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        input_poll();
        /* 10 ms => ~100 Hz, matching the Palm IIIx's native digitizer sampling
         * (the ROM re-samples the pen on its 100 Hz Timer-1 system tick). The
         * GT911/I2C cost per poll is sub-millisecond and runs on core 0, off the
         * CPU-bound core. (FreeRTOS tick is 1 kHz here, so 10 ms is exact.) */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* --- Battery: read the real M5Paper pack and report it as the Palm battery ---
 *
 * The M5Paper measures its battery on GPIO35 (ADC1 channel 7) through a 1:2
 * resistive divider, so the true pack voltage is 2x the pin voltage. We map that
 * onto the 12-bit ADS7846 battery sample the ROM reads (level = sample>>4, dead
 * below ~125). The window below is an honest Li-ion map: a genuinely flat pack
 * lands below the shutdown threshold so Palm OS runs its own low-battery path. */
#if !CONFIG_BOARD_JC4827W543
#define BAT_ADC_CHANNEL  ADC_CHANNEL_7   /* GPIO35 on ADC1 */
#define BAT_MV_FULL      4200            /* ~100% -> level ~255 (sample 0xFF0) */
#define BAT_MV_EMPTY     3300            /* dead  -> level ~110 (< 125 thresh)  */
#define BAT_LEVEL_FULL   255
#define BAT_LEVEL_EMPTY  110
#define BAT_ADC_SAMPLES  8               /* averaged per reading */

static adc_oneshot_unit_handle_t bat_adc;
static adc_cali_handle_t         bat_cali;
static bool                      bat_cali_ok;

/* Convert a true pack voltage (mV) to the 12-bit ADS7846 sample (level<<4). */
static uint16_t battery_mv_to_sample(int mv)
{
    int level;
    if (mv >= BAT_MV_FULL)       level = BAT_LEVEL_FULL;
    else if (mv <= BAT_MV_EMPTY) level = BAT_LEVEL_EMPTY;
    else level = BAT_LEVEL_EMPTY + (mv - BAT_MV_EMPTY) *
                 (BAT_LEVEL_FULL - BAT_LEVEL_EMPTY) / (BAT_MV_FULL - BAT_MV_EMPTY);
    int sample = level << 4;             /* ROM reads level = sample >> 4 */
    if (sample > 0x0FFF) sample = 0x0FFF;
    if (sample < 0)      sample = 0;
    return (uint16_t)sample;
}

static void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit_cfg, &bat_adc) != ESP_OK) {
        ESP_LOGW(TAG, "battery: ADC unit init failed; using default sample");
        bat_adc = NULL;
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,     /* full ~0..3.1V at the pin */
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(bat_adc, BAT_ADC_CHANNEL, &chan_cfg);

    /* The ADC calibration scheme is target-specific: the classic ESP32 has only
     * line fitting; newer parts (e.g. the ESP32-P4) use curve fitting. Pick
     * whichever the target's esp_adc provides. */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = BAT_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    bat_cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &bat_cali) == ESP_OK);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    bat_cali_ok = (adc_cali_create_scheme_line_fitting(&cali_cfg, &bat_cali) == ESP_OK);
#else
    bat_cali_ok = false;
#endif
    if (!bat_cali_ok)
        ESP_LOGW(TAG, "battery: ADC calibration unavailable; readings approximate");
}

static void battery_task(void *param)
{
    battery_init();
    if (!bat_adc) vTaskDelete(NULL);   /* no ADC: leave the default sample */

    for (;;) {
        if (esp32_powersave_requested) {     /* don't read across light sleep */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int raw_sum = 0, ok = 0;
        for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
            int raw;
            if (adc_oneshot_read(bat_adc, BAT_ADC_CHANNEL, &raw) == ESP_OK) {
                raw_sum += raw; ok++;
            }
        }
        if (ok) {
            int raw = raw_sum / ok;
            int pin_mv = raw;
            if (bat_cali_ok) adc_cali_raw_to_voltage(bat_cali, raw, &pin_mv);
            int pack_mv = pin_mv * 2;                  /* undo the 1:2 divider */
            uint16_t sample = battery_mv_to_sample(pack_mv);
            db_spi_set_battery_sample(sample);
            ESP_LOGI(TAG, "battery: %d mV -> sample 0x%03X (level %d)",
                     pack_mv, sample, sample >> 4);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));   /* battery moves slowly */
    }
}

#endif

/* How often the display task checks Palm's framebuffer for changes. This is the
 * change-DETECTION rate, decoupled from the actual e-ink push rate: a check is a cheap
 * ~3 KB memcmp, while pushes are paced by the panel's waveform via it8951_wait_ready()
 * inside region_load(). Polling fast keeps the quiescence settle in display_refresh()
 * cheap (it costs ~one poll of latency) so a settled redraw appears ~60 ms after Palm
 * stops drawing instead of ~200 ms, while still never showing a half-drawn frame. */
#if CONFIG_BOARD_JC4827W543
#define DISPLAY_POLL_MS 33
#else
#define DISPLAY_POLL_MS 60
#endif

static void display_task(void *param)
{
    ESP_LOGI(TAG, "Display task waiting for framebuffer...");

    /* Service a waveform benchmark even before the OS programs the framebuffer:
     * the IT8951 + silkscreen are already up (display_init/display_draw_static in
     * app_main), so the timing run does not need a booted Palm. */
    while (db_lcd_framebuffer_addr() == 0) {
        if (g_wave_benchmark_request) {
            display_run_waveform_benchmark();
            g_wave_benchmark_request = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Framebuffer at 0x%08lX, starting display updates",
             (unsigned long)db_lcd_framebuffer_addr());

    bool disp_was_on = true;
    for (;;) {
        if (esp32_powersave_requested) {
            xEventGroupSetBits(park_eg, PARK_DISPLAY);
            while (esp32_powersave_requested) vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (g_wave_benchmark_request) {
            display_run_waveform_benchmark();   /* runs on this task = sole SPI owner */
            g_wave_benchmark_request = false;
            continue;
        }
        bool on = g_display_enabled;
        if (on) {
            if (!disp_was_on) display_resume_after_wake();  /* re-enabled: clean full repaint */
            if (!ram_status_latched) display_refresh();
        } else if (disp_was_on) {
            display_blank_for_sleep();   /* disabled: blank the panel once, then idle (no wear) */
        }
        disp_was_on = on;
        /* Poll fast while the panel is live (cheap memcmp; pushes self-pace on the
         * waveform). When the panel is disabled there is nothing to detect, so idle
         * slowly. */
        vTaskDelay(pdMS_TO_TICKS(on ? DISPLAY_POLL_MS : 200));
    }
}

void app_main(void)
{
    /* Reserve the JIT's executable cache first, while the internal D/IRAM (SRAM1)
     * region is still unfragmented — later display/SD/bus allocations otherwise
     * split it, shrinking the cache and emulation speed. Codegen runs only once
     * the cpu task samples a hot PC, long after this, so the ROM need not be loaded
     * yet. The 68k->Xtensa burst JIT is interpreter-primary; set g_m68k_accel_mode
     * = 0 for the pure interpreter. */
#if CONFIG_M68K_JIT
    g_m68k_accel_mode = 1;
#else
    g_m68k_accel_mode = 0;
#endif
#if !CONFIG_BOARD_JC4827W543
    if (g_m68k_accel_mode)
        m68k_jit_init();
#endif

    esp_err_t err = display_init();
    bool display_ok = (err == ESP_OK);
#if CONFIG_BOARD_JC4827W543
    ram_progress_available = display_ok;
#endif
    if (!display_ok)
        ESP_LOGW(TAG, "Display init failed: %s (continuing without display)",
                 esp_err_to_name(err));

    ESP_LOGI(TAG, "=== Dragonfruit on %s ===", BOARD_NAME);

#if CONFIG_BOARD_JC4827W543
    /* Touch first: the boot menu needs it. Its calibration (on-screen, saved
     * in NVS, else the touch.* keys of /PalmOS/palm.cfg) serves both systems. */
    err = input_init();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Input init failed: %s", esp_err_to_name(err));
    if (display_ok && boot_menu_run() == BOOT_SYSTEM_CYBIKO) {
        cybiko_app_start();
        return;
    }
    if (display_ok) display_ui_fill(0, 0, BOARD_PANEL_W, BOARD_PANEL_H, 0xffff);
    /* On this board the JIT cache is reserved only once Palm OS is chosen, so
     * a Cybiko boot keeps that internal RAM. */
    if (g_m68k_accel_mode)
        m68k_jit_init();
#endif
    ESP_LOGI(TAG, "Starting Palm IIIx emulator");

    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Establish the screen layout before anything is drawn or any touch is
     * mapped. Read the optional /PalmOS/palm.cfg off the SD (it shares the display SPI
     * bus, so this must run after display_init); fall back to the stock portrait
     * layout if the card/file is absent or the config is invalid. The layout is
     * set even when the display failed, because input.c still maps touches
     * through it. Then draw the static silkscreen for the active layout. */
#if CONFIG_BOARD_JC4827W543
#if CONFIG_PALM_WIFI_GATEWAY
    int serial_mode = SERIAL_MODE_WIFI;
#else
    int serial_mode = SERIAL_MODE_CONSOLE;
#endif
#else
    int serial_mode = SERIAL_MODE_AUTO;
#endif
    int serial_baud = 57600;       /* the Palm HotSync default; PC end must match */
    {
        uint16_t pw = BOARD_PANEL_W, ph = BOARD_PANEL_H;
        if (display_ok) display_get_panel_size(&pw, &ph);

        static char cfg_text[LAYOUT_CFG_MAX];
        size_t cfg_len = 0;
        config_load_result_t cr = config_load_from_sd(cfg_text, sizeof(cfg_text), &cfg_len);
        if (cr == CONFIG_OK)
            layout_load(cfg_text, pw, ph);      /* logs applied/invalid itself */
        else
            layout_use_default(pw, ph);

        /* Same palm.cfg also carries the serial-bridge settings (see
         * serial_bridge.h): serial.mode = auto|console|passthrough, serial.baud. */
        if (cr == CONFIG_OK) {
            char v[24];
            if (config_get_value(cfg_text, "state.enabled", v, sizeof v))
                ram_state_enabled = !strcmp(v, "1");
#if CONFIG_BOARD_JC4827W543
            if (config_get_value(cfg_text, "card.enabled", v, sizeof v))
                card_enabled = !strcmp(v, "1");
            if (config_get_value(cfg_text, "card.read_only", v, sizeof v))
                card_read_only = !strcmp(v, "1");
#endif
            if (config_get_value(cfg_text, "sound.volume", v, sizeof v)) {
                long vol = strtol(v, NULL, 10);
                if (vol >= 0 && vol <= 100) sound_volume = (uint8_t)vol;
                else ESP_LOGW(TAG, "sound.volume '%s' out of range; using %u", v, sound_volume);
            }
            if (config_get_value(cfg_text, "serial.mode", v, sizeof v)) {
                int m = serial_bridge_parse_mode(v);
                if (m >= 0) serial_mode = m;
                else ESP_LOGW(TAG, "serial.mode '%s' invalid; using auto", v);
            }
            if (config_get_value(cfg_text, "serial.baud", v, sizeof v)) {
                long b = strtol(v, NULL, 0);
                if (b >= 300 && b <= 921600) serial_baud = (int)b;
                else ESP_LOGW(TAG, "serial.baud '%s' out of range; using %d", v, serial_baud);
            }
        }

        if (display_ok) display_draw_static();
    }

    /* The ROM lives on the SD card (/PalmOS/palm.rom), not embedded in the firmware.
     * Load it into PSRAM; on any failure show why on the e-ink and halt — there
     * is nothing to emulate without a ROM. The SD shares the display's SPI bus,
     * so this runs after display_init() and unmounts before any task starts. */
    uint8_t *rom_data = NULL;
    size_t   rom_size = 0;
    rom_load_result_t lr = rom_load_from_sd(&rom_data, &rom_size);
    if (lr != ROM_LOAD_OK) {
        const char *msg;
        switch (lr) {
        case ROM_LOAD_NO_SD:
            msg = "NO SD CARD\n\nINSERT A FAT\nSD CARD WITH\n/PALMOS/\nPALM.ROM\nAND RESET";
            break;
        case ROM_LOAD_NO_FILE:
            msg = "PALM.ROM\nNOT FOUND\n\nCOPY ROM TO\n/PALMOS/\nPALM.ROM\nAND RESET";
            break;
        default:
            msg = "BAD PALM.ROM\n\nNOT A VALID\nIIIX ROM\n\nREPLACE AND\nRESET";
            break;
        }
        ESP_LOGE(TAG, "ROM load failed (%d) - halting", (int)lr);
        if (display_ok)
            display_show_message(msg);
        return;   /* no tasks started -> device sits on the message until reset */
    }
    ESP_LOGI(TAG, "ROM loaded from SD: %u bytes", (unsigned)rom_size);

    /* Allocate the larger Palm RAM before buffering optional install images. */
    bus_init(rom_data, rom_size);
    if (!bus_ram_ptr() || !bus_vram_ptr()) {
        ESP_LOGE(TAG, "Palm memory allocation failed");
        if (display_ok) display_show_message("NOT ENOUGH\nMEMORY");
        return;
    }
    ram_state_init(&ram_state, ram_state_crc(rom_data, rom_size), BUS_RAM_SIZE);
    ram_state.yield = ram_state_yield;
    ram_state.progress = ram_progress_available ? ram_progress_update : NULL;
    snprintf(ram_state_prefix, sizeof(ram_state_prefix), PALM_SD_DIR "/palm-%08lX-%uK-ram",
             (unsigned long)ram_state.rom_crc, BUS_RAM_SIZE / 1024);
    if (ram_state_enabled) {
        ram_progress_begin(true);
        sdmmc_card_t *card = NULL;
        ram_state_result_t r = RAM_STATE_IO;
        if (m5_sd_mount(&card) == ESP_OK) {
            r = ram_state_load(&ram_state, ram_state_prefix, bus_ram_ptr());
            m5_sd_unmount(card);
        }
        g_ram_last_result = r;
        ram_restored = r == RAM_STATE_OK;
        ESP_LOGI(TAG, "RAM restore: %s, %s", ram_state_result_str(r), ram_state_prefix);
        if (r == RAM_STATE_IO || r == RAM_STATE_INVALID) {
            /* Do not replace a possibly recoverable save with this cold boot. */
            ram_state_enabled = false;
            ESP_LOGW(TAG, "RAM save disabled this session to preserve unreadable checkpoints");
        }
        if (!ram_restored) {
            memset(bus_ram_ptr(), 0, BUS_RAM_SIZE);
            memcpy(bus_ram_ptr(), rom_data, 8);
        }
        if (ram_progress_available) {
            display_show_message(ram_restored ? "RAM RESTORED\n\nSTARTING PALM" :
                r == RAM_STATE_MISSING ? "NO SAVED RAM\n\nSTARTING NEW" : "RESTORE ERROR\nSAVE KEPT\nSTARTING NEW");
            vTaskDelay(pdMS_TO_TICKS(r == RAM_STATE_OK || r == RAM_STATE_MISSING ? 600 : 2500));
        }
    }
    /* Source files remain on SD; preserve existing databases on restored RAM. */
    installer_scan_sd(&g_install.set);
    g_install.pump.keep_existing = ram_restored;
    g_install.phase = g_install.set.count ? INST_WAIT : INST_IDLE;
    g_installer_yield = installer_yield_cb;

    /* The volume set in the boot menu, once set, wins over palm.cfg. */
    uint8_t saved_volume;
    if (audio_saved_volume(&saved_volume)) sound_volume = saved_volume;
    err = audio_init(sound_volume);
    if (err == ESP_OK)
        db_pwm_set_sink(audio_tone);
    else
        ESP_LOGW(TAG, "Audio init failed: %s", esp_err_to_name(err));

    dragonball_init();

    cpup = heap_caps_calloc(1, sizeof(m68k_cpu_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!cpup) { ESP_LOGE(TAG, "CPU alloc fail"); return; }

    m68k_init(&cpu);
#if CONFIG_BOARD_JC4827W543
    if (card_enabled && m5_sd_mount(&palm_sd_card) == ESP_OK) {
        if (palm_card_init(PALM_SD_DIR "/palm-card", card_read_only)) {
            palm_card_space = card_space;
            cpu.palm_trap_callback = palm_card_trap;
        } else {
            ESP_LOGW(TAG, "Virtual Palm card unavailable; check /PalmOS/palm-card directory");
            m5_sd_unmount(palm_sd_card);
            palm_sd_card = NULL;
        }
    }
#endif
    cpu.irq_callback = db_interrupt_pending_level;
    cpu.tick_callback = db_timer_advance;
    cpu.wake_callback = db_iwr_wake_pending;
    cpu.vector_base_callback = db_int_vector_base;
    cpu.resume_callback = db_cpu_resume;
    if (!ram_restored) setup_exception_vectors();

    uint32_t init_ssp = bus_read32(0x00000000);
    uint32_t reset_pc = bus_read32(0x00000004);

    cpu.regs.sr = SR_S | SR_I2 | SR_I1 | SR_I0;
    cpu.regs.a[7] = init_ssp;
    cpu.regs.ssp = init_ssp;
    cpu.regs.pc = reset_pc;
    if (ram_restored) m68k_warm_reset(&cpu);

    ESP_LOGI(TAG, "CPU: PC=%08lX SSP=%08lX",
             (unsigned long)cpu.regs.pc,
             (unsigned long)cpu.regs.a[7]);

#if !CONFIG_BOARD_JC4827W543
    err = input_init();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Input init failed: %s", esp_err_to_name(err));
#endif

    park_eg = xEventGroupCreate();
    if (!park_eg) ESP_LOGW(TAG, "park event group alloc failed; light sleep disabled");

    debug_console_start();
    serial_bridge_init(serial_mode, serial_baud);

    /* Task stacks come from internal RAM; say so if it ran out rather than
     * leaving a silent, frozen screen. */
    ESP_LOGI(TAG, "Free internal RAM before tasks: %u bytes (largest block %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (xTaskCreatePinnedToCore(cpu_task, "cpu", 16384, NULL, 5, NULL, 1) != pdPASS)
        ESP_LOGE(TAG, "cpu task: out of internal RAM");
    if (xTaskCreatePinnedToCore(display_task, "display", 8192, NULL, 3, NULL, 0) != pdPASS)
        ESP_LOGE(TAG, "display task: out of internal RAM");
    if (xTaskCreatePinnedToCore(input_task, "input", 4096, NULL, 4, NULL, 0) != pdPASS)
        ESP_LOGE(TAG, "input task: out of internal RAM");
#if !CONFIG_BOARD_JC4827W543
    xTaskCreatePinnedToCore(battery_task, "battery", 4096, NULL, 2, NULL, 0);
#else
    /* No verified battery divider on this board. Leave the emulated battery full. */
    db_spi_set_battery_sample(0xff0);
#endif

    ESP_LOGI(TAG, "Emulator running");
}
