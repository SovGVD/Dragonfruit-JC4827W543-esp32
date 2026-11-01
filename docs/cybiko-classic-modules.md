# Cybiko Classic: the MP3 player and the memory card

The first Cybiko, the Classic, had two add-ons for the 68-pin slot on its
back: the **MP3 player** (CY65P10) and the **memory card** (sold as the
"Cybiko 1Mb Memory Card", or Expansion Memory). Only one of them fits at a
time, because they share the slot. Neither is emulated yet. This page collects
what is known about them, so that they can be emulated later. The Xtreme's MP3
cartridge is a different device; it is described in
[cybiko-mp3-cartridge.md](cybiko-mp3-cartridge.md).

Status, 2026-11-01: research only. Sources are listed at the end; what is a
guess is marked as one.

## The expansion slot

The slot uses a PCMCIA Type 1 connector, but not PCMCIA signals ("PCMCIA
connector but not PCMCIA signals"). 33.6K PCMCIA modems were seen by CyOS,
which reported modem errors; they did not work. The pinout, from the
hallikainen.org page:

| Pins | Signals |
| --- | --- |
| 1, 34, 35, 68 | GND |
| 17, 51 | 3.27 V |
| 30-32, 3-7, 64-66, 36-40 | D0-D15, the CPU's data bus |
| 63-56, 46, 45, 42, 44, 55, 47, 48, 54, 53, 12-14 | A0-A19, the CPU's address bus |
| 43, 49 | /RD, /WR |
| 24 | /RS, reset |
| 41 | /RC, expansion RAM chip select |
| 10 | /C0, the internal DataFlash's chip select (CPU P3.4, pin 12) |
| 9, 27, 25, 26 | SCK, SIN, SOT, RDY/BSY: the internal DataFlash's serial lines |
| 2, 19 | /C1, /C2, expansion flash chip selects 1 and 2 |
| 21, 23, 22 | SC2, SI2, SO2, the expansion flash's serial clock, in and out |

So the slot carries both the CPU's parallel bus, with a chip select for
expansion RAM, and a second serial DataFlash bus with two chip selects. In
the emulator the internal DataFlash is an AT45 on SCI1, chip select P3.4 (see
[cybiko-internals.md](cybiko-internals.md#the-classic)). SC2/SI2/SO2 are very
likely SCI2, the port the emulator treats as CyOS's console today. That is a
guess, but a strong one: the H8S/2241's third SCI is the only serial port
left.

## The memory card

- It adds **256 KB of static RAM and 1 MB of DataFlash** (Wikipedia: "256
  kilobytes of static RAM, and 1 megabyte of data flash memory"). The hardware
  table on hallikainen.org says "Flash disk: 512 KB, extendible up to 1 MB".
- "Some Cybiko programs will not run unless the Expansion Memory is plugged
  in" (Wikipedia), so programs could use the extra RAM.
- From the pinout: the SRAM is on the parallel bus behind /RC, and the flash
  is a serial DataFlash on SC2/SI2/SO2 behind /C1 (and /C2 for a second chip).
  The hallikainen page links an AT45D081 data sheet next to these pins, so the
  chip is most likely a 1 MB AT45DB081: the same family as the internal
  AT45DB041, and the same chip the emulator already uses as the Classic's
  internal flash.
- **Which CyOS supports it.** CyOS 1.2.46 (the MAME dump) has a single flash
  device class, `OnboardFlash`, and no other flash device, card or RAM-module
  strings in RAM after it boots. Support most likely came with a later System
  Pack (CyOS 1.2.57 or 1.3.x for the Classic). This is to be checked once that
  CyOS is at hand (see [What is needed](#what-is-needed)).
- **Plan for emulating it.** Map 256 KB of RAM at the area /RC selects (found
  by watching where the newer CyOS probes). Put a second AT45DB081 model on
  SCI2 behind /C1, sharing the code in `cy_classic.c`, and save it as its own
  file next to the internal flash. On the board this could also be the
  `/Cybiko/Classic` folder's "card" image.

## The MP3 player (CY65P10)

The player is a whole computer of its own, powered by the Cybiko through the
slot:

| Part | Chip |
| --- | --- |
| CPU | Hitachi H8S/2246 (HD6472246, 16-bit, 11 MHz), with its own firmware inside |
| MP3 decoder | Micronas MAS3507D-F10 (MPEG 1 and 2, layers 2 and 3, 8-384 kbit/s) |
| DAC | Micronas DAC3550A |
| USB | National USBN9603, full speed, to a mini-USB socket |
| Storage | one SmartMedia (SSFDC) card, 4 to 64 MB, FAT12 |
| Extended edition | FM tuner (Philips TDA7021T, TDA7040T stereo decoder) and a microphone for voice recording |

How it worked, from the Cybiko MP3 Player manual pages:

- It plays on its own buttons (six keys and a two-colour LED). The Cybiko MP3
  application, loaded with CyberLoad, adds "extended control, including an
  equalizer and play-list editing", and the buttons keep working while it
  runs.
- It needs System Pack v.56 or later on the Cybiko ("CyOS v.1.2.x" on the
  spec sheet). The Cybiko must be off when the player is plugged in or out.
- Music gets onto the SmartMedia card over the player's own USB. Windows shows
  the card as a removable disk; on the Mac, the Cybiko MP3 Loader is used. On
  insertion the player builds a play-list of the tunes it finds.
- The card can also hold Cybiko games and applications. So CyOS can read files
  from the SmartMedia card through the player, much as the Xtreme reads apps
  from its MP3 cartridge's card.
- Lite edition: MP3 only. Extended edition: also FM radio and voice
  record/playback.

**How the Classic talks to it is not documented.** Its own CPU and firmware do
the decoding, USB and card work, so the Classic only sends commands and
reads or writes files. That goes either over the parallel bus or over the
SCI2 lines. Emulating it means implementing that command protocol (HLE). The
player's H8S/2246 has its program in on-chip ROM that nobody has dumped, so
running its real firmware is not an option. The protocol has to be read out of
the Classic side: the MP3 application and CyOS's driver for the player.

The CyOS 1.2.46 dump has a deleted file named `AppHackMP3.app` in its flash.
Its blocks have been reused, so only the name is left. It shows the player's
software existed for CyOS 1.2. `mplayer.app` in the same dump is the `.mus`
melody player, not MP3.

**Plan for emulating it.**
1. Get the Classic's MP3 application and the System Pack that supports the
   player (see below).
2. Find its driver's commands by running it in the PC runner with I/O tracing
   on the bus and SCI2.
3. Answer them from a model in the style of `cy_cart.c`: the SmartMedia card
   as a FAT folder (`cy_vfat`, as for the Xtreme's cartridge), and MP3 frames
   to the existing `cy_mp3` decoder.
4. The player's own buttons go on the on-screen keyboard. The FM radio and
   the voice recording are left out.

## What is needed

1. **The Classic's later CyOS** (System Pack v.56 or later). ReBiko's
   `tools/rebiko_cyos_update.py` installs CyOS 1.2.57 for the Classic from
   Cybiko's `Cybiko-AutoUpdate.exe`; its notes say kn100.me keeps a copy. That
   copy is the only source found so far.
2. **The Classic MP3 application.** The cybiko.net mirror's `Applications/`
   folder has "MP3 (200x)(Cybiko Inc.).app", which is likely it. It still has
   to be checked that it is the Classic's and not the Xtreme's.
3. Neither site can be reached from the sandbox the emulator is built in, so
   the files have to be downloaded by hand. They also belong in
   [cybiko-roms](https://github.com/SovGVD/cybiko-roms) as a backup.

## Sources

- [Cybiko expansion connector pinout](https://pic.hallikainen.org/techref/cybiko/expansion.htm), hallikainen.org
- [CY65P10 MP3 player for Cybiko Classic](https://pic.hallikainen.org/techref/cybiko/CY65P10.htm), hallikainen.org
- [Cybiko hardware](https://pic.hallikainen.org/techref/cybiko.htm), hallikainen.org (specifications, "Flash disk: 512 KB, extendible up to 1 MB")
- [MP3 Player](http://dbzoo.com/cybiko/mp3player), DBZoo (chips, editions, SmartMedia sizes)
- [Mp3 Player](https://bluecybiko.tripod.com/bluecybiko/id4.html), BlueCybiko (the manual: System Pack v.56, USB, play-list, the shared slot with the 1Mb Memory Card)
- [Cybiko](https://en.wikipedia.org/wiki/Cybiko), Wikipedia (memory card: 256 KB SRAM and 1 MB DataFlash)
- MAME's `cybiko.cpp` lists the MP3 player's chips; it has no driver for the player or the memory card
- [ReBiko](https://github.com/RetroCodeRamen/rebiko): `tools/rebiko_cyos_update.py` (System Pack to CyOS 1.2.57)
- The [DBZoo memory expansion unit page](https://www.dbzoo.com/cybiko/memoryexpansionunit) exists but could not be read (server error)
