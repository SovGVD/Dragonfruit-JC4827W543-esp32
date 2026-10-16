#include "debug_console.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"

#include "m68k.h"
#include "bus.h"
#include "dragonball.h"
#include "input.h"
#include "installer.h"
#include "backup.h"
#include "ram_state.h"
#include "serial_bridge.h"

/* CPU state lives in main.c; we only ever READ it here (racy across cores, but
 * fine for a diagnostic dump). Anything that MUTATES emulator/CPU state is
 * marshalled through the input queues, which the cpu_task drains — so the
 * console never writes CPU state off the cpu_task. */
extern m68k_cpu_t *cpup;
extern dragonball_state_t db;
extern volatile bool g_display_enabled;   /* main.c: panel-refresh gate (`disp on|off`) */
extern volatile bool g_wave_benchmark_request;   /* main.c: display task runs the `wavetime` benchmark */

/* PDB/PRC auto-install runtime (main.c). Read-only here (racy diagnostic dump),
 * except `force`, which the `install now` command sets for the cpu task. */
extern install_runtime_t g_install;
extern volatile bool g_warm_reset_request;   /* main.c: cpu task services a warm reset */

/* Backup/restore runtime (main.c). Read-only here (racy diagnostic dump) except
 * arg + the request booleans, which the `backup`/`restore` commands set for the
 * cpu task to service. */
extern backup_runtime_t g_backup;
extern volatile bool g_ram_save_request;
extern volatile unsigned g_ram_save_count;
extern volatile int g_ram_last_result;

static const char *TAG = "console";

#define CONSOLE_UART  CONFIG_ESP_CONSOLE_UART_NUM

/* All replies start with "= " so a host script can distinguish them from the
 * interleaved ESP_LOG output on the same UART. */
#define REPLY(fmt, ...)  printf("= " fmt "\n", ##__VA_ARGS__)

static void cmd_help(void)
{
    REPLY("OK commands:");
    REPLY("  tap X Y        inject pen-down at Palm coords (0..159)");
    REPLY("  penup          inject pen-up");
    REPLY("  key HEX [MOD]  EvtEnqueueKey(ascii=HEX, mod=MOD|default 0x8)");
    REPLY("  home           inject vchrLaunch (launcher)");
    REPLY("  power          awake: sleep (vchrHardPower); asleep: wake (matrix)");
    REPLY("  wake           pulse the matrix POWER key (KB-interrupt wake)");
    REPLY("  regs           dump CPU registers");
    REPLY("  irq            dump interrupt state (IMR/IPR/IWR/ISR/IVR/level)");
    REPLY("  uart           dump UART regs (USTCNT/UBAUD/UMISC) + bridge state");
    REPLY("  sleep          report PLL-off sleep state");
    REPLY("  fb             framebuffer address + checksum");
    REPLY("  disp on|off    enable/disable e-ink panel refresh (off = blank, spare wear)");
    REPLY("  wavetime       time each e-ink waveform mode (results stream as '= WAVE ...')");
    REPLY("  serrx HEX...   push bytes into the emulated UART RX ring");
    REPLY("  sertx          drain + hex-dump the emulated UART TX ring");
    REPLY("  serloop on|off internal UART loopback (TX -> RX)");
    REPLY("  serial [auto|console|passthrough|status]  USB<->Palm serial bridge");
    REPLY("  install [list|now|status]  PDB/PRC auto-install from /install");
    REPLY("  backup [NAME|status]       back up RAM DBs to SD /backup (NAME = one DB)");
    REPLY("  restore [NAME|status]      install DBs from SD /backup (NAME = one file)");
    REPLY("  state [save|status]       checkpoint RAM to SD (auto-restored at boot)");
    REPLY("  reset          warm-reset the emulated Palm (databases preserved)");
}

/* Reads of g_install below are racy against the cpu task mutating it
 * (status/dbname), but this is a diagnostic dump only — consistent with this
 * file's read-only-on-CPU-state convention; never writes installer state here. */
static void cmd_install(char *arg)
{
    static const char *KIND[]  = { "prc", "pdb", "pqa" };
    static const char *PHASE[] = { "idle", "waiting", "running", "done" };
    if (!arg || !strcmp(arg, "status")) {
        REPLY("OK phase=%s count=%d next=%d ok=%d kept=%d fail=%d last=%s",
              PHASE[g_install.phase & 3], g_install.set.count, g_install.set.next,
              g_install.ok, g_install.skipped, g_install.fail, installer_result_str(g_install.last));
    } else if (!strcmp(arg, "list")) {
        REPLY("OK %d image(s) queued:", g_install.set.count);
        for (int i = 0; i < g_install.set.count; i++) {
            install_image_t *im = &g_install.set.img[i];
            const char *st = im->status == IMG_INSTALLED ? "installed"
                           : im->status == IMG_FAILED    ? "failed"
                           : im->status == IMG_SKIPPED   ? "kept" : "pending";
            REPLY("  [%d] %s %u %s %s", i, im->name, (unsigned)im->size,
                  KIND[im->kind], st);
        }
    } else if (!strcmp(arg, "now")) {
        /* `force` skips the settle pre-gate (which waits for the launcher to draw over
         * the boot splash) — but NOT the UIInitialize floor or the heap probe. The
         * settle gate is a safety wait: installing while the OS is still in early
         * bring-up corrupts it (cumulatively, with >1 app). So `install now` is meant
         * for use once the launcher is on screen; forcing during the boot splash skips
         * that safety wait and can crash. Auto-install already waits for it. */
        if (g_install.phase == INST_WAIT || g_install.phase == INST_RUN) {
            g_install.force = true;
            REPLY("OK forcing install (skips settle wait; use only after launcher is up)");
        } else if (g_install.phase == INST_DONE) {
            REPLY("OK already complete");
        } else {
            REPLY("OK nothing queued");
        }
    } else {
        REPLY("ERR usage: install [list|now|status]");
    }
}

/* Trim leading AND trailing spaces/tabs in place; returns the advanced start. The
 * trailing trim matters for `backup <name>`: a stray trailing space would otherwise
 * make the exact name match fail and silently back up nothing. */
static char *trim(char *s)
{
    if (!s) return s;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
    return s;
}

/* Report current backup/restore phase + counts (racy diagnostic read of g_backup,
 * consistent with this file's convention). Shared by `backup status`/`restore
 * status`. */
static void cmd_br_status(void)
{
    static const char *PH[] = { "idle", "running", "done" };
    REPLY("OK phase=%s ok=%d fail=%d skipped=%d last=%s",
          PH[g_backup.phase % 3], g_backup.ok, g_backup.fail, g_backup.skipped,
          backup_result_str(g_backup.last));
}

/* `backup`/`restore [NAME|status]` — request the op (kind = BR_REQ_BACKUP/RESTORE).
 * NAME is the rest of the line (may contain spaces): it targets a single DB (backup)
 * or /backup file (restore); bare = all. The handoff mirrors db_uart_rx_push: publish
 * arg, __sync_synchronize() barrier, THEN set req (the cpu task acquires req, barriers,
 * snapshots arg). A non-zero req is the busy-lock, so a second command in flight is
 * rejected rather than clobbering the shared arg. */
static void cmd_br(int kind, char *arg)
{
    arg = trim(arg);
    if (arg && !strcmp(arg, "status")) { cmd_br_status(); return; }
    if (g_backup.req != BR_REQ_NONE) { REPLY("ERR busy"); return; }
    snprintf(g_backup.arg, sizeof(g_backup.arg), "%s", (arg && arg[0]) ? arg : "");
    g_backup.ok = g_backup.fail = g_backup.skipped = 0;
    g_backup.phase = BR_IDLE;
    __sync_synchronize();                       /* release: arg visible before req */
    g_backup.req = kind;
    const char *what = (kind == BR_REQ_RESTORE) ? "restore" : "backup";
    if (arg && arg[0]) REPLY("OK %s requested for '%s'", what, arg);
    else REPLY("OK %s requested (%s)", what,
               (kind == BR_REQ_RESTORE) ? "all of /backup" : "backup-flagged + PIM RAM DBs");
}

/* Pulse the matrix POWER key: assert, hold briefly so the ROM's KB ISR scans
 * it, release. This is the only thing that wakes a STOPped CPU (the KB
 * interrupt). db_kbd_set is already called cross-core from the input task, so
 * doing it here follows the existing pattern. */
static void power_matrix_pulse(void)
{
    db_kbd_set(KBD_KEY_POWER, true);
    vTaskDelay(pdMS_TO_TICKS(60));
    db_kbd_set(KBD_KEY_POWER, false);
}

static void cmd_regs(void)
{
    m68k_regs_t r = cpup->regs;   /* snapshot */
    REPLY("OK pc=%08lX sr=%04X stopped=%d pend=%d",
          (unsigned long)r.pc, r.sr, r.stopped, r.pending_irq);
    REPLY("d=%08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
          (unsigned long)r.d[0], (unsigned long)r.d[1], (unsigned long)r.d[2],
          (unsigned long)r.d[3], (unsigned long)r.d[4], (unsigned long)r.d[5],
          (unsigned long)r.d[6], (unsigned long)r.d[7]);
    REPLY("a=%08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
          (unsigned long)r.a[0], (unsigned long)r.a[1], (unsigned long)r.a[2],
          (unsigned long)r.a[3], (unsigned long)r.a[4], (unsigned long)r.a[5],
          (unsigned long)r.a[6], (unsigned long)r.a[7]);
}

static void cmd_irq(void)
{
    REPLY("OK imr=%08lX ipr=%08lX iwr=%08lX isr=%08lX ivr=%02X level=%d",
          (unsigned long)db.imr, (unsigned long)db.ipr, (unsigned long)db.iwr,
          (unsigned long)db.isr, db.ivr, db_interrupt_pending_level());
}

static void cmd_fb(void)
{
    uint32_t fb = db_lcd_framebuffer_addr();
    uint32_t cksum = 0;
    if ((fb - BUS_SED1375_BASE) < BUS_SED1375_VRAM_SIZE) {
        uint32_t off = fb - BUS_SED1375_BASE;
        int bpp = db_lcd_bpp();
        uint32_t nbytes = (uint32_t)(160 * 160 * bpp / 8);
        uint8_t *vram = bus_vram_ptr();
        if (vram && off + nbytes <= BUS_SED1375_VRAM_SIZE) {
            for (uint32_t i = 0; i < nbytes; i++)
                cksum += vram[off + i];
        }
    } else if (fb && fb + 160 * 160 / 8 <= BUS_RAM_SIZE) {   /* whole 3200-byte FB in range */
        uint8_t *ram = bus_ram_ptr();
        for (int i = 0; i < 160 * 160 / 8; i++)   /* 160x160 1bpp = 3200 bytes */
            cksum += ram[fb + i];
    }
    REPLY("OK fb=%08lX cksum=%08lX", (unsigned long)fb, (unsigned long)cksum);
}

/* Stream the 160x160 1bpp Palm framebuffer as hex (one "FB:" line per 20-byte row)
 * so a host script can reconstruct/render the screen for autonomous UI testing. */
static void cmd_fbdump(void)
{
    uint32_t fb = db_lcd_framebuffer_addr();
    if ((fb - BUS_SED1375_BASE) < BUS_SED1375_VRAM_SIZE) {
        uint32_t off = fb - BUS_SED1375_BASE;
        int bpp = db_lcd_bpp();
        uint32_t nbytes = (uint32_t)(160 * 160 * bpp / 8);
        uint8_t *vram = bus_vram_ptr();
        if (!vram || off + nbytes > BUS_SED1375_VRAM_SIZE) { REPLY("ERR no framebuffer"); return; }
        REPLY("FBDUMP fb=%08lX w=160 h=160 stride=20", (unsigned long)fb);
        for (int row = 0; row < 160; row++) {
            char line[2 * 20 + 1];
            for (int b = 0; b < 20; b++) {
                uint8_t packed = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int x = b * 8 + bit;
                    uint8_t idx = 0;
                    if (bpp == 8) idx = vram[off + row * 160 + x];
                    else if (bpp == 4) idx = (vram[off + row * 80 + (x >> 1)] >> ((1 - (x & 1)) * 4)) & 0x0F;
                    else if (bpp == 2) idx = (vram[off + row * 40 + (x >> 2)] >> ((3 - (x & 3)) * 2)) & 0x03;
                    else idx = (vram[off + row * 20 + (x >> 3)] >> (7 - (x & 7))) & 0x01;
                    if (db.sed_lut_rgb565[idx] != 0xFFFF)
                        packed |= (uint8_t)(1u << (7 - bit));
                }
                snprintf(line + 2 * b, 3, "%02X", packed);
            }
            printf("FB:%s\n", line);
        }
        REPLY("FBEND");
        return;
    }
    if (!fb || fb + 160 * 20 > BUS_RAM_SIZE) { REPLY("ERR no framebuffer"); return; }
    uint8_t *ram = bus_ram_ptr();
    REPLY("FBDUMP fb=%08lX w=160 h=160 stride=20", (unsigned long)fb);
    for (int row = 0; row < 160; row++) {
        char line[2 * 20 + 1];
        for (int b = 0; b < 20; b++)
            snprintf(line + 2 * b, 3, "%02X", ram[fb + row * 20 + b]);
        printf("FB:%s\n", line);
    }
    REPLY("FBEND");
}

static void dispatch(char *line)
{
    /* Strip trailing CR/LF/space. */
    size_t n = strlen(line);
    while (n && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' '))
        line[--n] = '\0';

    char *cmd = strtok(line, " \t");
    if (!cmd)
        return;   /* blank line */

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "tap") == 0) {
        char *sx = strtok(NULL, " \t"), *sy = strtok(NULL, " \t");
        if (!sx || !sy) { REPLY("ERR usage: tap X Y"); return; }
        int x = (int)strtol(sx, NULL, 0), y = (int)strtol(sy, NULL, 0);
        input_inject_pen((int16_t)x, (int16_t)y, false);
        REPLY("OK pen down (%d,%d)", x, y);
    } else if (strcmp(cmd, "penup") == 0) {
        input_inject_pen(-1, -1, true);
        REPLY("OK pen up");
    } else if (strcmp(cmd, "key") == 0) {
        char *sa = strtok(NULL, " \t"), *sm = strtok(NULL, " \t");
        if (!sa) { REPLY("ERR usage: key HEX [MOD]"); return; }
        uint16_t ascii = (uint16_t)strtol(sa, NULL, 16);
        uint16_t mod = sm ? (uint16_t)strtol(sm, NULL, 16) : 0x0008 /*commandKeyMask*/;
        input_inject_key(ascii, 0, mod);
        REPLY("OK key %04X mod %04X queued", ascii, mod);
    } else if (strcmp(cmd, "home") == 0) {
        input_inject_key(0x0108 /*vchrLaunch*/, 0, 0x0008);
        REPLY("OK home queued");
    } else if (strcmp(cmd, "power") == 0) {
        if (db_is_asleep()) {
            power_matrix_pulse();
            REPLY("OK power: matrix wake pulse");
        } else {
            input_inject_key(0x0208 /*vchrHardPower*/, 0, 0x0008);
            REPLY("OK power: vchrHardPower queued (sleep)");
        }
    } else if (strcmp(cmd, "wake") == 0) {
        power_matrix_pulse();
        REPLY("OK matrix POWER pulse");
    } else if (strcmp(cmd, "regs") == 0) {
        cmd_regs();
    } else if (strcmp(cmd, "irq") == 0) {
        cmd_irq();
    } else if (strcmp(cmd, "uart") == 0) {
        REPLY("OK ustcnt=%04X ubaud=%04X umisc=%04X | bridge=%s active=%d",
              db.ustcnt, db.ubaud, db.umisc,
              serial_bridge_mode_str(serial_bridge_mode()), serial_bridge_active());
    } else if (strcmp(cmd, "sleep") == 0) {
        REPLY("OK asleep=%d pllcr=%04X", db_is_asleep(), db.pllcr);
    } else if (strcmp(cmd, "fb") == 0) {
        cmd_fb();
    } else if (strcmp(cmd, "fbdump") == 0) {
        cmd_fbdump();
    } else if (strcmp(cmd, "disp") == 0) {
        char *s = strtok(NULL, " \t");
        bool on = s && strcmp(s, "on") == 0;
        g_display_enabled = on;
        REPLY("OK display %s", on ? "on" : "off (blanked)");
    } else if (strcmp(cmd, "wavetime") == 0) {
        /* The display task (sole IT8951 owner) runs the benchmark and streams
         * the per-mode "= WAVE ..." results; we just request it. */
        g_wave_benchmark_request = true;
        REPLY("OK waveform benchmark queued (results follow as '= WAVE ...')");
    } else if (strcmp(cmd, "serrx") == 0) {
        if (serial_bridge_mode() == SERIAL_MODE_WIFI) { REPLY("ERR UART owned by PPP"); return; }
        uint8_t buf[64];
        int n = 0;
        char *tok;
        while (n < (int)sizeof(buf) && (tok = strtok(NULL, " \t")) != NULL)
            buf[n++] = (uint8_t)strtol(tok, NULL, 16);
        if (!n) { REPLY("ERR usage: serrx HEX [HEX...]"); return; }
        db_uart_rx_push(buf, n);
        REPLY("OK pushed %d byte(s) to UART RX", n);
    } else if (strcmp(cmd, "sertx") == 0) {
        if (serial_bridge_mode() == SERIAL_MODE_WIFI) { REPLY("ERR UART owned by PPP"); return; }
        uint8_t buf[64];
        uint32_t n = db_uart_tx_pop(buf, sizeof(buf));
        char hex[64 * 3 + 1];
        int p = 0;
        for (uint32_t i = 0; i < n; i++)
            p += sprintf(hex + p, "%02X ", buf[i]);
        hex[p] = '\0';
        REPLY("OK UART TX %lu byte(s): %s", (unsigned long)n, hex);
    } else if (strcmp(cmd, "serloop") == 0) {
        if (serial_bridge_mode() == SERIAL_MODE_WIFI) { REPLY("ERR UART owned by PPP"); return; }
        char *s = strtok(NULL, " \t");
        bool on = s && strcmp(s, "on") == 0;
        db_uart_set_loopback(on);
        REPLY("OK loopback %s", on ? "on" : "off");
    } else if (strcmp(cmd, "serial") == 0) {
        char *s = strtok(NULL, " \t");
        if (!s || strcmp(s, "status") == 0) {
            REPLY("OK serial mode=%s active=%d", serial_bridge_mode_str(serial_bridge_mode()),
                  serial_bridge_active());
        } else {
            int m = serial_bridge_parse_mode(s);
            if (m < 0) { REPLY("ERR usage: serial [auto|console|passthrough|status]"); return; }
            if (m == SERIAL_MODE_WIFI || serial_bridge_mode() == SERIAL_MODE_WIFI) {
                REPLY("ERR set serial.mode in palm.cfg and restart to change Wi-Fi transport"); return;
            }
            /* Reply (and flush it out at the console baud) BEFORE the bridge can
             * park this task / switch UART0 to the sync baud. */
            REPLY("OK serial mode=%s", serial_bridge_mode_str(m));
            fflush(stdout);
            serial_bridge_set_mode(m);
        }
    } else if (strcmp(cmd, "install") == 0) {
        cmd_install(strtok(NULL, " \t"));
    } else if (strcmp(cmd, "backup") == 0) {
        cmd_br(BR_REQ_BACKUP, strtok(NULL, ""));   /* rest of line = optional DB name */
    } else if (strcmp(cmd, "restore") == 0) {
        cmd_br(BR_REQ_RESTORE, strtok(NULL, ""));  /* rest of line = optional /backup filename */
    } else if (strcmp(cmd, "state") == 0) {
        char *arg = strtok(NULL, " \t");
        if (arg && !strcmp(arg, "save")) {
            g_ram_save_request = true;
            REPLY("OK RAM save queued; waits for Palm idle (requires state.enabled=1)");
        } else if (!arg || !strcmp(arg, "status")) {
            REPLY("OK RAM=%u KB pending=%d saves=%u last=%s", BUS_RAM_SIZE / 1024,
                  g_ram_save_request, g_ram_save_count, ram_state_result_str(g_ram_last_result));
        } else REPLY("ERR usage: state [save|status]");
    } else if (strcmp(cmd, "reset") == 0) {
        /* Request a warm reset; the cpu task (sole CPU-state writer) services it. */
        g_warm_reset_request = true;
        REPLY("OK warm reset requested");
    } else {
        REPLY("ERR unknown command '%s' (try 'help')", cmd);
    }
}

/* The console task is the SINGLE owner of UART0 reads. In passthrough it forwards
 * raw bytes to the emulated UART; otherwise it assembles command lines. This is
 * why the serial bridge never reads UART0 itself (and never suspends this task):
 * two tasks blocking on the driver's rx_mux deadlocked the bridge. */
static void console_task(void *arg)
{
    (void)arg;
    char    line[128];
    int     len = 0;
    uint8_t buf[64];
    ESP_LOGI(TAG, "debug console ready on UART%d — type 'help'", CONSOLE_UART);
    for (;;) {
        bool pass = serial_bridge_active();

        /* In passthrough, read at most what the emulated RX ring can accept so
         * db_uart_rx_push never drops; the rest stays buffered in the driver ring
         * (our flow control on a 3-wire link). */
        uint32_t want = sizeof buf;
        if (pass) {
            uint32_t freeb = db_uart_rx_free();
            if (freeb == 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }  /* ring full; let CPU drain */
            if (freeb < want) want = freeb;
        }

        int n = uart_read_bytes(CONSOLE_UART, buf, want, pdMS_TO_TICKS(50));
        if (n <= 0) continue;

        if (pass) {
            db_uart_rx_push(buf, (uint32_t)n);   /* wire -> emulated serial port */
            len = 0;                              /* drop any partial console line */
            continue;
        }

        for (int i = 0; i < n; i++) {            /* console mode: assemble + dispatch lines */
            char c = (char)buf[i];
            if (c == '\r' || c == '\n') { line[len] = '\0'; if (len) dispatch(line); len = 0; }
            else if (len < (int)sizeof(line) - 1) line[len++] = c;
        }
    }
}

void debug_console_start(void)
{
    /* Route stdout through the interrupt-driven UART driver so printf()/REPLY
     * reach the wire; the console reads RX directly via uart_read_bytes (see
     * console_task), so no stdin/line-ending setup is needed. ESP_LOG shares this
     * driver — log lines and "= " replies interleave but never corrupt. */
    const uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* RX buffer is generous (2 KB) so a HotSync burst can sit in the driver ring
     * while the serial bridge meters it into the emulated UART (db_uart_rx_free
     * back-pressure) — the console itself only ever needs a line. */
    uart_driver_install(CONSOLE_UART, 2048, 0, 0, NULL, 0);
    uart_param_config(CONSOLE_UART, &uart_config);
    uart_vfs_dev_use_driver(CONSOLE_UART);

    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 2, NULL, 0);
}
