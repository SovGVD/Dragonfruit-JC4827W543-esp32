/* The Cybiko Classic's MP3 player (CY65P10), seen from the Classic: a
 * command protocol on the slot, answered here (high-level emulation; the
 * player's own H8S/2246 firmware was never dumped). What MP3.dl 1.1.x does,
 * from its code:
 *
 * - Lines: P1.4 is read as "player there" (1). P1.7, an output only while
 *   MP3.dl talks, selects the player (low); MP3.dl lets it go high when it
 *   is done. P2.1 (an input while selected; on the memory card it is the
 *   card flash's select) is the player's ready line: MP3.dl waits for it to
 *   change before each chunk it sends or reads (600 ms at most).
 * - Bytes go on SCI1 in clocked sync mode, the bus of the DataFlash, while
 *   the DataFlash is not selected. A chunk is a 4-byte header or up to 512
 *   bytes of data.
 * - A command: header (length, command; 16 bits each, big-endian), then the
 *   data. The answer: header (length, status), then the data. The status's
 *   low byte repeats the command; its high byte is 0 for OK, else error
 *   bits (bit 1: start again).
 *
 * The commands (cy_mp3c_player; the list is in
 * docs/cybiko-classic-modules.md): 10 play list, 11 a song's details, 12
 * pause/go on, 13 play from a block, 14 stop, 15 play, 16 go to a block, 17
 * status, 18 volume, 19 balance, 1a equalizer, 1c repeat and shuffle, 1d new
 * play list, 1e previous/next, 1f mode, 31 versions, 38 list the card, 39 a
 * file's details, 3b the card's size. Commands not known are answered with nothing and logged
 * (cy_mp3c_t.log), as are all but the status polls. */
#ifndef CY_MP3C_H
#define CY_MP3C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CY_MP3C_CHUNK 512u
#define CY_MP3C_MAX   4096u
#define CY_MP3C_LIST  100u

typedef struct cy_mp3c cy_mp3c_t;

/* Answer command `cmd` with its `len` bytes of data: fill out (up to
 * CY_MP3C_MAX bytes), set *out_len, return the status's high byte (0 OK).
 * Setting p->follow asks for that many bytes of data after the answer; the
 * handler is called again for them with p->second set, and answers again. */
typedef uint8_t (*cy_mp3c_handler_t)(cy_mp3c_t *p, uint16_t cmd, const uint8_t *data, uint16_t len,
                                     uint8_t *out, uint16_t *out_len);

struct cy_mp3c {
    bool present;
    bool selected;             /* P1.7 low */
    uint8_t ready;             /* P2.1, toggled at the start of each chunk */
    enum { MP3C_CMD_HDR, MP3C_CMD_DATA, MP3C_ANS_HDR, MP3C_ANS_DATA } state;
    uint8_t hdr[4];
    uint16_t pos, chunk_left;
    uint16_t len, cmd;
    uint8_t data[CY_MP3C_MAX];
    uint8_t ans_hdr[4];
    uint16_t ans_len;
    uint8_t ans[CY_MP3C_MAX];
    cy_mp3c_handler_t handler;
    void (*log)(void *user, const char *line);
    void *user;
    bool armed;                /* a chunk is coming in (cy_mp3c_arm) */
    uint32_t commands, unknown, dropped;
    bool software;             /* MP3.dl has talked: a command other than 31, which CyOS
                                * 1.2.57 sends itself at start-up */
    uint16_t follow;           /* the handler: this many bytes of data follow the answer */
    bool second;               /* the handler is called for those */

    /* the player (cy_mp3c_player): its SmartMedia card is a folder */
    char root[160];
    struct cy_mp3c_file *files;
    uint16_t nfiles;
    bool no_card;              /* the folder is not there */
    uint16_t play_state;       /* status word 0: CY_MP3C_STOPPED ... */
    int8_t balance;            /* -15 .. 15 */
    uint8_t volume;            /* 0 .. CY_MP3C_VOLUME_MAX */
    uint8_t bass, treble;
    uint8_t repeat, shuffle;   /* command 1c */
    uint32_t song;             /* the file playing (its id), CY_MP3C_NONE */
    FILE *fp;
    uint32_t play_pos;         /* bytes into it */
    uint32_t list[CY_MP3C_LIST];   /* the play list (file ids) */
    uint16_t nlist;
    uint32_t song_size;
};

#define CY_MP3C_NONE 0xffffffffu
#define CY_MP3C_VOLUME_MAX 46
enum { CY_MP3C_STOPPED = 0, CY_MP3C_PAUSED = 1, CY_MP3C_PLAYING = 2 };   /* status word 0 */

/* the player's buttons */
enum { CY_MP3C_BTN_PREV, CY_MP3C_BTN_PLAY, CY_MP3C_BTN_NEXT, CY_MP3C_BTN_STOP,
       CY_MP3C_BTN_VOL_UP, CY_MP3C_BTN_VOL_DOWN };

void cy_mp3c_init(cy_mp3c_t *p, cy_mp3c_handler_t handler, void (*log)(void *, const char *), void *user);
/* The player as far as it is known, with its card the folder `root` (MP3
 * files in it are the songs). Pass it to cy_mp3c_init as the handler. */
uint8_t cy_mp3c_player(cy_mp3c_t *p, uint16_t cmd, const uint8_t *data, uint16_t len, uint8_t *out, uint16_t *out_len);
void cy_mp3c_set_root(cy_mp3c_t *p, const char *root);
/* The MPEG data of the song playing, for the decoder (0 when none) */
size_t cy_mp3c_stream(cy_mp3c_t *p, uint8_t *buf, size_t max);
/* The decoder's gain as the cartridge's volume register (0x6a: 0 dB, 0 mute) */
uint8_t cy_mp3c_gain(const cy_mp3c_t *p);
void cy_mp3c_button(cy_mp3c_t *p, int button);
void cy_mp3c_free(cy_mp3c_t *p);
/* P1.7 as the pin is (1: high or not driven) */
void cy_mp3c_select_pin(cy_mp3c_t *p, bool high);
/* One byte on SCI1. mode: SCR's TE and RE (bits 1, 0): 2 sends only
 * (MP3.dl's DTC writes), 1 receives only (MP3.dl's reads), 3 both (CyOS
 * 1.2.57's own byte-at-a-time check at start-up). */
uint8_t cy_mp3c_spi(cy_mp3c_t *p, uint8_t in, int mode);
/* SCI1's transmitter or receiver switched on: MP3.dl starts moving a chunk
 * (bytes clocked past a chunk's end are dropped until then). */
void cy_mp3c_arm(cy_mp3c_t *p);
void cy_mp3c_hex(char *out, size_t cap, const uint8_t *b, uint16_t n);
void cy_mp3c_logf(cy_mp3c_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
