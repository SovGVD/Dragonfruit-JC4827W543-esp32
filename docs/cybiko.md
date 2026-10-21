# Cybiko Xtreme

The JC4827W543 firmware also emulates the **Cybiko Xtreme** (2001): a Hitachi
H8S/2323 at 18.432 MHz, 2 MB RAM, 512 KB flash and a 160×100 four-shade LCD.
Pick **Cybiko** in the [boot menu](jc4827w543.md#boot-menu).

The hardware model follows MAME's `cybikoxt` driver. It has been tested with
small H8S programs (`src/host/test_cybiko.c`) but **not yet with the real
ROM**, because no ROM can be shipped here. Expect fixes once it has been run
with real dumps: please run the [PC runner](#pc-runner) and send its output.

## SD card

Everything for the Cybiko lives in `/Cybiko` on the card. You supply both
images. They are the MAME `cybikoxt` set:

| File | Size | Accepted names (first match wins) |
| --- | --- | --- |
| Boot ROM | 32 KB | `bootrom.bin`, `cyrom150.bin`, `xtreme-rom.bin` |
| CyOS flash | 512 KB | `flash.bin`, `cyos_v1508.bin`, `xtreme-flash.bin` |

The firmware writes these itself:

```text
ramdisk.bin   the 512 KB battery-backed RAM disk where CyOS keeps user files
state.bin     the Cybiko clock, so the time survives restarts
flash.bin     only if CyOS reprograms its flash (an OS update); being the
              first flash name, it then replaces the original image
cybiko.cfg    optional settings, written by you (see below)
```

The RAM disk is saved within 30 seconds of a change, once the screen has not
been touched for 2 seconds, and when you hold BOOT to leave. To start from an
empty RAM disk, delete `ramdisk.bin`.

## Controls

The LCD is shown at 2× (320×200) in the top-left corner. Touch uses the
calibration from `/PalmOS/palm.cfg`, so calibrate once in Palm mode if taps
land in the wrong place.

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

- **MORE** shows F1–F7, AS and ON/OFF; **ABC** goes back.
- The touchscreen sees one finger at a time, so **SHIFT** and **FN** latch: tap
  one, then the key. They release with that key.
- **BOOT** is the Cybiko On/Off key. **Hold BOOT for 3 seconds** to save and
  restart to the boot menu.

## Sound

The Cybiko beeper plays through the board's speaker with the same `audio`
component as Palm. The tone is taken from the timer that drives the speaker
pin (TPU channel 1, TIOCB1), or measured from how fast software toggles the
pin. Set the volume in `cybiko.cfg`:

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
rf.port = 0      # serial port to carry, 0 or 1 (2 is the debug console)
rf.channel = 1   # Wi-Fi channel; use the same one on every board
```

Once the protocol has been worked out from the ROM, this layer is meant to
emulate the radio co-processor so that Cybikos on different boards can see
each other.

## Debug console

CyOS prints to its debug serial port (SCI2, 57600 baud). The firmware logs
those lines on the ESP32 serial monitor with the `cyos` tag.

## PC runner

`cybiko_host` runs the machine headless on a PC with the real dumps. It is
built with the other host tools:

```sh
cmake -S src/host -B build-host && cmake --build build-host
build-host/cybiko_host --rom cyrom150.bin --flash cyos_v1508.bin \
    --seconds 30 --screenshot screen.pgm --frames frames/ --press enter@8000
```

It prints the debug console to stdout and bytes on the other serial ports to
stderr as hex, then a summary: emulation speed and anything the model saw but
does not cover (unmapped accesses, undefined opcodes, DTC/DMA use, watchdog
resets). `--ramdisk FILE` loads and saves a RAM disk as on the board.

## Status and limits

- Not yet run with the real ROM (see above).
- Software standby (CyOS switching itself off) stops the CPU. Waking it with
  On/Off assumes the button is wired to IRQ7 (PA7), which is a guess.
- The DTC and DMA controllers are not modelled; the runner counts any use.
- The USB controller ignores writes.

## Future improvements

- **MP3 module.** The Xtreme had an optional MP3 player add-on (CY65P10).
  Emulating it through the board's speaker is planned but has low priority.
- **Mesh network and internet gateway.** Cybikos formed a mesh, and one device
  connected to a PC could share internet access, including the WAP browser,
  with all the others. The plan is to emulate the radio over ESP-NOW and let a
  board with Wi-Fi act as the PC gateway, set up through a web portal (the
  firmware already has one for Palm networking, see [Wi-Fi](wifi.md)) and later
  perhaps a native Cybiko setup app.
