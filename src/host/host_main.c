#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
#include <direct.h>
#define HOST_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define HOST_MKDIR(p) mkdir((p), 0777)
#endif

#include "m68k.h"
#include "bus.h"
#include "dragonball.h"
#include "installer.h"
#include "backup.h"
#include "layout.h"
#include "host_ui.h"

/* Host harness for the emulator core: boots a ROM from the host filesystem and
 * runs the SAME components/ code as the firmware, with ESP-IDF stubbed out (see
 * esp_shim.h). Two modes:
 *   headless (default) — run to a cycle limit, dumping screen.png on every
 *                        framebuffer change. Deterministic, for fast iteration.
 *   --ui               — interactive SDL3 window (requires -DHOST_UI): renders
 *                        the display + silkscreen through the shared layout
 *                        engine and feeds mouse input through as pen events.
 * Both share host_step() so the install/backup pumps behave identically. */

m68k_cpu_t *cpup;
#define cpu (*cpup)

/* PDB/PRC images queued via -i <file>; installed once the OS boots (no SD on the
 * host, so files come from the host filesystem — only the install engine + the
 * shared installer_pump driver are exercised). */
static install_set_t host_install_set;
static installer_pump_state_t host_pump_state;
static int host_inst_done = 0;

/* -b <dir>: once the OS is up, dump card-0 RAM databases to <dir> as .prc/.pdb
 * files via the shared export engine (no flag filter — useful for serializer
 * round-trip testing). Exercises backup_enumerate + backup_serialize_db without
 * hardware. */
static const char *host_backup_dir = NULL;
static int host_backup_done = 0;

static volatile int running = 1;
static uint32_t total_cycles = 0;

static void sigint_handler(int sig) { (void)sig; running = 0; }

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

/* --- PNG snapshot (grayscale, self-contained: no zlib) --- */

static uint32_t png_crc_table[256];
static int png_crc_done = 0;
static uint32_t png_crc(uint32_t crc, const uint8_t *buf, size_t len)
{
    if (!png_crc_done) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            png_crc_table[n] = c;
        }
        png_crc_done = 1;
    }
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = png_crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t png_adler32(const uint8_t *buf, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + buf[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void png_be32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v>>24), (uint8_t)(v>>16), (uint8_t)(v>>8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    png_be32(f, len);
    uint8_t *tmp = (uint8_t *)malloc(4 + len);
    memcpy(tmp, type, 4);
    if (len) memcpy(tmp + 4, data, len);
    fwrite(tmp, 1, 4 + len, f);
    png_be32(f, png_crc(0, tmp, 4 + len));
    free(tmp);
}

/* Dump the live framebuffer to a PNG (3x scale). Reads the LCD bpp / stride /
 * dimensions from the active controller (SED1375 RGB color or DragonBall
 * grayscale), so it tracks whatever depth/palette the ROM has programmed. */
static void save_bmp(const char *filename)   /* writes PNG; name kept for callers */
{
    uint32_t fb = db_lcd_framebuffer_addr();
    if (!fb) return;

    extern dragonball_state_t db;
    bool color = db_lcd_is_color();
    int width = color ? 160 : (db.lxmax + 1);
    if (width == 0) width = 160;
    int height = color ? 160 : (db.lymax + 1);
    if (height == 0) height = 160;
    int bpp = db_lcd_bpp();
    int stride = color ? (width * bpp / 8) : (db.lvpw ? db.lvpw * 2 : width * bpp / 8);
    int maxval = (1 << bpp) - 1;

    const uint8_t *src = NULL;
    if (color) {
        uint8_t *vram = bus_vram_ptr();
        uint32_t off = fb - BUS_SED1375_BASE;
        if (!vram || (size_t)off + (size_t)height * stride > BUS_SED1375_VRAM_SIZE) return;
        src = vram + off;
    } else {
        if (fb >= BUS_RAM_SIZE || (size_t)fb + (size_t)height * stride > BUS_RAM_SIZE) return;
        src = bus_ram_ptr() + fb;
    }

    int scale = 3;
    int img_w = width * scale;
    int img_h = height * scale;
    int ch = color ? 3 : 1;

    /* Raw image: each output row prefixed with a 0 (None) filter byte. */
    size_t line_bytes = (size_t)img_w * ch;
    size_t raw_stride = line_bytes + 1;
    size_t raw_size = raw_stride * img_h;
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    uint8_t *line = (uint8_t *)malloc(line_bytes);
    size_t p = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t byte = src[y * stride + x * bpp / 8];
            int idx;
            if (bpp == 8) {
                idx = byte;
            } else if (bpp == 4) {
                idx = (byte >> ((1 - (x & 1)) * 4)) & 0xF;
            } else if (bpp == 2) {
                idx = (byte >> ((3 - (x & 3)) * 2)) & 3;
            } else {
                idx = (byte >> (7 - (x & 7))) & 1;
            }
            if (color) {
                const uint8_t *rgb = db.sed_lut_rgb[idx & 0xFF];
                for (int sx = 0; sx < scale; sx++) {
                    uint8_t *dst = line + (x * scale + sx) * 3;
                    dst[0] = rgb[0]; dst[1] = rgb[1]; dst[2] = rgb[2];
                }
            } else {
                uint8_t pixel = (uint8_t)((maxval - idx) * 255 / maxval);
                for (int sx = 0; sx < scale; sx++)
                    line[x * scale + sx] = pixel;
            }
        }
        for (int sy = 0; sy < scale; sy++) {
            raw[p++] = 0;
            memcpy(raw + p, line, line_bytes);
            p += line_bytes;
        }
    }
    free(line);

    /* zlib stream using uncompressed (stored) deflate blocks. */
    size_t zcap = 2 + raw_size + ((raw_size / 65535) + 1) * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(zcap);
    size_t zp = 0;
    z[zp++] = 0x78; z[zp++] = 0x01;            /* zlib header */
    size_t off = 0;
    do {
        size_t blk = raw_size - off;
        if (blk > 65535) blk = 65535;
        int final = (off + blk >= raw_size);
        z[zp++] = (uint8_t)final;              /* BFINAL, BTYPE=00 stored */
        z[zp++] = (uint8_t)(blk & 0xFF);
        z[zp++] = (uint8_t)(blk >> 8);
        uint16_t nlen = (uint16_t)~blk;
        z[zp++] = (uint8_t)(nlen & 0xFF);
        z[zp++] = (uint8_t)(nlen >> 8);
        if (blk) { memcpy(z + zp, raw + off, blk); zp += blk; }
        off += blk;
    } while (off < raw_size);
    uint32_t ad = png_adler32(raw, raw_size);
    z[zp++] = ad>>24; z[zp++] = ad>>16; z[zp++] = ad>>8; z[zp++] = (uint8_t)ad;
    free(raw);

    FILE *png = fopen(filename, "wb");
    if (!png) { free(z); return; }

    static const uint8_t sig[8] = { 137,80,78,71,13,10,26,10 };
    fwrite(sig, 1, 8, png);

    uint8_t ihdr[13];
    ihdr[0]=img_w>>24; ihdr[1]=img_w>>16; ihdr[2]=img_w>>8; ihdr[3]=(uint8_t)img_w;
    ihdr[4]=img_h>>24; ihdr[5]=img_h>>16; ihdr[6]=img_h>>8; ihdr[7]=(uint8_t)img_h;
    ihdr[8]=8;                   /* bit depth */
    ihdr[9]=color ? 2 : 0;       /* color type 2 = RGB, 0 = grayscale */
    ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
    png_chunk(png, "IHDR", ihdr, 13);
    png_chunk(png, "IDAT", z, (uint32_t)zp);
    png_chunk(png, "IEND", NULL, 0);

    fclose(png);
    free(z);
    fprintf(stderr, "Saved %s\n", filename);
}

/* Read a .prc/.pdb/.pqa from the host filesystem into a malloc'd buffer and queue
 * it for auto-install. Takes ownership of the buffer on success (the set frees
 * it); frees it and returns false on any open/size/read error or queue rejection.
 * Host-local on purpose: the device path is installer_scan_sd (PSRAM allocs off
 * the SD), but the host has no SD and uses plain malloc, so the alloc source
 * differs and there is nothing to share beyond installer_add_buffer itself. */
static bool host_queue_install(install_set_t *set, const char *path)
{
    FILE *inf = fopen(path, "rb");
    if (!inf) { fprintf(stderr, "cannot open -i file: %s\n", path); return false; }
    fseek(inf, 0, SEEK_END); long isz = ftell(inf); fseek(inf, 0, SEEK_SET);
    if (isz <= 0) { fprintf(stderr, "empty -i file: %s\n", path); fclose(inf); return false; }
    uint8_t *buf = malloc((size_t)isz);
    size_t rd = buf ? fread(buf, 1, (size_t)isz, inf) : 0;
    fclose(inf);
    if (!buf || rd != (size_t)isz) { free(buf); fprintf(stderr, "read failed: %s\n", path); return false; }

    const char *bn = path, *s;
    if ((s = strrchr(path, '/'))  != NULL) bn = s + 1;
    if ((s = strrchr(bn, '\\')) != NULL) bn = s + 1;
    if (!installer_add_buffer(set, bn, buf, (size_t)isz)) {
        free(buf);
        fprintf(stderr, "install queue rejected %s (bad ext / too big / full)\n", bn);
        return false;
    }
    fprintf(stderr, "queued install: %s (%ld bytes)\n", bn, isz);
    return true;
}

/* Dump every non-ROM card-0 database to host_backup_dir via the shared exporter.
 * Runs once, after the OS is up. Mirrors the device backup_run_to_sd loop, minus
 * the SD mount and the backup-flag filter (we dump all RAM DBs for testing). Uses
 * the shared backup_file_sink + backup_dedupe so two DBs whose names sanitize to
 * the same file don't silently overwrite each other. */
static void host_run_backup(void)
{
    static backup_db_info_t dbs[BACKUP_MAX_DBS];
    static char used[BACKUP_MAX_DBS][BACKUP_NAME_MAX];
    int n = backup_enumerate(&cpu, dbs, BACKUP_MAX_DBS);
    if (n < 0) { fprintf(stderr, "backup: enumerate failed (OS not ready)\n"); return; }
    HOST_MKDIR(host_backup_dir);

    int ok = 0, fail = 0, skip = 0, nused = 0;
    for (int i = 0; i < n; i++) {
        if (dbs[i].romBased) { skip++; continue; }
        char fname[BACKUP_NAME_MAX];
        backup_make_filename(&dbs[i], fname, sizeof(fname));
        backup_dedupe(used, nused, fname);
        if (nused < BACKUP_MAX_DBS) snprintf(used[nused++], BACKUP_NAME_MAX, "%s", fname);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", host_backup_dir, fname);
        FILE *bf = fopen(path, "wb");
        if (!bf) { fprintf(stderr, "backup: cannot create %s\n", path); fail++; continue; }
        backup_sink_t sink = { backup_file_sink, bf };
        backup_result_t r = backup_serialize_db(&cpu, &dbs[i], &sink);
        fclose(bf);
        if (r == BACKUP_OK) {
            ok++;
            fprintf(stderr, "backup: '%s' -> %s\n", dbs[i].name, path);
        } else {
            fail++;
            remove(path);
            fprintf(stderr, "backup: '%s' FAILED: %s\n", dbs[i].name, backup_result_str(r));
        }
    }
    fprintf(stderr, "backup: %d ok, %d failed, %d skipped(ROM) -> %s\n",
            ok, fail, skip, host_backup_dir);
}

/* Advance the emulator one batch and run the install/backup pumps the host owns.
 * Returns the cycles advanced. Shared by the headless loop and the UI step
 * callback so -i/-b behave identically in both modes. */
static int host_step(m68k_cpu_t *c, void *ctx)
{
    (void)ctx;
    const int batch = 4000;

    m68k_execute(c, batch);
    if (!c->regs.stopped)
        db_timer_advance(batch);
    total_cycles += batch;
    m68k_set_irq(c, db_interrupt_pending_level());

    /* PDB/PRC auto-install (host): drive the SAME pump the device uses. It waits for
     * the OS to settle (the launcher drawing over the boot splash) + a functional heap
     * probe, then installs one queued image per loop and reports completion. */
    if (!host_inst_done && host_install_set.count) {
        installer_pump_result_t pr = installer_pump(
            &host_install_set, c, installer_os_ready(),
            db_lcd_framebuffer_addr(), false, &host_pump_state);
        if (pr.attempted) {
            if (pr.attempted->status == IMG_FAILED)
                fprintf(stderr, "[%uM] install %s ('%s'): %s (failed at step %d, dmErr=0x%04X)\n",
                        total_cycles / 1000000, pr.attempted->name, pr.attempted->dbname,
                        installer_result_str(pr.attempted->result),
                        g_installer_last_fail_step, g_installer_last_dm_err);
            else
                fprintf(stderr, "[%uM] install %s ('%s'): %s\n",
                        total_cycles / 1000000, pr.attempted->name, pr.attempted->dbname,
                        installer_result_str(pr.attempted->result));
        }
        if (pr.status == 1) {
            if (pr.need_warm_reset) {
                /* Exercise the warm reset on the host too (device parity). */
                dragonball_init();
                m68k_warm_reset(c);
                m68k_set_irq(c, db_interrupt_pending_level());
                fprintf(stderr, "[%uM] auto-install complete (warm reset)\n",
                        total_cycles / 1000000);
            } else {
                if (pr.need_launcher_kick) installer_refresh_launcher(c);
                fprintf(stderr, "[%uM] auto-install complete\n", total_cycles / 1000000);
            }
            host_inst_done = 1;
        }
    }

    /* -b backup: once the OS is up, dump RAM databases to the host dir ONCE. If an
     * install was queued, wait for it to finish (so a known DB is present);
     * otherwise just wait for the OS to settle. Gated on a heap probe, like the
     * device backup, so we never drive traps before MemInit. */
    if (host_backup_dir && !host_backup_done) {
        bool ready = host_install_set.count
                     ? host_inst_done
                     : (backup_os_ready() && total_cycles > 75000000u);
        if (ready && installer_probe_heap(c)) {
            host_run_backup();
            host_backup_done = 1;
        }
    }

    return batch;
}

/* Establish the layout the UI window renders through. "upright" (default) is the
 * desktop-friendly ROT_0 stack; "default" is the device-exact rotated panel; any
 * other value is treated as a path to a palm.cfg loaded against the device panel. */
static void host_setup_layout(const char *which)
{
    if (strcmp(which, "default") == 0) {
        layout_use_default(960, 540);
    } else if (strcmp(which, "upright") == 0) {
        layout_use_default_upright(480, 660);
    } else {
        FILE *cf = fopen(which, "rb");
        if (!cf) {
            fprintf(stderr, "cannot open --layout file '%s'; using upright\n", which);
            layout_use_default_upright(480, 660);
            return;
        }
        static char cfg[LAYOUT_CFG_MAX];
        size_t n = fread(cfg, 1, sizeof(cfg) - 1, cf);
        /* Reject (don't silently truncate) a config larger than the buffer: a
         * clipped file would parse as if complete, and layout_load's own
         * len>=LAYOUT_CFG_MAX guard can never fire once we've truncated it. */
        bool too_big = (n == sizeof(cfg) - 1) && (fgetc(cf) != EOF);
        fclose(cf);
        if (too_big) {
            fprintf(stderr, "--layout file '%s' exceeds %d bytes; using upright\n",
                    which, LAYOUT_CFG_MAX - 1);
            layout_use_default_upright(480, 660);
            return;
        }
        cfg[n] = '\0';
        layout_load(cfg, 960, 540);   /* cfg presets target the device panel */
    }
}

int main(int argc, char **argv)
{
    const char *rom_path = "Palm-IIIx-3.1.rom";
    int max_cycles = 150000000;
    bool ui = false;
    const char *layout_arg = "upright";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 && i+1 < argc)
            rom_path = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i+1 < argc)
            max_cycles = atoi(argv[++i]);
        else if (strcmp(argv[i], "-i") == 0 && i+1 < argc)
            host_queue_install(&host_install_set, argv[++i]);   /* repeatable */
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc)
            host_backup_dir = argv[++i];                        /* dump RAM DBs to <dir> */
        else if (strcmp(argv[i], "--ui") == 0)
            ui = true;                                          /* interactive SDL window */
        else if (strcmp(argv[i], "--layout") == 0 && i+1 < argc)
            layout_arg = argv[++i];                             /* upright|default|<cfg> */
    }

    FILE *f = fopen(rom_path, "rb");
    if (!f) { fprintf(stderr, "Cannot open ROM: %s\n", rom_path); return 1; }
    fseek(f, 0, SEEK_END);
    size_t rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom_data = malloc(rom_size);
    if (!rom_data) { fprintf(stderr, "ROM malloc failed\n"); return 1; }
    fread(rom_data, 1, rom_size, f);
    fclose(f);
    fprintf(stderr, "Loaded ROM: %s (%zu bytes)\n", rom_path, rom_size);

    bus_init(rom_data, rom_size);
    dragonball_init();

    cpup = calloc(1, sizeof(m68k_cpu_t));
    m68k_init(&cpu);
    cpu.irq_callback = db_interrupt_pending_level;
    cpu.tick_callback = db_timer_advance;
    cpu.wake_callback = db_iwr_wake_pending;
    cpu.vector_base_callback = db_int_vector_base;
    cpu.resume_callback = db_cpu_resume;

    setup_exception_vectors();

    uint32_t init_ssp = bus_read32(0x00000000);
    uint32_t reset_pc = bus_read32(0x00000004);

    cpu.regs.sr = SR_S | SR_I2 | SR_I1 | SR_I0;
    cpu.regs.a[7] = init_ssp;
    cpu.regs.ssp = init_ssp;
    cpu.regs.pc = reset_pc;

    signal(SIGINT, sigint_handler);
    fprintf(stderr, "Running: PC=%08X SSP=%08X max_cycles=%d%s\n",
            cpu.regs.pc, cpu.regs.ssp, max_cycles, ui ? " (UI)" : "");

    if (ui) {
#ifdef HOST_UI
        host_setup_layout(layout_arg);
        int rc = host_ui_run(&cpu, host_step, NULL);
        free(rom_data);
        free(cpup);
        return rc;
#else
        fprintf(stderr, "built without SDL: reconfigure with -DHOST_UI=ON to use --ui\n");
        free(rom_data);
        free(cpup);
        return 2;
#endif
    }

    /* Headless: run to the cycle limit, dumping screen.png on framebuffer change.
     * Use a 1M status interval near boot (25-55M, to catch the boot animation),
     * 5M otherwise. */
    uint32_t last_fb_cksum = 0;
    while (running && total_cycles < (uint32_t)max_cycles) {
        host_step(&cpu, NULL);

        uint32_t interval = (total_cycles >= 25000000 && total_cycles <= 55000000)
                            ? 1000000 : 5000000;
        if (total_cycles % interval == 0) {
            uint32_t fba = db_lcd_framebuffer_addr();
            const uint8_t *fb = NULL;
            uint32_t fb_bytes = (uint32_t)(160 * 160 * db_lcd_bpp() / 8);
            if (fba >= BUS_SED1375_BASE && fba + fb_bytes <= BUS_SED1375_BASE + BUS_SED1375_VRAM_SIZE) {
                fb = bus_vram_ptr() + (fba - BUS_SED1375_BASE);
            } else if (fba && fba + fb_bytes <= BUS_RAM_SIZE) {
                fb = bus_ram_ptr() + fba;
            }
            uint32_t cksum = 0;
            if (fb) {
                for (uint32_t i = 0; i < fb_bytes; i++) cksum = cksum * 33u + fb[i];
            }
            fprintf(stderr, "  %uM cyc: PC=%08X SR=%04X FB=%08X cksum=%08X irqs=%u\n",
                    total_cycles / 1000000, cpu.regs.pc, cpu.regs.sr,
                    fba, cksum, cpu.regs.irq_count);
            if (cksum != last_fb_cksum) {
                char name[64];
                snprintf(name, sizeof(name), "screen_%03uM.png", total_cycles / 1000000);
                save_bmp(name);
                last_fb_cksum = cksum;
            }
        }
    }

    fprintf(stderr, "\n=== Final: PC=%08X A7=%08X cycles=%u IRQs=%u ===\n",
            cpu.regs.pc, cpu.regs.a[7], total_cycles, cpu.regs.irq_count);
    save_bmp("screen.png");
    free(rom_data);
    free(cpup);
    return 0;
}
