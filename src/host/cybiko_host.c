/* Headless Cybiko Xtreme runner for checking the machine model on a PC with
 * the real ROM and flash dumps:
 *
 *   cybiko_host --rom cyrom150.bin --flash cyos_v1508.bin [options]
 *
 *   --seconds S        emulated seconds to run (default 20)
 *   --ramdisk FILE     load the RAM disk from FILE and save it back
 *   --disk-size KB     move the RAM disk to 800000 and make it KB kilobytes
 *                      (a multiple of 64), as the firmware does
 *   --install FILE     copy FILE onto the RAM disk before starting (repeatable)
 *   --ls               list the files on the RAM disk at the end
 *   --cart             plug in the MP3 player cartridge (--cart-log: and log it)
 *   --card IMG|DIR     ... with an SD card: a disk image (512-byte blocks) or a
 *                      folder, shown as a FAT16 card (writes stay in memory)
 *   --wav FILE         write what the cartridge plays as a 32 kHz mono WAV file
 *   --screenshot FILE  write the final screen as a PGM image
 *   --frames DIR       write DIR/frame_NNNN.pgm whenever the screen changes
 *                      (the other machines' as b_frame_NNNN.pgm, c_frame_NNNN.pgm)
 *   --frame-log        print the time each frame is written
 *   --pc-profile MS    from MS milliseconds on, sample the PC and list where
 *                      the CPU spends its time at the end
 *   --blit-fuzz N      (built with -DCY_HLE_CHECK) at the end, check N random
 *                      calls of the native ROM blit against the original
 *                      (checked every 100 ms of emulated time)
 *   --browse KEYS      (built with the browser, src/components/cy_browser) when
 *                      the Browser launcher opens the browser, press KEYS there
 *                      (a comma list of --press key names, or "type:TEXT") and
 *                      close it; its screens go to --frames too. Pages come
 *                      through the curl program
 *   --browse-url URL   the page the browser opens first (default its home)
 *   --gate             (built with the browser) a Cybiko Internet Gate (CyWIG)
 *                      on the radio for CyOS's WAP browser, fetching through
 *                      curl
 *   --gate-home URL    (after --gate) the page the WAP browser's Home opens
 *   --dump-ram FILE    write RAM at the end (the other machines' to FILE.b, FILE.c)
 *   --pair RAMDISK2    run a second machine (ID + 1) with its own RAM disk, on
 *                      the same radio; --press b:KEY@MS presses its keys
 *   --third RAMDISK3   (with --pair) a third machine (ID + 2), keys c:KEY@MS
 *   --chain            the second machine hears the first and the third, but
 *                      those two don't hear each other (radio out of range)
 *   --bot NICK         a radio-only chat partner built on cy_radio (no
 *                      emulated Cybiko): pings as NICK, prints what it hears
 *                      on the chat channel and acknowledges messages to it
 *   --bot-say MS:TEXT  ... at MS ms says TEXT to everyone in the General room
 *   --bot-room N       ... or in room N (cy_radio.h's CYR_ROOM_*)
 *   --bot-tell MS:ID:TEXT ... at MS ms sends TEXT privately to Cybiko ID (hex)
 *   --led-log          print each change of the status LED (red: sending,
 *                      green: hearing)
 *   --rf-log           print every radio packet sent (time, machine A or B,
 *                      channel, then the packet without its parity)
 *   --press KEY@MS[:D] hold KEY for D ms (default 200) from MS milliseconds; KEY is
 *                      a letter, f1-f7, enter, esc, space, up/down/left/right,
 *                      select, del, tab, ins, menu, shift, fn, help, dot, comma,
 *                      semi, excl, lparen, rparen, onoff, or rROW.BIT for
 *                      any matrix position
 *
 * Text on SCI1 (where the boot loader and CyOS print) and SCI2 is copied to
 * stdout; bytes on SCI0 go to stderr as hex. A summary with the speed and anything the model
 * saw but does not cover is printed at the end. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <ctype.h>
#include <libgen.h>
#include <sys/stat.h>
#include "cybiko.h"
#ifdef CY_CALLGRIND
#include <valgrind/callgrind.h>
#endif
#include "cy_mp3.h"
#include "cy_vfat.h"
#include "cy_radio.h"
#ifdef CY_WITH_BROWSER
#include "cybiko_browser.h"
#include "cyb_net_curl.h"
#endif

static const struct { const char *name; int key; } key_names[] = {
    { "a", CYK_A }, { "b", CYK_B }, { "c", CYK_C }, { "d", CYK_D }, { "e", CYK_E },
    { "f", CYK_F }, { "g", CYK_G }, { "h", CYK_H }, { "i", CYK_I }, { "j", CYK_J },
    { "k", CYK_K }, { "l", CYK_L }, { "m", CYK_M }, { "n", CYK_N }, { "o", CYK_O },
    { "p", CYK_P }, { "q", CYK_Q }, { "r", CYK_R }, { "s", CYK_S }, { "t", CYK_T },
    { "u", CYK_U }, { "v", CYK_V }, { "w", CYK_W }, { "x", CYK_X }, { "y", CYK_Y },
    { "z", CYK_Z }, { "f1", CYK_F1 }, { "f2", CYK_F2 }, { "f3", CYK_F3 },
    { "f4", CYK_F4 }, { "f5", CYK_F5 }, { "f6", CYK_F6 }, { "f7", CYK_F7 },
    { "enter", CYK_ENTER }, { "esc", CYK_ESC }, { "space", CYK_SPACE },
    { "up", CYK_UP }, { "down", CYK_DOWN }, { "left", CYK_LEFT }, { "right", CYK_RIGHT },
    { "select", CYK_SELECT }, { "del", CYK_DEL }, { "tab", CYK_TAB }, { "ins", CYK_INS },
    { "as", CYK_INS }, { "menu", CYK_MENU }, { "comma", CYK_COMMA }, { "semi", CYK_SEMI },
    { "excl", CYK_EXCL }, { "lparen", CYK_LPAREN }, { "rparen", CYK_RPAREN },
    { "shift", CYK_SHIFT }, { "fn", CYK_FN }, { "help", CYK_HELP }, { "dot", CYK_DOT },
    { "onoff", CYK_ONOFF },
};

/* --pc-profile: where the CPU spends its time, in 16-byte blocks of RAM
 * (400000-5fffff), the boot ROM or anywhere else. */
static uint32_t *pc_ram, pc_rom[0x40000 / 16], pc_other, pc_total;
static void pc_sample(uint32_t pc)
{
    if (!pc_ram) pc_ram = calloc(0x200000 / 16, sizeof *pc_ram);
    pc_total++;
    if (pc >= 0x400000 && pc < 0x600000) pc_ram[(pc - 0x400000) / 16]++;
    else if (pc < 0x40000) pc_rom[pc / 16]++;
    else pc_other++;
}
static void pc_report(void)
{
    fprintf(stderr, "PC profile, %u samples (%u outside RAM and ROM):\n", pc_total, pc_other);
    for (int k = 0; k < 40; k++) {
        uint32_t best = 0, at = 0;
        for (uint32_t i = 0; i < 0x200000 / 16; i++) if (pc_ram && pc_ram[i] > best) { best = pc_ram[i]; at = 0x400000 + i * 16; }
        for (uint32_t i = 0; i < 0x40000 / 16; i++) if (pc_rom[i] > best) { best = pc_rom[i]; at = i * 16; }
        if (!best) break;
        fprintf(stderr, "  %06x  %5.1f%%\n", (unsigned)at, 100.0 * best / pc_total);
        if (at >= 0x400000) pc_ram[(at - 0x400000) / 16] = 0; else pc_rom[at / 16] = 0;
    }
}

#define MAX_INSTALL 32
static const char *installs[MAX_INSTALL];
static int ninstalls;

static bool print_file(void *user, const cy_disk_file_t *f)
{
    (void)user;
    fprintf(stderr, "  %7u  %s\n", (unsigned)f->size, f->name);
    return true;
}

#define MAX_PRESSES 1024
/* who: 0 first machine, 1 second (--pair, KEY written b:KEY), 2 third
 * (--third, c:KEY), -1 all of them */
static struct { int key, who; uint64_t at, until; bool down; } presses[MAX_PRESSES];
static int npresses;

static int hex_port = -1;           /* --hex-port N: show that port's bytes as hex */
static cybiko_t *the_machine;

static void on_tx(void *user, int port, uint8_t b)
{
    (void)user;
    if (port == hex_port) {
        fprintf(stderr, "[%.3f sci%d %02x]\n", (double)the_machine->cpu.cycles / CY_CLOCK_HZ, port, b);
        return;
    }
    if (port == 1) { putchar(b); fflush(stdout); }
    else if (port == 0) fprintf(stderr, "[sci%d %02x]", port, b);
}

/* --trace-io LO-HI: count register accesses in [LO, HI] by PC, address and
 * direction, and list them at the end (for working out undocumented
 * peripherals such as the radio co-processor's port). */
#define TRACE_MAX 4096
static uint32_t trace_lo, trace_hi;
static struct { uint32_t pc, a; bool w; uint32_t n; uint8_t first, last; } traces[TRACE_MAX];
static int ntraces;
static void io_trace(cybiko_t *m, uint32_t a, uint8_t v, bool w)
{
    if (a < trace_lo || a > trace_hi) return;
    uint32_t pc = m->cpu.ppc;
    for (int i = 0; i < ntraces; i++)
        if (traces[i].pc == pc && traces[i].a == a && traces[i].w == w) {
            traces[i].n++;
            traces[i].last = v;
            return;
        }
    if (ntraces < TRACE_MAX)
        traces[ntraces++] = (typeof(traces[0])){ pc, a, w, 1, v, v };
}

/* --card IMG|DIR: the MP3 cartridge's SD card, a disk image or a folder */
static FILE *card_file;
static cy_vfat_t *card_dir;
static bool card_read(void *user, uint32_t block, uint8_t *buf)
{
    (void)user;
    if (card_dir) return cy_vfat_read(card_dir, block, buf);
    return fseek(card_file, (long)block * 512, SEEK_SET) == 0 && fread(buf, 1, 512, card_file) == 512;
}
static bool card_write(void *user, uint32_t block, const uint8_t *buf)
{
    (void)user;
    if (card_dir) return cy_vfat_write(card_dir, block, buf);
    return fseek(card_file, (long)block * 512, SEEK_SET) == 0 && fwrite(buf, 1, 512, card_file) == 512;
}

/* --cart-log: what the cartridge model sees */
static void on_cart_log(cybiko_t *m, const char *msg)
{
    fprintf(stderr, "[%9.4f] cart: %s\n", (double)m->cpu.cycles / CY_CLOCK_HZ, msg);
}

/* The cartridge's decoder, run in step with emulated time. */
#define WAV_RATE 32000
static uint64_t wav_samples;

static void put_le(FILE *f, uint32_t v, int n) { for (int i = 0; i < n; i++) fputc((int)(v >> (8 * i)) & 0xff, f); }

static void wav_header(FILE *f, uint32_t samples)
{
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); put_le(f, 36 + samples * 2, 4);
    fwrite("WAVEfmt ", 1, 8, f); put_le(f, 16, 4); put_le(f, 1, 2); put_le(f, 1, 2);
    put_le(f, WAV_RATE, 4); put_le(f, WAV_RATE * 2, 4); put_le(f, 2, 2); put_le(f, 16, 2);
    fwrite("data", 1, 4, f); put_le(f, samples * 2, 4);
}

static void mp3_pump(cybiko_t *m, cy_mp3_t *d, FILE *wav)
{
    uint64_t due = m->cpu.cycles * WAV_RATE / CY_CLOCK_HZ;
    static uint8_t in[4096];
    static int16_t pcm[1024];
    while (wav_samples < due) {
        size_t n = cybiko_mp3_take(m, in, cy_mp3_space(d));
        cy_mp3_feed(d, in, n);
        size_t k = due - wav_samples < 1024 ? (size_t)(due - wav_samples) : 1024;
        cy_mp3_render(d, pcm, k, WAV_RATE);
        if (wav) fwrite(pcm, 2, k, wav);
        wav_samples += k;
    }
}

/* --pair and --third: more machines on the same radio channel */
#define MAX_MACHINES 3
static cybiko_t *machines[MAX_MACHINES];
static bool chain;                  /* --chain: B hears A and C, A and C don't hear each other */

#ifdef CY_WITH_BROWSER
#include "cy_gate.h"
/* --gate: a virtual CyWIG on the radio */
static cy_gate_t *gate;

static void gate_rf(void *user, const uint8_t *pkt, size_t len, uint8_t ch)
{
    (void)user;
    if (machines[0] && !cybiko_rf_receive(machines[0], pkt, len, ch)) fprintf(stderr, "gate: radio queue full\n");
}

static void gate_log(void *user, const char *line)
{
    (void)user;
    fprintf(stderr, "[%.3f gate] %s\n", machines[0] ? (double)machines[0]->cpu.cycles / CY_CLOCK_HZ : 0.0, line);
}
#endif

static bool rf_log;                 /* --rf-log: print every radio packet */

/* --bot: a chat partner that only knows the radio protocol (cy_radio), the
 * way another ESP32 project would talk to Cybikos. It listens on the chat
 * channel only, like a radio tuned there. */
#define BOT_ID 0x4c0000b0u
#define MAX_BOT_LINES 16
static struct {
    bool on;
    char nick[9];
    int room;                        /* --bot-room: where --bot-say lines go */
    uint32_t full_reported;
    uint64_t next_ping_ms;
    uint8_t version;
    struct { uint64_t at; uint32_t to; char text[256]; bool done; } lines[MAX_BOT_LINES];
    int nlines;
    /* the line being sent: its packets and where we are */
    uint8_t out[8][CYR_LONG];
    int nout, sent, tries;
    uint32_t out_to;
    uint8_t number;
    bool waiting, querying;
    uint64_t retry_ms;
    int copies;                      /* broadcast copies still to send */
    cyr_rx_t rx;
    struct { uint32_t from, p0; } seen[16];
    int seen_next;
    uint32_t known[16];
    int nknown;
} bot;

static double now_s(void);

static void bot_send(const uint8_t *pkt, size_t len, uint8_t channel)
{
    for (int k = 0; k < MAX_MACHINES; k++)
        if (machines[k] && !cybiko_rf_receive(machines[k], pkt, len, channel)) {
            /* the queue fills while CyOS isn't listening (booting, radio off) */
            if (!(bot.full_reported++ & 63))
                fprintf(stderr, "[%.3f bot] machine %c isn't taking packets (queue full): %u lost so far\n",
                        now_s(), 'A' + k, (unsigned)bot.full_reported);
        }
}

static void bot_line(const char *what, uint32_t from, const cyr_msg_t *m)
{
    int room;
    const char *text;
    size_t tl;
    char id[8];
    cyr_cyid_str(from, id);
    if (cyr_chat_parse(m, &room, &text, &tl)) {
        for (int i = 0; i < 16; i++)
            if (bot.seen[i].from == from && bot.seen[i].p0 == m->p0) return;     /* a repeat */
        bot.seen[bot.seen_next].from = from;
        bot.seen[bot.seen_next].p0 = m->p0;
        bot.seen_next = (bot.seen_next + 1) % 16;
        fprintf(stderr, "[%.3f bot] %s chat line from %08x (%s), room %d%s: \"%.*s\"\n", now_s(), what,
                (unsigned)from, id, room, m->extra & 0x80 ? ", private" : "", (int)tl, text);
    } else {
        fprintf(stderr, "[%.3f bot] %s message %04x from %08x (%s) to %s%s%.0x, %u bytes: not chat, not handled\n",
                now_s(), what, m->id, (unsigned)from, id, m->name ? "process " : "port ",
                m->name ? m->name : "", m->name ? 0 : m->port, (unsigned)m->len);
    }
}

static void bot_rf_in(const uint8_t *pkt, size_t len, uint8_t channel)
{
    if (channel != CYR_RF_CHAT) return;                  /* tuned to the chat channel */
    if (!cyr_check(pkt, len)) {
        fprintf(stderr, "[%.3f bot] packet with a bad CRC dropped\n", now_s());
        return;
    }
    const uint32_t to = cyr_to(pkt), from = cyr_from(pkt);
    const uint8_t type = cyr_type(pkt);
    if (type == CYR_T_PING) {
        char nick[9];
        int age;
        bool female;
        bool known = false;
        for (int i = 0; i < bot.nknown; i++) known |= bot.known[i] == from;
        if (!known && bot.nknown < 16) {
            bot.known[bot.nknown++] = from;
            if (cyr_ping_details(pkt, nick, &age, &female))
                fprintf(stderr, "[%.3f bot] ping from %08x: %s, age %d, %s, on RF channel %u\n", now_s(),
                        (unsigned)from, nick, age, female ? "female" : "male", pkt[8] & 0x3f);
            else
                fprintf(stderr, "[%.3f bot] ping from %08x without details\n", now_s(), (unsigned)from);
        }
        return;
    }
    if (to != BOT_ID && to != CYR_BROADCAST) return;      /* for someone else */
    uint8_t ack[CYR_SHORT];
    if (type == CYR_T_ACK) {
        if (bot.waiting && from == bot.out_to && pkt[11] == bot.number && pkt[10] == bot.out[bot.sent][10]) {
            const int code = pkt[CYR_HEAD] >> 4;
            if (++bot.sent < bot.nout && code == CYR_ACK_TAKEN) {
                bot.tries = 0;
                bot.retry_ms = 0;                         /* next part now */
            } else if (code == CYR_ACK_GOT || code == CYR_ACK_AGAIN) {
                /* arrived but not handed over yet: ask until it is */
                fprintf(stderr, "[%.3f bot] private line to %08x: answer %d, asking if it was delivered\n",
                        now_s(), (unsigned)from, code);
                bot.sent--;
                bot.querying = true;
                bot.retry_ms = 0;
            } else {
                fprintf(stderr, "[%.3f bot] private line to %08x: answer %d (%s)\n", now_s(), (unsigned)from, code,
                        code == CYR_ACK_DELIVERED ? "delivered" : code == CYR_ACK_GOT ? "got it" :
                        code == CYR_ACK_NOBODY ? "nobody took it" : "other");
                bot.waiting = false;
            }
        }
        return;
    }
    if (type == CYR_T_QUERY) {
        if (to == BOT_ID) bot_send(ack, cyr_ack(ack, pkt, BOT_ID, CYR_RF_CHAT, CYR_ACK_DELIVERED), CYR_RF_CHAT);
        return;
    }
    if (type != CYR_T_MSG) return;
    uint8_t body[CYR_MSG_MAX];
    const int n = cyr_rx_feed(&bot.rx, pkt, len, body);
    if (to == BOT_ID && (pkt[9] & CYR_F_ACK_ME)) {
        const bool more = pkt[10] && n == 0;
        bot_send(ack, cyr_ack(ack, pkt, BOT_ID, CYR_RF_CHAT, more ? CYR_ACK_TAKEN : CYR_ACK_DELIVERED), CYR_RF_CHAT);
    }
    if (n < 0) fprintf(stderr, "[%.3f bot] part %u from %08x out of order: dropped\n", now_s(), pkt[10], (unsigned)from);
    if (n <= 0) return;
    cyr_msg_t m;
    if (!cyr_msg_parse(body, (size_t)n, &m)) {
        fprintf(stderr, "[%.3f bot] message from %08x not understood\n", now_s(), (unsigned)from);
        return;
    }
    bot_line(to == BOT_ID ? "private" : "heard", from, &m);
}

static void bot_poll(uint64_t ms)
{
    uint8_t pkt[CYR_SHORT];
    if (ms >= bot.next_ping_ms) {
        /* on the base channel for everyone, and on the chat channel; byte 8
         * says we are on the chat channel */
        cyr_ping(pkt, BOT_ID, CYR_RF_CHAT, bot.version, bot.nick, 20, false);
        bot_send(pkt, CYR_SHORT, CYR_RF_CHAT);
        bot_send(pkt, CYR_SHORT, CYR_RF_BASE);
        bot.next_ping_ms = ms + 3000;
    }
    if (!bot.waiting && !bot.copies) {
        for (int i = 0; i < bot.nlines; i++) {
            if (bot.lines[i].done || ms < bot.lines[i].at) continue;
            bot.lines[i].done = true;
            cyr_msg_t m;
            const bool priv = bot.lines[i].to != CYR_BROADCAST;
            cyr_chat(&m, bot.room, (uint32_t)ms, bot.lines[i].text, priv);
            m.len = strlen(bot.lines[i].text) + 1;     /* not cut to Chat's 99: tests longer messages */
            bot.out_to = bot.lines[i].to;
            if (priv && !++bot.number) bot.number = 1;
            bot.nout = cyr_msg_packets(&m, bot.out_to, BOT_ID, CYR_RF_CHAT, priv, priv ? bot.number : 0, bot.out, 8);
            bot.sent = bot.tries = 0;
            bot.querying = false;
            bot.retry_ms = 0;
            if (priv) bot.waiting = true;
            else bot.copies = 3;
            fprintf(stderr, "[%.3f bot] says \"%s\" to %08x in %d packet(s)\n", now_s(), bot.lines[i].text,
                    (unsigned)bot.out_to, bot.nout);
            break;
        }
    }
    if (bot.copies && ms >= bot.retry_ms) {
        /* Chat posts a line to everyone three times */
        for (int i = 0; i < bot.nout; i++) bot_send(bot.out[i], CYR_LONG, CYR_RF_CHAT);
        bot.copies--;
        bot.retry_ms = ms + 150;
    }
    if (bot.waiting && ms >= bot.retry_ms) {
        if (bot.tries++ == 6) {
            fprintf(stderr, "[%.3f bot] private line to %08x: no answer\n", now_s(), (unsigned)bot.out_to);
            bot.waiting = false;
            return;
        }
        if (bot.querying) bot_send(pkt, cyr_query(pkt, bot.out[bot.sent], CYR_RF_CHAT), CYR_RF_CHAT);
        else bot_send(bot.out[bot.sent], CYR_LONG, CYR_RF_CHAT);
        bot.retry_ms = ms + 400;
    }
}
static bool led_log;                /* --led-log: print status LED changes */
static void led_poll(const cybiko_t *m)
{
    static int shown = CY_LED_OFF;
    int led = cybiko_led(m);
    if (!led_log || led == shown) return;
    static const char *const names[] = { "off", "red", "green" };
    fprintf(stderr, "[%.3f] LED %s\n", (double)m->cpu.cycles / CY_CLOCK_HZ, names[led]);
    shown = led;
}
static bool frame_log;              /* --frame-log: print when each frame is written */

static void on_rf(void *user, const uint8_t *pkt, size_t len, uint8_t channel)
{
    int from = (int)(intptr_t)user;
    if (rf_log) {
        /* the 16-byte header, the data, then the Reed-Solomon parity */
        size_t data = len == CY_RF_SHORT ? 30 : 120;
        fprintf(stderr, "[%.3f rf %c ch%u]", machines[from] ? (double)machines[from]->cpu.cycles / CY_CLOCK_HZ : 0.0,
                'A' + from, channel);
        for (size_t i = 0; i < data; i++) fprintf(stderr, "%s%02x", i == 4 || i == 8 || i == 16 ? " " : "", pkt[i]);
        fprintf(stderr, "\n");
    }
    if (bot.on) bot_rf_in(pkt, len, channel);
    for (int to = 0; to < MAX_MACHINES; to++)
        if (to != from && machines[to] && (!chain || abs(to - from) == 1))
            cybiko_rf_receive(machines[to], pkt, len, channel);
#ifdef CY_WITH_BROWSER
    if (gate) cy_gate_rf_in(gate, pkt, len, channel);
#endif
}

static int64_t wall(void *user) { (void)user; return (int64_t)time(NULL); }

static double now_s(void) { return machines[0] ? (double)machines[0]->cpu.cycles / CY_CLOCK_HZ : 0.0; }

static bool browse_requested;

static uint32_t on_app_call(void *user, uint32_t fn, uint32_t arg)
{
    (void)user;
    fprintf(stderr, "[%.3f] app call %u (%u)\n", (double)the_machine->cpu.cycles / CY_CLOCK_HZ, (unsigned)fn, (unsigned)arg);
    if (fn == CY_APP_BROWSER) browse_requested = true;
    return fn == CY_APP_VERSION ? CY_APP_API : fn == CY_APP_BROWSER;
}

static uint8_t *load(const char *path, size_t want, size_t *got, bool exact)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    uint8_t *buf = calloc(1, want);
    size_t n = fread(buf, 1, want, f);
    fclose(f);
    if (exact && n != want) {
        fprintf(stderr, "%s: expected %zu bytes, got %zu\n", path, want, n);
        exit(1);
    }
    if (got) *got = n;
    return buf;
}

static void write_pgm(const char *path, const uint8_t *lum)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P5\n%d %d\n255\n", CY_LCD_W, CY_LCD_H);
    fwrite(lum, 1, CY_LCD_W * CY_LCD_H, f);
    fclose(f);
}

static int key_by_name(const char *s)
{
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (!strcasecmp(s, key_names[i].name)) return key_names[i].key;
    unsigned row, bit;          /* rROW.BIT: any matrix position */
    char end;
    if (sscanf(s, "r%u.%u%c", &row, &bit, &end) == 2 && row < 16 && bit < 16) return (int)CY_KEY(row, bit);
    return -1;
}

#ifdef CY_WITH_BROWSER
/* The browser, opened by the launcher: KEYS are pressed in it one after
 * another, each once the browser has nothing left to load, and it is
 * closed at the end if they did not close it. */
static void run_browser(cybiko_t *m, const uint8_t *flash, const char *keys, const char *url,
                        const char *frames, int *frame_no)
{
    static cyb_t *b;
    static cyb_net_t net;
    static uint32_t clock_ms;
    static bool url_done;
    uint8_t fb[CYB_FB_SIZE], saved[sizeof m->lcd.ram], lum[CY_LCD_W * CY_LCD_H];
    if (!b) {
        cyb_net_curl(&net, NULL);
        b = cyb_create(&net, flash, CY_FLASH_SIZE, NULL);
    }
    memcpy(saved, m->lcd.ram, sizeof saved);
    cyb_show(b);
    if (url && !url_done) { cyb_go(b, url); url_done = true; }
    char *list = strdup(keys ? keys : ""), *save = NULL;
    char *k = strtok_r(list, ",", &save);
    bool open = true;
    while (open) {
        for (int i = 0; open && (i < 3 || cyb_busy(b)) && i < 5000; i++) open = cyb_step(b, clock_ms += 20);
        if (cyb_render(b, fb)) {
            cybiko_lcd_show(m, fb);
            if (frames) {
                char path[512];
                cybiko_lcd_render(m, lum);
                snprintf(path, sizeof path, "%s/frame_%04d.pgm", frames, (*frame_no)++);
                write_pgm(path, lum);
            }
        }
        if (!open) break;
        if (!k) { fprintf(stderr, "browser: closed after the last key\n"); break; }
        if (!strncmp(k, "type:", 5)) {
            for (const char *c = k + 5; *c; c++) cyb_char(b, (unsigned char)*c);
        } else {
            int key = cybiko_browser_key(key_by_name(k));
            if (!key) fprintf(stderr, "browser: no key %s\n", k);
            cyb_key(b, key, true);
            if (key != CYB_K_SHIFT && key != CYB_K_FN) cyb_key(b, key, false);
        }
        k = strtok_r(NULL, ",", &save);
    }
    free(list);
    memcpy(m->lcd.ram, saved, sizeof saved);
    m->lcd.version++;
    fprintf(stderr, "[%.3f] browser closed\n", (double)m->cpu.cycles / CY_CLOCK_HZ);
}
#endif

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *flash_path = NULL, *ramdisk_path = NULL;
    const char *shot = NULL, *frames = NULL, *dump = NULL, *boot_path = NULL, *save_flash = NULL;
    double pc_profile_ms = -1;
#ifdef CY_HLE_CHECK
    int blit_fuzz = 0;
#endif
    const char *extra_path[MAX_MACHINES - 1] = { NULL };   /* --pair, --third */
    const char *pair_path = NULL;
    bool press_both = false;          /* --press keys go to the second machine too */
    uint32_t unit_id = 0;
    double seconds = 20;
    unsigned disk_kb = 0;
    bool ls = false, cart = false, cart_log = false;
    const char *card_path = NULL, *wav_path = NULL, *browse_keys = NULL, *browse_url = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom") && v) { rom_path = v; i++; }
        else if (!strcmp(a, "--flash") && v) { flash_path = v; i++; }
        else if (!strcmp(a, "--ramdisk") && v) { ramdisk_path = v; i++; }
        else if (!strcmp(a, "--boot") && v) { boot_path = v; i++; }
        else if (!strcmp(a, "--save-flash") && v) { save_flash = v; i++; }
        else if (!strcmp(a, "--seconds") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--screenshot") && v) { shot = v; i++; }
        else if (!strcmp(a, "--frames") && v) { frames = v; i++; }
        else if (!strcmp(a, "--pc-profile") && v) { pc_profile_ms = atof(v); i++; }
#ifdef CY_HLE_CHECK
        else if (!strcmp(a, "--blit-fuzz") && v) { blit_fuzz = atoi(v); i++; }
#endif
        else if (!strcmp(a, "--disk-size") && v) { disk_kb = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--install") && v && ninstalls < MAX_INSTALL) { installs[ninstalls++] = v; i++; }
        else if (!strcmp(a, "--ls")) ls = true;
        else if (!strcmp(a, "--cart")) cart = true;
        else if (!strcmp(a, "--card") && v) { card_path = v; cart = true; i++; }
        else if (!strcmp(a, "--cart-log")) cart = cart_log = true;
        else if (!strcmp(a, "--wav") && v) { wav_path = v; i++; }
        else if (!strcmp(a, "--browse") && v) { browse_keys = v; i++; }
        else if (!strcmp(a, "--browse-url") && v) { browse_url = v; i++; }
        else if (!strcmp(a, "--dump-ram") && v) { dump = v; i++; }
        else if (!strcmp(a, "--hex-port") && v) { hex_port = atoi(v); i++; }
        else if (!strcmp(a, "--pair") && v) { pair_path = extra_path[0] = v; i++; }
        else if (!strcmp(a, "--third") && v) { extra_path[1] = v; i++; }
        else if (!strcmp(a, "--chain")) chain = true;
        else if (!strcmp(a, "--bot") && v) {
            bot.on = true;
            snprintf(bot.nick, sizeof bot.nick, "%s", v);
            bot.version = (uint8_t)(time(NULL) | 1);
            i++;
        }
        else if (!strcmp(a, "--bot-room") && v) { bot.room = atoi(v); i++; }
        else if ((!strcmp(a, "--bot-say") || !strcmp(a, "--bot-tell")) && v && bot.nlines < MAX_BOT_LINES) {
            char *end;
            bot.lines[bot.nlines].at = strtoull(v, &end, 10);
            bot.lines[bot.nlines].to = CYR_BROADCAST;
            if (*end == ':' && !strcmp(a, "--bot-tell")) bot.lines[bot.nlines].to = (uint32_t)strtoul(end + 1, &end, 16);
            if (*end != ':') { fprintf(stderr, "bad %s %s\n", a, v); return 2; }
            snprintf(bot.lines[bot.nlines].text, sizeof bot.lines[0].text, "%s", end + 1);
            bot.nlines++;
            i++;
        }
        else if (!strcmp(a, "--rf-log")) rf_log = true;
        else if (!strcmp(a, "--led-log")) led_log = true;
        else if (!strcmp(a, "--frame-log")) frame_log = true;
#ifdef CY_WITH_BROWSER
        else if (!strcmp(a, "--gate")) {
            static cyb_net_t gate_net;
            cyb_net_curl(&gate_net, NULL);
            cy_gate_host_t gh = { .rf_send = gate_rf, .log = gate_log };
            gate = cy_gate_create(0x4c80c0deu, &gh, &gate_net);
        }
        else if (!strcmp(a, "--gate-home") && v && gate) { cy_gate_set_home(gate, v); i++; }
#endif
        else if (!strcmp(a, "--press-both")) press_both = true;
        else if (!strcmp(a, "--unit-id") && v) { unit_id = (uint32_t)strtoul(v, NULL, 16); i++; }
        else if (!strcmp(a, "--trace-io") && v) {
            trace_lo = (uint32_t)strtoul(v, NULL, 16);
            const char *dash = strchr(v, '-');
            trace_hi = dash ? (uint32_t)strtoul(dash + 1, NULL, 16) : trace_lo;
            i++;
        }
        else if (!strcmp(a, "--press") && v && npresses < MAX_PRESSES) {
            char name[32];
            unsigned ms, dur = 200;
            int who = 0;
            if (v[0] >= 'b' && v[0] <= 'c' && v[1] == ':') { who = v[0] - 'a'; v += 2; }
            if (sscanf(v, "%31[^@]@%u:%u", name, &ms, &dur) < 2 || key_by_name(name) < 0) {
                fprintf(stderr, "bad --press %s\n", v);
                return 2;
            }
            presses[npresses].key = key_by_name(name);
            presses[npresses].who = who;
            presses[npresses].at = (uint64_t)ms * (CY_CLOCK_HZ / 1000);
            presses[npresses].until = presses[npresses].at + (uint64_t)dur * (CY_CLOCK_HZ / 1000);
            npresses++;
            i++;
        } else {
            fprintf(stderr, "usage: %s --rom FILE --flash FILE [--ramdisk FILE] [--seconds S]\n"
                            "       [--screenshot FILE.pgm] [--frames DIR] [--press KEY@MS[:D]]...\n"
                            "       [--disk-size KB] [--install FILE]... [--ls]\n"
                            "       [--unit-id HEX] [--pair RAMDISK2] [--third RAMDISK3] [--chain]\n"
                            "       [--press-both] [--bot NICK] [--bot-room N] [--bot-say MS:TEXT] [--bot-tell MS:ID:TEXT]\n"
                            "       [--rf-log] [--frame-log] [--hex-port N]\n"
                            "       [--trace-io LO-HI] [--dump-ram FILE] [--cart] [--cart-log] [--card IMG] [--wav FILE]\n"
                            "       [--boot UPDATE.boot] [--save-flash FILE]\n", argv[0]);
            return 2;
        }
    }
    if (!rom_path || !flash_path) {
        fprintf(stderr, "--rom and --flash are required\n");
        return 2;
    }

    uint8_t *rom = calloc(1, CY_ROM_BUF);
    uint8_t *r = load(rom_path, CY_ROM_SIZE, NULL, true);
    memcpy(rom, r, CY_ROM_SIZE);
    free(r);
    uint8_t *flash = load(flash_path, CY_FLASH_SIZE, NULL, true);
    if (boot_path) {
        size_t boot_len = 0;
        uint8_t *boot = load(boot_path, CY_FLASH_SIZE, &boot_len, false);
        int r = boot ? cybiko_flash_stage_boot(flash, boot, boot_len) : CY_BOOT_BAD_IMAGE;
        free(boot);
        if (r != CY_BOOT_OK) {
            fprintf(stderr, "%s: %s\n", boot_path,
                    r == CY_BOOT_TOO_BIG ? "too big for the flash" :
                    r == CY_BOOT_NO_LOADER ? "the flash has no CyOS loader to install it" :
                    "not a CyOS update image (magic ffc0ffab)");
            return 1;
        }
    }
#ifdef CY_WITH_BROWSER
    if (gate) cy_gate_set_fonts(gate, flash, CY_FLASH_SIZE);
#endif
    uint8_t *ram = calloc(1, CY_RAM_SIZE);
    uint32_t disk_size = disk_kb ? disk_kb * 1024u : CY_RAMDISK_SIZE;
    uint8_t *disk = disk_kb ? calloc(1, disk_size) : ram;
    if (ramdisk_path) {
        FILE *f = fopen(ramdisk_path, "rb");
        if (f) {
            size_t n = fread(disk, 1, disk_size, f);
            fclose(f);
            fprintf(stderr, "ramdisk: loaded %zu bytes\n", n);
        }
    }
    for (int i = 0; i < ninstalls; i++) {
        size_t len;
        uint8_t *data = load(installs[i], 8u << 20, &len, false);
        char path[512];
        snprintf(path, sizeof path, "%s", installs[i]);
        const char *name = basename(path);
        uint32_t date = (uint32_t)(time(NULL) + 2208988800LL);
        int r = cy_disk_write(disk, disk_size, name, data, (uint32_t)len, date);
        fprintf(stderr, "install %s (%zu bytes): %s\n", name, len,
                r == 0 ? "ok" : r == -1 ? "no room" : "bad name");
        free(data);
    }

    static cybiko_t m, others[MAX_MACHINES - 1];
    /* cybiko_init patches the ROM in place, so the other machines take a
     * copy made before that (patching a patched ROM breaks it). */
    uint8_t *rom2[MAX_MACHINES - 1] = { NULL }, *flash2[MAX_MACHINES - 1] = { NULL };
    if (extra_path[1] && !extra_path[0]) { fprintf(stderr, "--third needs --pair\n"); return 2; }
    for (int k = 0; k < MAX_MACHINES - 1 && extra_path[k]; k++) {
        rom2[k] = calloc(1, CY_ROM_BUF);
        flash2[k] = malloc(CY_FLASH_SIZE);
        memcpy(rom2[k], rom, CY_ROM_SIZE);
        memcpy(flash2[k], flash, CY_FLASH_SIZE);
    }
    cy_host_t host = { .user = (void *)0, .serial_tx = on_tx, .wall_time = wall, .rf_send = on_rf,
                       .card_read = card_read, .card_write = card_write,
                       .app_call = on_app_call };
    cybiko_init(&m, &host, rom, flash, ram);
    if (disk_kb) cybiko_set_disk(&m, disk, disk_size);
    if (unit_id) cybiko_set_unit_id(&m, unit_id);
    if (trace_hi) m.io_trace = io_trace;
    m.cart.present = cart;
    struct stat card_st;
    if (card_path && stat(card_path, &card_st) == 0 && S_ISDIR(card_st.st_mode)) {
        card_dir = cy_vfat_open(card_path, 4096);
        if (!card_dir) { fprintf(stderr, "%s: cannot read the folder\n", card_path); return 1; }
        m.cart.card_blocks = cy_vfat_blocks(card_dir);
        fprintf(stderr, "card: %s as a %u MB disk, %u files (%u left out)\n", card_path,
                (unsigned)(cy_vfat_blocks(card_dir) >> 11), (unsigned)cy_vfat_files(card_dir),
                (unsigned)cy_vfat_skipped(card_dir));
    } else if (card_path) {
        card_file = fopen(card_path, "r+b");
        if (!card_file) { perror(card_path); return 1; }
        fseek(card_file, 0, SEEK_END);
        m.cart.card_blocks = (uint32_t)(ftell(card_file) / 512);
    }
    if (cart_log) m.cart_log = on_cart_log;
    cy_mp3_t *mp3 = NULL;
    FILE *wav = NULL;
    if (cart) {
        mp3 = malloc(cy_mp3_size());
        cy_mp3_init(mp3);
        wav_samples = m.cpu.cycles * WAV_RATE / CY_CLOCK_HZ;
    }
    if (wav_path) {
        wav = fopen(wav_path, "wb");
        if (!wav) { perror(wav_path); return 1; }
        wav_header(wav, 0);
    }
    the_machine = &m;
    machines[0] = &m;

    /* The other machines: same ROM and flash, their own RAM disks and IDs
     * (the first machine's ID plus 1, 2). */
    uint8_t *disk2[MAX_MACHINES - 1] = { NULL };
    int nother = 0;
    for (int k = 0; k < MAX_MACHINES - 1 && extra_path[k]; k++, nother++) {
        if (!disk_kb) { fprintf(stderr, "--pair needs --disk-size\n"); return 2; }
        disk2[k] = calloc(1, disk_size);
        FILE *f = fopen(extra_path[k], "rb");
        if (f) { if (fread(disk2[k], 1, disk_size, f) == 0) fprintf(stderr, "%s is empty\n", extra_path[k]); fclose(f); }
        cy_host_t host2 = { .user = (void *)(intptr_t)(k + 1), .wall_time = wall, .rf_send = on_rf };
        cybiko_init(&others[k], &host2, rom2[k], flash2[k], calloc(1, CY_RAM_SIZE));
        cybiko_set_disk(&others[k], disk2[k], disk_size);
        cybiko_set_unit_id(&others[k], (unit_id ? unit_id : 0x4c000001u) + 1 + k);
        machines[k + 1] = &others[k];
    }

    static uint8_t lum[CY_LCD_W * CY_LCD_H];
    uint32_t last_version = 0xffffffff;
    int frame_no = 0;
    const int32_t chunk = CY_CLOCK_HZ / 10;      /* 100 ms */
    uint64_t total = (uint64_t)(seconds * CY_CLOCK_HZ);
    clock_t t0 = clock();
#ifdef CY_CALLGRIND
    /* Profiling builds: collect only from CY_PROFILE_FROM_MS on
     * (run valgrind with --collect-atstart=no). */
    const char *pf = getenv("CY_PROFILE_FROM_MS");
    uint64_t profile_at = pf ? (uint64_t)atoll(pf) * (CY_CLOCK_HZ / 1000) : 0;
    bool profiling = false;
#endif
    while (m.cpu.cycles < total) {
#ifdef CY_CALLGRIND
        if (!profiling && m.cpu.cycles >= profile_at) { profiling = true; CALLGRIND_TOGGLE_COLLECT; }
#endif
        /* press/release keys in 1 ms steps around scheduled presses */
        int32_t step = chunk;
        for (int i = 0; i < npresses; i++) {
            uint64_t now = m.cpu.cycles;
            if (!presses[i].down && now >= presses[i].at && now < presses[i].until) {
                int who = press_both ? -1 : presses[i].who;
                for (int k = 0; k <= nother; k++)
                    if (who < 0 || who == k) cybiko_key(machines[k], presses[i].key, true);
                presses[i].down = true;
            } else if (presses[i].down && now >= presses[i].until) {
                int who = press_both ? -1 : presses[i].who;
                for (int k = 0; k <= nother; k++)
                    if (who < 0 || who == k) cybiko_key(machines[k], presses[i].key, false);
                presses[i].down = false;
            }
            uint64_t next = !presses[i].down ? presses[i].at : presses[i].until;
            if (next > now && next - now < (uint64_t)step) step = (int32_t)(next - now);
        }
        if (mp3 && step > (int32_t)(CY_CLOCK_HZ / 200)) step = CY_CLOCK_HZ / 200;   /* keep the decoder fed */
        if (pc_profile_ms >= 0 && m.cpu.cycles >= (uint64_t)(pc_profile_ms * (CY_CLOCK_HZ / 1000))) {
            /* sample the PC every 997 states (a prime, to avoid beating with loops) */
            for (int32_t d = 0; d < step; d += 997) {
                cybiko_run(&m, step - d < 997 ? step - d : 997);
                pc_sample(m.cpu.pc);
            }
            if (mp3) mp3_pump(&m, mp3, wav);
            continue;
        }
        if (pair_path || bot.on) {
            /* 1 ms turns keep radio replies well inside CyOS's timeouts */
            for (int32_t d = 0; d < step; d += CY_CLOCK_HZ / 1000) {
                int32_t n = step - d < (int32_t)(CY_CLOCK_HZ / 1000) ? step - d : (int32_t)(CY_CLOCK_HZ / 1000);
                for (int k = 0; k <= nother; k++) cybiko_run(machines[k], n);
                if (bot.on) bot_poll(m.cpu.cycles / (CY_CLOCK_HZ / 1000));
                led_poll(&m);
            }
        } else if (0
#ifdef CY_WITH_BROWSER
                   || gate
#endif
                   ) {
#ifdef CY_WITH_BROWSER
            for (int32_t d = 0; d < step; d += CY_CLOCK_HZ / 1000) {
                int32_t n = step - d < (int32_t)(CY_CLOCK_HZ / 1000) ? step - d : (int32_t)(CY_CLOCK_HZ / 1000);
                cybiko_run(&m, n);
                cy_gate_poll(gate, (uint32_t)(m.cpu.cycles / (CY_CLOCK_HZ / 1000)));
            }
#endif
        } else {
            cybiko_run(&m, step);
        }
        led_poll(&m);
        if (mp3) mp3_pump(&m, mp3, wav);
        if (browse_requested) {
            browse_requested = false;
#ifdef CY_WITH_BROWSER
            run_browser(&m, flash, browse_keys, browse_url, frames, &frame_no);
#else
            (void)browse_keys;
            (void)browse_url;
            fprintf(stderr, "built without the browser (src/components/cy_browser)\n");
#endif
        }
        if (frames && m.cpu.cycles / chunk != (m.cpu.cycles - step) / chunk) {
            uint32_t v = cybiko_lcd_render(&m, lum);
            if (v != last_version) {
                char path[512];
                snprintf(path, sizeof path, "%s/frame_%04d.pgm", frames, frame_no++);
                write_pgm(path, lum);
                last_version = v;
                if (frame_log) fprintf(stderr, "[%.3f] %s\n", (double)m.cpu.cycles / CY_CLOCK_HZ, path);
            }
            for (int k = 0; k < nother; k++) {
                static uint32_t last_version2[MAX_MACHINES - 1] = { 0xffffffff, 0xffffffff };
                static int frame_no2[MAX_MACHINES - 1];
                v = cybiko_lcd_render(&others[k], lum);
                if (v != last_version2[k]) {
                    char path[512];
                    snprintf(path, sizeof path, "%s/%c_frame_%04d.pgm", frames, 'b' + k, frame_no2[k]++);
                    write_pgm(path, lum);
                    last_version2[k] = v;
                    if (frame_log) fprintf(stderr, "[%.3f] %s\n", (double)m.cpu.cycles / CY_CLOCK_HZ, path);
                }
            }
        }
    }
    double host_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (pc_profile_ms >= 0) pc_report();
#ifdef CY_HLE_CHECK
    if (blit_fuzz) {
        extern uint32_t cy_hle_blit_fuzz(cybiko_t *m, int n, uint32_t seed);
        fprintf(stderr, "blit fuzz: %u of %d calls differ\n", cy_hle_blit_fuzz(&m, blit_fuzz, 1), blit_fuzz);
    }
    extern void cy_hle_check_report(void);
    cy_hle_check_report();
#endif
    if (wav) {
        long bytes = ftell(wav) - 44;
        wav_header(wav, (uint32_t)(bytes / 2));
        fclose(wav);
    }

    if (shot) {
        cybiko_lcd_render(&m, lum);
        write_pgm(shot, lum);
        for (int k = 0; k < nother; k++) {
            char path[512];
            snprintf(path, sizeof path, "%s.%c.pgm", shot, 'b' + k);
            cybiko_lcd_render(&others[k], lum);
            write_pgm(path, lum);
        }
    }
    for (int k = 0; k < nother; k++) {
        FILE *f = fopen(extra_path[k], "wb");
        if (f) { fwrite(disk2[k], 1, disk_size, f); fclose(f); }
    }
    fprintf(stderr, "radio: id %08x sent %u heard %u", (unsigned)cybiko_unit_id(&m),
            (unsigned)m.rf.sent, (unsigned)m.rf.heard);
    for (int k = 0; k < nother; k++)
        fprintf(stderr, "; machine %c id %08x sent %u heard %u", 'B' + k, (unsigned)cybiko_unit_id(&others[k]),
                (unsigned)others[k].rf.sent, (unsigned)others[k].rf.heard);
    fprintf(stderr, "\n");
    if (ramdisk_path) {
        FILE *f = fopen(ramdisk_path, "wb");
        if (f) { fwrite(disk, 1, disk_size, f); fclose(f); }
    }
    for (int i = 0; i < ntraces; i++)
        fprintf(stderr, "io %06x %s %06x x%u first %02x last %02x\n", traces[i].pc,
                traces[i].w ? "W" : "R", traces[i].a, traces[i].n, traces[i].first, traces[i].last);
    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (f) { fwrite(m.ram, 1, CY_RAM_SIZE, f); fclose(f); }
        for (int k = 0; k < nother; k++) {
            char path[512];
            snprintf(path, sizeof path, "%s.%c", dump, 'b' + k);
            f = fopen(path, "wb");
            if (f) { fwrite(others[k].ram, 1, CY_RAM_SIZE, f); fclose(f); }
        }
    }
    if (ls) {
        fprintf(stderr, "RAM disk (%u KB, %u bytes free):\n", (unsigned)(disk_size / 1024),
                (unsigned)cy_disk_free(disk, disk_size));
        cy_disk_list(disk, disk_size, print_file, NULL);
    }
    if (disk_kb) fprintf(stderr, "kernel patched %u time(s) for the bigger RAM disk\n", m.disk_patched);
    for (int k = 0; k < nother; k++)
        fprintf(stderr, "machine %c: pc=%06x lcd_on=%d kernel patched %u time(s) illegal=%u wdt resets=%u\n",
                'B' + k, (unsigned)others[k].cpu.pc, (others[k].lcd.reg[0] & 0x40) != 0, others[k].disk_patched,
                (unsigned)others[k].illegal_ops, (unsigned)others[k].wdt_resets);
    if (save_flash) {
        uint8_t *copy = malloc(CY_FLASH_SIZE);
        memcpy(copy, flash, CY_FLASH_SIZE);
        cybiko_flash_original(&m, copy);
        FILE *f = fopen(save_flash, "wb");
        if (f) { fwrite(copy, 1, CY_FLASH_SIZE, f); fclose(f); }
        free(copy);
    } else if (m.flash.dirty) {
        fprintf(stderr, "note: the OS wrote to flash (not saved without --save-flash)\n");
    }

    double emu_s = (double)m.cpu.cycles / CY_CLOCK_HZ;
    fprintf(stderr,
            "\n--- %.2f s emulated in %.2f s host (%.0f%% of real time, %.1f MHz)\n"
            "pc=%06x sleeping=%d off=%d lcd_on=%d frames=%d\n"
            "unmapped reads=%u writes=%u illegal=%u dma-dreq=%u wdt resets=%u\n",
            emu_s, host_s, host_s > 0 ? 100.0 * emu_s / host_s : 0.0,
            host_s > 0 ? m.cpu.cycles / host_s / 1e6 : 0.0,
            (unsigned)m.cpu.pc, m.cpu.sleeping, m.standby, (m.lcd.reg[0] & 0x40) != 0, frame_no,
            (unsigned)m.unmapped_reads, (unsigned)m.unmapped_writes, (unsigned)m.illegal_ops,
            (unsigned)m.dtc_dma_use, (unsigned)m.wdt_resets);
    return 0;
}
