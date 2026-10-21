# Cybiko Xtreme

The JC4827W543 firmware also emulates the **Cybiko Xtreme** (2001): a Hitachi
H8S/2323 at 18.432 MHz, 2 MB RAM, 512 KB flash and a 160×100 four-shade LCD.
Pick **Cybiko** in the [boot menu](jc4827w543.md#boot-menu).

The hardware model follows MAME's `cybikoxt` driver. It is tested with small
H8S programs (`src/host/test_cybiko.c`), and with the real `cybikoxt` dumps
CyOS 1.5.08 boots to its first-run setup (welcome, nickname, contrast, clock)
in the [PC runner](#pc-runner). No ROM is shipped here.

## SD card

Everything for the Cybiko lives in `/Cybiko` on the card. You supply both
images. They are the MAME `cybikoxt` set:

| File | Size | Accepted names (first match wins) |
| --- | --- | --- |
| Boot ROM | 32 KB | `bootrom.bin`, `cyrom150.bin`, `xtreme-rom.bin` |
| CyOS flash | 512 KB | `flash.bin`, `cyos_v1508.bin`, `xtreme-flash.bin` |

Games and apps go in `/Cybiko/apps` (see [Games and apps](#games-and-apps)).
The firmware writes these itself:

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

The Xtreme's MP3 player add-on also stored files, on a SmartMedia card. It
was a separate computer (its own H8S CPU and firmware, linked through the
expansion connector), and CyOS 1.5.08 has its memory card support stubbed
out, so it is not emulated; the bigger RAM disk takes its place.

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
+-------------------------+
|   Cybiko LCD (160x100)  |
| F1 F2 F3 F4 F5 F6 F7 AS |
|      UP     | ESC  HELP |
| LEFT SEL RIGHT| TAB DEL |
|     DOWN    |  ON/OFF   |
| Q W E R T Y U I O P     |
| A S D F G H J K L .     |
| SHIFT Z X C V B N M FN  |
| SPACE           ENTER   |
+-------------------------+
```

Landscape (480×272), LCD at 2×:

```text
+--------------------------+----------------+
|                          | ESC   UP  HELP |
|  Cybiko LCD (x2)         | LEFT SEL RIGHT |
|                          | TAB  DOWN  DEL |
|                          | ENTER   SPACE  |
+--------------------------+----------------+
| Q  W  E  R  T  Y  U  I  O  P              |
| A  S  D  F  G  H  J  K  L  .              |
| SHIFT Z  X  C  V  B  N  M  FN  MORE       |
+-------------------------------------------+
```

- In landscape, **MORE** shows F1–F7, AS and ON/OFF; **ABC** goes back.
- The touchscreen sees one finger at a time, so **SHIFT** and **FN** latch: tap
  one, then the key. They release with that key. While one is on, the keys
  show what they type: capitals with SHIFT, and with FN the digits on Q-P and
  the symbols CyOS puts on the other letters (`@ } (C) $ % * + - _ = > < & { #
  "` and `/` on ".").
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

## Radio (experimental)

A real Cybiko talks to its radio through a co-processor on a serial port. That
protocol is not documented, so the radio is not emulated yet. As a first step,
the firmware can carry the raw bytes of one serial port between boards over
ESP-NOW, which is useful for studying the traffic but will not make CyOS chat
work on its own. It is off by default:

```ini
rf = espnow      # default: off
rf.port = 0      # serial port to carry, 0 or 1 (1 also carries the console text)
rf.channel = 1   # Wi-Fi channel; use the same one on every board
```

Once the protocol has been worked out from the ROM, this layer is meant to
emulate the radio co-processor so that Cybikos on different boards can see
each other.

## Debug console

The boot loader and CyOS print text on serial port SCI1 (SCI2 is a debug
port too). The firmware logs those lines on the ESP32 serial monitor with the
`cyos` tag.

## PC runner

`cybiko_host` runs the machine headless on a PC with the real dumps. It is
built with the other host tools:

```sh
cmake -S src/host -B build-host && cmake --build build-host
build-host/cybiko_host --rom cyrom150.bin --flash cyos_v1508.bin \
    --seconds 30 --screenshot screen.pgm --frames frames/ --press enter@8000
```

It prints the text on SCI1 and SCI2 to stdout and bytes on SCI0 to stderr as
hex, then a summary: emulation speed and anything the model saw but
does not cover (unmapped accesses, undefined opcodes, DMA DREQ use, watchdog
resets). `--ramdisk FILE` loads and saves a RAM disk as on the board,
`--disk-size KB` makes it bigger as the firmware does (the board uses 4096),
`--install FILE` copies a file onto it first, and `--ls` lists its files at
the end.

## Status and limits

- CyOS boots on the PC runner; speed and sound on the board still need
  checking.
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

- **MP3 module.** The Xtreme had an optional MP3 player add-on (CY65P10).
  Emulating it through the board's speaker is planned but has low priority.
  Its storage is covered by the bigger RAM disk (see [Storage](#storage)).
- **Mesh network and internet gateway.** Cybikos formed a mesh, and one device
  connected to a PC could share internet access, including the WAP browser,
  with all the others. The plan is to emulate the radio over ESP-NOW and let a
  board with Wi-Fi act as the PC gateway, set up through a web portal (the
  firmware already has one for Palm networking, see [Wi-Fi](wifi.md)) and later
  perhaps a native Cybiko setup app.
