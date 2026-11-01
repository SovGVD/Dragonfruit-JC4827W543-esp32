# Cybiko Classic: the memory card, CyOS 1.2.57 and the MP3 player

The first Cybiko, the Classic, had two add-ons for the 68-pin slot on its
back: the **memory card** (the "Cybiko 1Mb Memory Card", or Expansion Memory)
and the **MP3 player** (CY65P10). Only one of them fits at a time, because
they share the slot. Both need a newer CyOS than the 1.2.46 of MAME's dump:
Cybiko's **System Pack 57** (CyOS 1.2.57).

Status, 2026-11-01:

| What | State |
| --- | --- |
| Memory card (256 KB RAM, 1 MB DataFlash) | Emulated; CyOS 1.2.57 sees and uses it. PC-tested |
| Installing System Pack 57 (CyOS 1.2.57) | Emulated, from Cybiko's own `Cybiko-AutoUpdate.exe`, on the board, in the PC runner and in the web emulator. PC-tested |
| MP3 player | Emulated from what its software does: songs from a folder, MP3.app's controls, the player's six buttons on screen. PC-tested |

None of it has been tried on the board yet. Sources are listed at the end;
what is a guess is marked as one.

## The expansion slot

The slot uses a PCMCIA Type 1 connector, but not PCMCIA signals. Cybiko's
CY6411 schematic (Gleb's copy, kept with the project's files) shows what the
slot (X2) carries, and corrects the guesses an earlier version of this page
made from the hallikainen.org pinout:

| Signals on the slot | Where they go in the Classic |
| --- | --- |
| D0-D7, A0-A19, /RD, /WR | the CPU's parallel bus |
| CE1 | chip select CS2: the card's RAM, at 400000 |
| CEF1- | port P2.1: the card's DataFlash chip select |
| DIN, DOUT, CCLK | **SCI1**, shared with the internal DataFlash (whose select is P3.4) |
| a second flash select | P1.4, for a card with two flash chips; the MP3 player's "I am here" and reset line |
| (the MP3 player) | P1.7 selects it, P2.1 is its ready line, SCI1 its bytes |
| R_BF, RES, TIOCA0/B0/A1/B2, TMO1 | the flash's ready line, reset, timer pins |

So the card's flash is on the same serial bus as the internal one, and only
its chip select differs. SCI2 is not on the slot: it goes to the PC serial
cable (TXDS/RXDS), as CyOS's console, and SCI0 goes to the radio's AVR. The
inside of the Classic has a BS62XV4000 SRAM on CS1, footprints for an
AT45DB161 or AT45DB041A, and a PCF8593 real-time clock.

## The memory card

- **256 KB of static RAM and 1 MB of DataFlash** (Wikipedia; "Some Cybiko
  programs will not run unless the Expansion Memory is plugged in").
- **How CyOS 1.2.57 finds it.** Its start-up writes each size from 2 MB down
  to 64 KB at 400000 + size - 4, then reads back the mirror at 5ffffc: the
  RAM answers as 256 KB. Its flash driver picks a chip by number: 0 is P3.4
  (inside), 1 is P2.1 (the card), 2 is P1.4. CyOS then says "Cartridge: RAM
  262144 bytes, flash disk 4091 blocks", and the File Manager shows **Card
  Flash** (991 KB) next to the internal flash.
- **In the emulator** (`cy_classic.c`): 256 KB of RAM at 400000, mirrored up
  to 5fffff, and a second AT45DB081 behind P2.1 on SCI1, sharing the internal
  flash's code. A new card is formatted empty. CyOS 1.2.46 ignores it.
- **On the board:** the card is in unless `cybiko.cfg` has
  `classic.slot = mp3` or `empty` (or `classic.card = 0`), or the MP3
  player's folder `/Cybiko/Classic/smc` has files. Its flash is `/Cybiko/Classic/card.bin` (1 MB), saved
  when CyOS writes to it, like `flash_saved.bin`. Delete it for an empty card.
- **In the PC runner:** `--mem-card IMG` plugs it in; IMG is loaded (or an
  empty card made) and written back at the end if it changed.
- **In the web emulator:** Settings, Memory card in (on by default). Its
  flash is kept in the browser.

Tested in the PC runner: File Manager, Fn+Enter on a file, Copy to... Card
Flash copied it (26 pages written), and the copy reads back from the card
image.

## Installing System Pack 57 (CyOS 1.2.57)

Cybiko shipped CyOS updates as **System Packs**, installed from a PC by
`Cybiko-AutoUpdate.exe` over the serial cable. Gleb found System Pack 57; it
is backed up in [cybiko-roms](https://github.com/SovGVD/cybiko-roms)
(`classic/Cybiko-AutoUpdate.exe`).

- **Inside the .exe.** It is a Windows program that keeps the files as
  resources of type `FILES_TO_LOAD`: resource 2000 lists the names, 2001 + i
  holds file i. Its `autoupdate.ini` has a part per model. The Classic's
  `[114]` takes `common\System Pack\*` and `114\System Pack\*` (31 files:
  `comp1.2.57r.boot`, `desktop.app` and the other apps), then runs `boot
  comp1.2.57r.boot` and `avr write avr19c6.bin`. `[119]` is CyOS 1.3.57 for
  Classics that run CyOS from a parallel ROM at 100000; the emulated Classic
  has none, so it is not used. `FILES_TO_BOOT` (1996-1999) holds the loaders.
- **The .boot file** is compressed with the boot ROM's LZSS (header
  `01c0ffab`, then the packed and unpacked sizes; flag bits from the lowest,
  1 a literal; a match is two bytes a, b: distance `a | (b & 0xf0) << 4`,
  length `(b & 15) + 3`). The ROM unpacks it to 200000 and jumps to the word
  at 200004. CyOS 1.2.57 keeps its Cybiko ID at 222cf4 in RAM, where the
  emulator puts the board's (or the tab's) ID, as for 1.2.46.
- **How it goes in** (`cy_sysupdate.c`, `cy_con.c`). Through CyOS's console
  on SCI2 at 57600 baud, as AutoUpdate did. ReBiko (MIT, Dan Berkowitz)
  worked out the console protocol and the order, in its
  `tools/rebiko_cyos_update.py`; Dragonfruit's code is written separately:
  - A command is its text, a newline and a 16-bit sum (each byte: `sum =
    ((sum ^ c) << 1)`, the carry back in at bit 0). CyOS answers 02 (taken)
    or 01 (bad sum), and "Ready >" when the command is done.
  - `rcv NAME SIZE` sends a file: CyOS asks "sendN" for block N, the PC
    answers `C`, N, the length (16 bits, little-endian), up to 512 bytes and
    their CRC-32. CyOS says "receive successfull", or gives up.
  - The steps: `ver` (already 1.2.57: nothing to do), every file but
    `desktop.app`, `rm -f desktop.app`, `desktop.app` (CyOS cannot replace
    the desktop it runs before it is deleted), `boot comp1.2.57r.boot`,
    `reboot`, then `ver` must say 1.2.57. A failed step is done again, up
    to five times. `avr write avr19c6.bin` (the radio chip's firmware) is
    left out: the radio chip is emulated.
- **Fix found on the way.** Bytes sent while a receive error flag was up
  raised only the error interrupt, so CyOS never read them and the transfer
  died. Clearing the error flag now raises the receive interrupt when a byte
  is waiting (`cy_periph.c`), as the real SCI does.
- **It takes about 6 minutes of emulated time**; the first start of 1.2.57
  is slow and once says "INCORRECT FILES FOUND!!! cluster count: 0", which
  is harmless (not there on the next start).

**CyOS 1.2.57 is the default.** cybiko-roms has `classic/flash_v1257.bin`,
made once by this installer from the 1.2.46 dump (`cybiko_host --rom
cyrom112.bin --flash flash_v1246.bin --sysupdate Cybiko-AutoUpdate.exe
--save-flash flash_v1257.bin --seconds 460`). The board takes it before
`flash_v1246.bin`, and the web emulator's Start fetches it, with 1.2.46 as a
choice (Settings, CyOS). The installer is for a flash that has 1.2.46 and
files to keep.

How to use the installer:

- **Board:** copy `Cybiko-AutoUpdate.exe` to `/Cybiko/Classic` and start the
  Classic. The serial log shows each step; when it is done the file is
  renamed `Cybiko-AutoUpdate.exe.installed`, and `flash_saved.bin` holds CyOS
  1.2.57. If it fails, the log says why and the file stays, so the next start
  tries again.
- **PC runner:** `--sysupdate Cybiko-AutoUpdate.exe --save-flash out.bin
  --seconds 420` (with `--mem-card card.bin` for the card); `[update]` lines
  show the steps.
- **Web emulator:** the Classic's **Install CyOS 1.2.57** button fetches the
  .exe from cybiko-roms and installs it, running as fast as the computer
  can.

## The MP3 player (CY65P10)

The player is a whole computer of its own, powered by the Cybiko through the
slot:

| Part | Chip |
| --- | --- |
| CPU | Hitachi H8S/2246 (16-bit, 11 MHz), with its own firmware inside |
| MP3 decoder | Micronas MAS3507D-F10 (MPEG 1 and 2, layers 2 and 3) |
| DAC | Micronas DAC3550A |
| USB | National USBN9603, to a mini-USB socket |
| Storage | one SmartMedia card, 4 to 64 MB, FAT12 |
| Extended edition | FM tuner (TDA7021T, TDA7040T) and a microphone |

From the manual pages: it plays on its own six buttons; the Cybiko's MP3
application adds an equalizer and play-list editing. It needs System Pack
v.56 or later. Music goes onto the SmartMedia card over the player's USB,
and the card can also hold Cybiko games and apps.

**Its software.** Gleb found all three pieces:

| File | What it is |
| --- | --- |
| `MP3.app` 1.3.13 | the Classic's MP3 application (MP3, FM and Voice modes), for CyOS 1.2.56 and later; backed up as `classic/MP3.app` in cybiko-roms |
| `MP3.dl` 1.1.7 | the player's driver: 11958 bytes of H8S code, 32 functions (the bus, the commands, the card's files) |
| `mp3_control.dl` 1.1.3 | "MP3 Player Dynamic Library", built on `MP3.dl` |

They are in neither System Pack 57 nor the three Cybiko SDKs; they came with
the player. All three go into CyOS's flash (see [Installing the
software](#installing-the-software)).

### How the Classic talks to the player

Read from `MP3.dl` and CyOS 1.2.57 (which asks for the player's version at
start-up and lists its card as a drive). The player's own firmware is in its
H8S/2246's ROM, which nobody has dumped, so the emulator answers the
commands itself (`cy_mp3c.c`).

- **Lines.** P1.4 reads 1 while a player is in (driven low for about 200 ms,
  it resets the player). P1.7 selects it: low while `MP3.dl` talks, high or
  let go when it is done. P2.1, an input while the player is selected, is
  its ready line: it changes before each chunk the player can take or give,
  and `MP3.dl` waits for the change (up to 600 ms).
- **Bytes.** On SCI1 in clocked mode, the bus of the DataFlash, while the
  DataFlash is not selected. `MP3.dl` moves each chunk with one DTC transfer
  (send: SCR 0xa0; receive: SCR 0x50) and switches the SCI off after it;
  CyOS's own check at start-up moves byte by byte with both ways on (SCR
  0x30). The real SCI clocks a byte or two past a chunk (the DTC count is one
  more than the chunk when sending; the receiver runs on until it
  overruns), so the player takes a chunk only after the SCI is switched on
  again.
- **Framing.** A command is a 4-byte header (length, command; 16 bits each,
  big-endian), then the data in chunks of up to 512 bytes. The answer is a
  header (length, status) and its data. The status's low byte repeats the
  command; its high byte is 0 for OK, else error bits (bit 0: 1, bit 1: 2
  "start again", bit 2: 4, bit 3: 3 "no SmartMedia card", bit 4: 6 "USB
  connection", bit 5: 7 "card write-protected"). `MP3.dl` tries a command
  up to 20 times.
- **Data after the answer.** Command 1d (a new play list) sends its count
  first, and the list (4 bytes a song) after the answer, with a second
  answer.

### The commands

Numbers are big-endian unless said otherwise. "Guess" marks what is inferred
from how MP3.app uses a command without seeing it change anything.

| Command | Data | Answer | What it does |
| --- | --- | --- | --- |
| 0110 | u16 max | u32 song ids | the MP3 play list (high byte 1: MP3's) |
| 0011 | u32 song | its details, under 0xb0 bytes (below) | what MP3.app shows for a song |
| 0012 | u32 song | | pause, or go on, if it is the song playing; else play it from the start (MP3.app's play button with no saved place; the File Manager's Play on an SMC file sends 1f 0000, 14, then this) |
| 0013, 0043 | u32 song, u16 block | | play a song from a 512-byte block |
| 0014 | | | stop |
| 0015 | | | play (the play list, else every song) |
| 0016 | u32 block | | go to a 512-byte block of the song (Shift + Left/Right) |
| 0017 | | 20 bytes (below) | status, asked several times a second |
| 0018 | u8 | | volume, 0 to 46 in steps of 2 |
| 0019 | s8 | | balance, -15 to 15 (guess) |
| 001a | u8, u8 | | the equalizer (guess; not done) |
| 001c | u8 repeat, u8 shuffle | | ff keeps one as it is |
| 011d | u16 count, then the ids | | a new MP3 play list |
| 001e | u8 | | 0 the previous song, 1 the next (Fn + Up/Down); 2 and 3 not seen |
| 001f | u16 | | the mode: 0 MP3 |
| 0031 | | u16 hardware, u16 software | versions (the emulator says 1 and 0103) |
| 0038 | u16 max, u16 kind, u32 from, u32 folder | u32 file ids | list the card (folder ffff fffe or 0: the top one; kind 1, from MP3.app: MP3 files only). Fewer than 0x32 ids: the end |
| 0039 | u32 file | u32 size, u16 time and u16 date (DOS, little-endian), u8 attributes, the name | a file's details |
| 003b | | 20 bytes: u32 total @4 and u32 free @8, in 512-byte blocks; the rest not known (sent as 0) | the card's size: the File Manager's free/total on its SMC line (found by trying values) |
| 003a | u32 file, u16 kind, u32 value | | set a file's details (`MP3.dl`; not seen) |
| 003b | | 20 bytes | the card's size (`MP3.dl` checks it against 4 to 64 MB) |
| 0032, 1032 | u32 folder, the name | u32 file | find a file by name |
| 0033 | u32 file | | delete |
| 0034 | u32 file, u32, the new name | | rename |
| 0030, 0035, 0036, 0037, 003c | | | open, read, write, close and others for CyOS's files on the card: not decoded |
| 0020-0027 | | | MP3.app's FM mode (the extended player): not decoded |

The status (0017): u16 state (0 stopped, 1 paused, 2 playing for MP3; 3 to
7 voice, 8 and 9 FM), s8 balance, u8 volume, u16 (not known), 4 bytes not
known, u8 repeat, u8 shuffle, u32 the song (ffff ffff: none), u32 where it
plays in 512-byte blocks. A song's details (0011): u32 size, u16 kbps, u16
stereo (0 mono), then ID3v1's title, artist, album (30 bytes each), year
(4), comment (30) and genre (1). MP3.app copies 0x168 bytes from where it
reads the answer, and anything past 0xb0 bytes lands on its own variables,
so the player answers with less.

### In the emulator

- **The card** is a folder: MP3 files in it are the songs (no subfolders
  yet). File ids are the files' places in the listing, sorted by name.
- **Playing.** The player reads the song itself and feeds it to the same
  MP3 decoder as the Xtreme's cartridge; its volume is 1 dB a step.
  Without MP3.app it still plays, from its own buttons, as the real one did.
- **The buttons**: back, play/pause, forward, stop, volume up and down. On
  the board they are beside the LCD in portrait and on the MORE page in
  landscape, shown only while the player is in; in the web emulator they
  are under the keyboard.
- **Board.** `cybiko.cfg` `classic.slot` says what is in the slot: `auto`
  (the default: the player when `/Cybiko/Classic/smc` has files, else the
  memory card), `card`, `mp3` or `empty`. The card is `/Cybiko/Classic/smc`.
- **PC runner.** `--mp3-player DIR` plugs it in with DIR as the card,
  `--mp3-button play@MS` presses its buttons, `--wav` records its music.
- **Web emulator.** With a Classic, the MP3 card (Settings) puts the player
  in instead of the memory card, its files the card; Install the MP3
  player's software puts the three files into CyOS's flash.
- **No card.** Without the folder, the status (0017) answers "no
  SmartMedia card" and the log says so.
- **Fix found on the way.** The emulated DTC wrote its registers back after
  moving the data, so when writing TDR raised the next transfer at once,
  that one started from the old registers and sent the first byte twice.
  It now writes them back first (`cy_dma.c`), as the real DTC does.
- **Not there.** CyOS's file work on the card (copying games to it), FM,
  voice recording, the equalizer and the USB link to a PC.

Tested in the PC runner (MP3.app 1.3.13 on CyOS 1.2.57): the song list,
play, pause, stop, next, Shift+Right (go to), volume, repeat and shuffle,
the title, artist, album, bit rate and time on screen, and the music in a
WAV file.

### Installing the software

CyOS 1.2 keeps programs in its flash, which the emulator cannot write files
into directly, so they go in through CyOS's console, as the System Pack
does (`cy_su_start_files`):

- **Board:** copy `MP3.app`, `MP3.dl` and `mp3_control.dl` (or any other
  Classic program) to `/Cybiko/Classic/install`. The next start sends them
  once CyOS is up and moves them to `install/installed`.
- **PC runner:** `--install FILE` for each.
- **Web emulator:** the Classic's Install the MP3 player's software button.

Then start MP3 from CyOS's desktop.

## Other uploads

- `cios_beta_2.app` and `hypercomm.app` are HyperDev homebrew; both need
  CyOS 1.3.57, which the emulated Classic does not run.
- The CY6411 schematic and the Classic's user manual are kept with the
  project's files, not in cybiko-roms (Gleb's choice).

## Sources

- Cybiko's CY6411 schematic and `Cybiko-AutoUpdate.exe` (System Pack 57),
  uploaded by Gleb; kn100.me keeps a copy of the updater
- [Cybiko expansion connector pinout](https://pic.hallikainen.org/techref/cybiko/expansion.htm), hallikainen.org
- [CY65P10 MP3 player for Cybiko Classic](https://pic.hallikainen.org/techref/cybiko/CY65P10.htm), hallikainen.org
- [MP3 Player](http://dbzoo.com/cybiko/mp3player), DBZoo
- [Mp3 Player](https://bluecybiko.tripod.com/bluecybiko/id4.html), BlueCybiko (the manual)
- [Cybiko](https://en.wikipedia.org/wiki/Cybiko), Wikipedia (memory card: 256 KB SRAM and 1 MB DataFlash)
- [ReBiko](https://github.com/RetroCodeRamen/rebiko) by Dan Berkowitz (MIT): `tools/rebiko_cyos_update.py`, the console protocol and the order of the update
- MAME's `cybiko.cpp` lists the MP3 player's chips; it has no driver for the player or the memory card
