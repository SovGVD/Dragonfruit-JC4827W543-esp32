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
| MP3 player | Not emulated: two of its libraries are missing (see [below](#the-mp3-player-cy65p10)) |

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
| a second flash select | P1.4, for a card with two flash chips |
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
  `classic.card = 0`. Its flash is `/Cybiko/Classic/card.bin` (1 MB), saved
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

How to use it:

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

**What the uploads showed.** Gleb's `MP3 (200x)(Cybiko Inc.).app` (backed up
as `classic/MP3.app` in cybiko-roms) is the Classic's MP3 application,
v1.3.13, for CyOS 1.2.56 and later, with MP3, FM and Voice modes. It needs
two libraries it does not have: `MP3.dl` (1.1.7) and `mp3_control.dl`
(1.1.3). They are in neither System Pack 57 nor the three Cybiko SDKs (2010
Std, 2112 Std, 3012 Pro: compilers, help and examples only; their
`FlashDevices.c` example names `MP3_DRIVE_NAME` next to the A and B flash
drives). Most likely they came on the player's own CD or card.

**So the player is not emulated.** How the Classic talks to it is in those
two libraries. With them, the plan is to trace their bus and pin accesses in
the PC runner, answer them from a model in the style of `cy_cart.c` (the
SmartMedia card as a FAT folder via `cy_vfat`, MP3 frames to `cy_mp3`), and
put the player's buttons on the on-screen keyboard. The player's own
firmware is in its CPU's on-chip ROM, which nobody has dumped, so running it
is not an option.

**Still wanted:** `MP3.dl` and `mp3_control.dl` (the MP3 player's CD, or a
SmartMedia card from one).

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
