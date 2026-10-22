# Cybiko Xtreme

The JC4827W543 firmware also emulates the **Cybiko Xtreme** (2001): a Hitachi
H8S/2323 at 18.432 MHz, 2 MB RAM, 512 KB flash and a 160×100 four-shade LCD.
Pick **Cybiko** in the [boot menu](jc4827w543.md#boot-menu).

The hardware model follows MAME's `cybikoxt` driver. It is tested with small
H8S programs (`src/host/test_cybiko.c`), and with the real `cybikoxt` dumps
CyOS 1.5.08 boots to its first-run setup (welcome, nickname, contrast, clock)
in the [PC runner](#pc-runner). No ROM is shipped here.

How the machine works inside (memory map, screen, keys, radio protocol, boot,
RAM disk format) is in [cybiko-internals.md](cybiko-internals.md); the MP3
cartridge, `.app` archives and MXD video are in
[cybiko-mp3-cartridge.md](cybiko-mp3-cartridge.md).

## SD card

Everything for the Cybiko lives in `/Cybiko` on the card. You supply both
images. They are the MAME `cybikoxt` set:

| File | Size | Accepted names (first match wins) |
| --- | --- | --- |
| Boot ROM | 32 KB | `bootrom.bin`, `cyrom150.bin`, `xtreme-rom.bin` |
| CyOS flash | 512 KB | `flash.bin`, `cyos_v1508.bin`, `xtreme-flash.bin` |

Games and apps go in `/Cybiko/apps` (see [Games and apps](#games-and-apps)),
and the MP3 cartridge's card is `/Cybiko/card` (see
[MP3 cartridge](#mp3-cartridge)). The firmware writes these itself:

```text
ramdisk.bin   the RAM disk where CyOS keeps user files, 4 MB by default
state.bin     the Cybiko clock, so the time survives restarts
flash.bin     only if CyOS reprograms its flash (an OS update); being the
              first flash name, it then replaces the original image
cybiko.cfg    optional settings, written by you (see below)
```

The RAM disk is saved within 30 seconds of a change, once the screen has not
been touched for 2 seconds, and when you hold BOOT to leave. Only the parts
that changed are written. To start from an empty RAM disk, delete
`ramdisk.bin`.

## Storage

A real Cybiko Xtreme keeps user files in a 512 KB battery-backed RAM disk.
Here the RAM disk is bigger, 4 MB by default, and lives in `ramdisk.bin` on
the SD card. Set its size in `cybiko.cfg`:

```ini
disk.size = 4    # MB, 1-6 (default 4)
```

The disk only grows: an existing `ramdisk.bin` that is bigger than
`disk.size` keeps its size. A 512 KB `ramdisk.bin` from older firmware keeps
its files and grows to the new size on the first start. A bigger disk makes
CyOS take longer to start: it checks every page of the disk, which adds about
2 seconds per MB at full speed.

How it works: CyOS 1.5.08 sets up its RAM disk with a fixed address and size.
The firmware patches those two numbers in the kernel after the boot loader
has unpacked it and before it runs, and moves the disk to addresses the real
machine leaves free (`800000`-`dfffff`, so at most 6 MB). The format on the
disk is unchanged.

More storage, and music, come with the emulated MP3 player cartridge: files
dropped into `/Cybiko/card` on the SD card are on its card (see
[MP3 cartridge](#mp3-cartridge)).

## MP3 cartridge

The Xtreme's MP3 player cartridge (CY45P01) is emulated: its SD card slot,
its MP3 decoder and the MXD video clips. Its software came on the
cartridge's CD and is not in the flash, so you need `MP3.cap` from that CD.
In short:

1. `python3 tools/cybiko_cap.py extract MP3.cap /Cybiko/apps` (on the SD
   card) installs the cartridge software.
2. Put `.mp3` files and MXD clips (`.app`) in `/Cybiko/card`, up to 1 GB.
3. "MP3 Player" and the clips appear on the desktop. Sound comes out of the
   speaker.

`cart = auto | mp3 | off` in `cybiko.cfg` plugs the cartridge in or out
(`auto`, the default, plugs it in when `/Cybiko/card` has files). The
**MP3 CART: IN / OUT** button on the Cybiko card of the boot menu takes it
out without editing the file. As on the real machine, some games (CyRace 2,
for one) quit straight back to the desktop while the cartridge and its card
are in: take it out to play them.
`tools/cybiko_mxd.py` turns a video into an MXD clip, and a clip back into
an MP4 (`cybiko_mxd.py clip.app clip.mp4`). How the cartridge
works, with everything needed to reimplement it, is in
[cybiko-mp3-cartridge.md](cybiko-mp3-cartridge.md).

## Games and apps

Put Cybiko Xtreme programs (`.app` files) and the files that come with them
in `/Cybiko/apps` on the SD card:

```text
/Cybiko/apps/game.app
/Cybiko/apps/game.dat
```

At start the firmware copies each file there onto the RAM disk if the disk
does not have a file of that name yet. Files already on the disk are left
alone, so game saves and other changes CyOS makes are kept. To update a
program, delete it on the Cybiko (or delete `ramdisk.bin`) and it is copied
again on the next start. A program you delete on the Cybiko comes back on the
next start while it is still in `/Cybiko/apps`. Names can be up to 58
characters long. Files that do not fit on the RAM disk are skipped and
reported on the serial monitor.

These must be programs for the Cybiko Xtreme. Programs for the original
Cybiko (Classic) do not run on it.

## Controls

The Cybiko starts in **portrait**: hold the board upright with USB at the
bottom. The button in the top-left corner of the boot menu switches between
**CYBIKO: PORTRAIT** and **CYBIKO: LANDSCAPE**, and the choice is remembered.
The boot menu itself is always landscape. The Cybiko LCD is only ever zoomed by
whole numbers, so its pixels stay square: 1× in portrait, 2× in landscape. If
taps land in the wrong place, run the touch calibration from the boot menu
(**CALIBRATE**, see
[Calibrate resistive touch](jc4827w543.md#calibrate-resistive-touch)).

Portrait (272×480), every key on one page:

```text
+--------------------------+
|   Cybiko LCD (160x100)   |
| F1 F2 F3 F4 F5 F6 F7 INS |
|      UP     | ESC  MENU  |
| LEFT SEL RIGHT| TAB  DEL |
|     DOWN    | HELP ON/OFF|
| Q W E R T Y U I O P      |
| A S D F G H J K L ;      |
| Z X C V B N M , . !      |
| SHIFT ( SPACE ) FN ENTER |
+--------------------------+
```

Landscape (480×272), LCD at 2×:

```text
+--------------------------+----------------+
|                          | ESC   UP  MENU |
|  Cybiko LCD (x2)         | LEFT SEL RIGHT |
|                          | TAB  DOWN  DEL |
|                          | ENTER   SPACE  |
+--------------------------+----------------+
| Q  W  E  R  T  Y  U  I  O  P              |
| A  S  D  F  G  H  J  K  L  .              |
| SHIFT Z  X  C  V  B  N  M  FN  MORE       |
+-------------------------------------------+
```

- Every key of the real Xtreme is there: F1-F7, Esc, Menu, the arrows,
  Select, Tab, Del, Ins, Enter, Help (the "?" key), On/Off, the letters,
  `; , . !`, `( )` beside the space bar, Shift and Fn.
- In landscape, **MORE** shows F1–F7, INS, ON/OFF, `( ) , ; !` and HELP;
  **ABC** goes back.
- The touchscreen sees one finger at a time, so **SHIFT** and **FN** latch: tap
  one, then the key. They release with that key. While one is on, the keys
  show what they type: capitals with SHIFT, and with FN the digits on Q-P and
  the symbols CyOS puts on the other letters (`@ } (C) $ % * + - _ = > < & { #
  "`). On the punctuation keys FN gives `/ ' : ? [ ]` and SHIFT gives
  `\ ~ | ^`, `` ` `` and a smiley.
- **BOOT** is the Cybiko On/Off key. **Hold BOOT for 3 seconds** to save and
  restart to the boot menu.

## Sound

The Cybiko beeper plays through the board's speaker with the same `audio`
component as Palm. The tone is taken from the timer that drives the speaker
pin (TPU channel 1, TIOCB1), or measured from how fast software toggles the
pin. Set the volume with **-** and **+** in the boot menu. Until that has
been changed, `cybiko.cfg` can set it:

```ini
sound.volume = 60    # 0-100, 0 = off
```

## Radio

The radio works between boards: every board running Cybiko sees the others
in CyOS (the people nearby counter, Chat, games that play over the radio).

A real Cybiko talks to its radio through a small AVR co-processor on serial
port SCI2. The firmware emulates that co-processor (`cy_rf.c`, worked out from
CyOS 1.5.08) and carries each radio packet to the other boards as an ESP-NOW
broadcast. The packets themselves are CyOS's own and pass through unchanged.

CyOS ignores packets that carry its own Cybiko ID, and every copy of a flash
dump has the same one, so each board gives itself a new ID made from its MAC
address. The ID lives in the flash dump with a CRC that the boot ROM checks;
the firmware recomputes it in RAM and never changes the dump on the SD card.

```ini
rf = espnow      # default; off switches the radio off
rf.channel = 1   # Wi-Fi channel, the same on every board (1-13)
rf.id = auto     # auto (from the MAC), keep (the dump's own ID) or 8 hex digits
```

The radio uses the Wi-Fi chip in station mode on a fixed channel, so it
cannot share it with the Palm side's Wi-Fi; they never run at the same time.

The co-processor protocol, from the H8S: `01 02 ch` tunes to channel `ch`,
`01 03 xx xx` is an unknown setting, `30 f` plus 51 bytes sends a short
packet (50 bytes and a spare byte), `cf f` plus 201 bytes sends a long one.
Back to the H8S: `03` when a packet went out, `32` plus 50 bytes or `c8` plus
200 bytes for a packet heard, and `13`/`11` for XOFF/XON. A packet starts with
the 4-byte destination (`ffffffff` for everyone) and the sender's ID.

## Debug console

The boot loader and CyOS print text on serial port SCI1. The firmware logs those lines on the ESP32 serial monitor with the
`cyos` tag.

## Desktop app

`cybiko` runs the emulator in a window on a PC, with sound and the MP3
cartridge. It uses a folder laid out like `/Cybiko` on the board's SD card
(ROM and flash dumps, `ramdisk.bin`, `apps/`, `card/`, `cybiko.cfg`), so you
can use a copy of that folder, or the card itself.

On Ubuntu (26.04 has SDL3 packaged):

```sh
sudo apt install build-essential cmake libsdl3-dev
cmake -S src/host -B build-host -DHOST_UI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-host --target cybiko -j
build-host/cybiko ~/Cybiko            # the folder with the dumps; --zoom N for the window size
```

Without `libsdl3-dev` CMake downloads SDL3 and builds it, which needs the
X11/Wayland and ALSA/PulseAudio development packages.

Keys: letters are letters. Enter, Esc, Space, Tab and the arrows are
themselves. Backspace or Delete is Del, Shift is Shift and Ctrl is Fn.
F1-F7 are F1-F7, F8 or Home is Select, F9 is Help, F10 or Insert is Ins, F11
or the menu key is Menu and F12 is On/Off. `, ; .` are themselves, `[` and `]`
are the Cybiko's `(` and `)` keys, and `/` is its `!` key. The
window keeps integer zoom when resized. Closing it saves the RAM disk, which
is also saved every 30 seconds. A reprogrammed flash is saved as `flash.bin`.

## PC runner

`cybiko_host` runs the machine headless on a PC with the real dumps. It is
built with the other host tools:

```sh
cmake -S src/host -B build-host && cmake --build build-host
build-host/cybiko_host --rom cyrom150.bin --flash cyos_v1508.bin \
    --seconds 30 --screenshot screen.pgm --frames frames/ --press enter@8000
```

It prints the text on SCI1 to stdout and bytes on SCI0 to stderr as hex, then a summary: emulation speed and anything the model saw but
does not cover (unmapped accesses, undefined opcodes, DMA DREQ use, watchdog
resets). `--ramdisk FILE` loads and saves a RAM disk as on the board,
`--disk-size KB` makes it bigger as the firmware does (the board uses 4096),
`--install FILE` copies a file onto it first, and `--ls` lists its files at
the end. `--pair RAMDISK2` runs a second machine with its own RAM disk and
links the two radios (`--press b:key@ms` presses keys on the second one, and
its screenshot is written next to the first with `.b.pgm`); `--unit-id HEX`
sets the first machine's Cybiko ID. `--card FOLDER_OR_IMAGE` plugs in the
MP3 cartridge with that card (`--cart` without a card), `--wav FILE` records
its sound, and `--cart-log` traces every cartridge access.

## Speed on the board

While an MXD clip plays, CyOS runs about 3.2 million H8S instructions a
second. The PC runner needs about 100 host instructions for each one, and
the ESP32-S3 spent about 375 cycles on each one when it ran the core from
flash. Most of that was waiting on memory. The 32 KB interpreter does not
fit the S3's 16 KB instruction cache, the flash is read in DIO mode, and
ESP-IDF's `-fno-jump-tables` turned every switch into a chain of compares.

`CYBIKO_CORE_IRAM` (menuconfig, "Cybiko emulator", on by default) runs the
interpreter and the machine's per-instruction paths from internal RAM, with
real jump tables. It costs about 49 KB of internal RAM, which the Palm 68k
JIT cache then does not get. Turn it off if Palm speed matters more.

These `sdkconfig` settings may help too. Each one is untested on this board:

| Setting | Effect | Cost |
| --- | --- | --- |
| `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` (instead of DIO) | twice the flash bandwidth on cache misses | none, if the module's flash supports QIO (the ESP32-S3-WROOM-1 does); the board fails to boot if it does not |
| `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y` | fewer code cache misses | 16 KB of internal RAM |
| `CONFIG_ESP32S3_DATA_CACHE_64KB=y` | more of the emulated RAM cached | 32 KB of internal RAM |
| `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y` | faster sequential PSRAM reads | none |

Change them in `idf.py menuconfig`, or delete `sdkconfig.jc4827w543` after
editing `sdkconfig.defaults.jc4827w543`. The serial log prints the speed
every 5 seconds.

## Status and limits

- CyOS boots on the PC runner; speed and sound on the board still need
  checking. Instruction timing counts each memory access as the bus
  controller sets it up (8- or 16-bit areas, wait states, DRAM), so the CPU
  does as much work per second as the real one. A few hot routines run as
  native code (cy_hle.c) with their original timing: the CyOS 1.5.08 page
  checksum, which the RAM disk check at start-up spends about half its time
  in, the boot ROM's software multiply, the boot ROM's bitmap row blit and
  the CyOS loop that sends rows to the LCD. The last two make games about
  3 times cheaper to run.
- Software standby (CyOS switching itself off) stops the CPU. Waking it with
  On/Off assumes the button is wired to IRQ7 (PA7), which is a guess.
- The DMA controller and the DTC are modelled (CyOS uses both at start-up).
  Software DTC activation and the DREQ pins are not; the runner counts DREQ
  use.
- The USB controller ignores writes.
- The bigger RAM disk is found by matching the RAM disk set-up code of CyOS
  1.5.08. With another CyOS version that code may differ; the disk then stays
  at 512 KB, and the serial monitor says "CyOS RAM disk set-up not recognised".

## Future improvements

- **MP3 cartridge.** It is emulated (see [MP3 cartridge](#mp3-cartridge)) and
  tested in the PC runner; the open questions are listed in
  [cybiko-mp3-cartridge.md](cybiko-mp3-cartridge.md#6-open-questions).
- **Mesh network and internet gateway.** Cybikos formed a mesh, and one device
  connected to a PC could share internet access, including the WAP browser,
  with all the others. The radio now works over ESP-NOW; the next step is to let a
  board with Wi-Fi act as the PC gateway, set up through a web portal (the
  firmware already has one for Palm networking, see [Wi-Fi](wifi.md)) and later
  perhaps a native Cybiko setup app.
