# The Cybiko Xtreme MP3 cartridge and MXD video

The MP3 player cartridge (model CY45P01, sold in 2001) plugs into the Cybiko
Xtreme's expansion port. It holds an SD/MMC card slot, an MP3 decoder chip and
a headphone jack. With it the Cybiko plays MP3 files from the card, runs games
and apps from the card, and plays MXD clips: short videos in four greys with
an MP3 sound track. Cybiko published nothing about how the cartridge works,
and its software was only on the cartridge's CD.

This document covers everything this project learned about the cartridge, in
enough detail to build another emulator, a replacement cartridge, or new
content. It covers:

- the hardware as the Cybiko sees it: pins, buses, protocols and timing;
- the software CyOS runs for it, and how that software uses the hardware;
- the file formats: EZ Loader packs, CyOS archives with their compression,
  and MXD clips.

All of it was worked out from the files on the CD. The method was to run the
CD's drivers (`CartMan.dl`, `sddisk.dl`, `00000000.dl` and the others) in the
emulator, disassemble them, and model the hardware until the original MP3
Player and MXD clips worked. Nothing comes from Cybiko or chip documentation.

Each statement is marked by how sure it is:

- **Verified:** the original software works against this model in the
  emulator.
- **Observed:** this is what the software does, but nothing depends on it yet.
- **Guess:** reasoned from the evidence, not proven. These are called out
  where they appear.

Contents:

1. [Using it in this project](#1-using-it-in-this-project)
2. [The hardware](#2-the-hardware)
3. [The software on the Cybiko](#3-the-software-on-the-cybiko)
4. [File formats](#4-file-formats)
5. [Building your own: pitfalls](#5-building-your-own-pitfalls)
6. [Open questions](#6-open-questions)

## 1. Using it in this project

1. **Get the cartridge software onto the RAM disk.** It is in `MP3.cap` on the
   CD. Unpack it into `/Cybiko/apps` on the board's SD card, and the firmware
   copies it onto the RAM disk at start:

   ```sh
   python3 tools/cybiko_cap.py extract MP3.cap /path/to/sdcard/Cybiko/apps
   ```

2. **Fill the card.** Put `.mp3` files and MXD clips (`.app`) in
   `/Cybiko/card` on the board's SD card. The firmware creates the folder on
   its first start. It shows the folder to the Cybiko as the cartridge's
   card, so there is no disk image to make. Notes on the card:
   - Up to 1 GB is shown. Files past that are left out, and the serial log
     says how many.
   - Subfolders work. The MP3 Player only lists files at the top level.
   - Names are shown in ASCII. Accents are dropped and Cyrillic is
     transliterated, because CyOS does not list files whose names contain
     other characters.
   - What the Cybiko writes to the card is kept in memory until the next
     start. It never changes the folder.

   If you prefer a real disk image, which keeps the Cybiko's writes, make one
   with `tools/cybiko_card.py FOLDER mp3card.img` and put it at
   `/Cybiko/mp3card.img`. The image is used only while `/Cybiko/card` is
   empty.

3. **Plug the cartridge in.** By default the cartridge counts as plugged in
   when the card folder or the image has files. To change that, set `cart`
   in `/Cybiko/cybiko.cfg`:

   ```ini
   cart = auto   # default
   cart = mp3    # always plugged in, even without a card
   cart = off    # never
   ```

4. **Play.** "MP3 Player" appears on the desktop. It takes about 10 seconds
   to open, as on the real machine. MXD clips on the card appear as desktop
   icons. The volume slider works.

To make your own clips, see [MXD clips](#43-mxd-clips) and
`tools/cybiko_mxd.py`. On a PC, `cybiko_host --card FOLDER_OR_IMAGE --wav
out.wav` runs the whole thing headless, and `--cart-log` traces every
cartridge access (see [cybiko.md](cybiko.md#pc-runner)).

The emulation is in these files:

| File | What it does |
| --- | --- |
| `src/components/cybiko/cy_cart.c` | the cartridge's pins, I2C decoder control, SD card in SPI mode, DEMAND line |
| `src/components/cybiko/cy_dma.c`, `cy_periph.c` | DMA idle mode and SCI behaviour the SD driver relies on |
| `src/components/cy_mp3/` | the decoder: MPEG audio to PCM ([minimp3](https://github.com/lieff/minimp3), CC0) |
| `src/components/cy_vfat/` | a folder shown as a FAT16 card |
| `src/components/cybiko_app/cybiko_cart.c` | firmware glue: card, decoder task, speaker |
| `tools/cybiko_cap.py` | unpacks `.cap`/`.csu` packs and CyOS archives, and writes archives |
| `tools/cybiko_card.py` | builds a FAT16 card image from a folder |
| `tools/cybiko_mxd.py` | converts a video into an MXD clip |

## 2. The hardware

### 2.1 Overview

The Cybiko Xtreme's CPU is a Hitachi H8S/2323. The cartridge has no
processor of its own; CyOS drives every part of it directly through the
expansion port:

| Signal | H8S resource | Direction | Use |
| --- | --- | --- | --- |
| Cartridge ID | ADC channel 3 | in | which cartridge is plugged in |
| I2C SCL | P2.1 | out (open drain) | decoder control clock |
| I2C SDA | P2.3 | in/out (open drain) | decoder control data |
| SD chip select | P2.2 | out | low selects the card |
| Card detect | P2.0 | in | low while a card is in the slot |
| MPEG data | SCI0, clocked synchronous | out | compressed audio to the decoder |
| DEMAND | P6.6 and IRQ2 | in | the decoder wants more data |
| SD card | SCI1, clocked synchronous | in/out | the card in SPI mode |

Port 3 is also set up when the cartridge starts (P3DDR 0x33 / 0x11 and data
0x0c / 0x2e are written). Those bits set the SCI pin directions; nothing on
the cartridge seemed to need modelling for them. (Observed.)

### 2.2 Detection

`CartMan.dl` reads ADC channel 3 (Verified):

- **0x3ff (full scale):** the slot is empty. The pin is pulled up.
- **Below about 15:** cartridge ID 0, the MP3 player. The cartridge
  presumably ties the pin to ground through a resistor that sets its ID.
  Other IDs were apparently planned.

The emulator returns 0 when the cartridge is in.

### 2.3 The decoder and its I2C control port

The decoder is a Micronas MAS35xx MP3 decoder, controlled over I2C. CyOS
bit-bangs the I2C on P2.1 (SCL) and P2.3 (SDA). It releases a line by making
the pin an input and drives it low by making it an output that is low. That
is open drain, so a released line reads high unless the decoder pulls it
low. The decoder answers at address **0x3c** for writes and **0x3d** for
reads. (Verified.)

The manual gives the decoding range: MPEG 1, 2 and 2.5, layers 2 and 3,
8-320 kbps, 8-48 kHz. That fits the MAS 3507D, but the codec-register
writes below look more like the later MAS 3529/3539/3587 family. The exact
part is a guess either way; the emulator just decodes everything minimp3
can.

Each message is a start condition, the address byte 0x3c, a subaddress, the
data, then a stop condition. Subaddresses work like the MAS family's: 0x68 is
a DSP data write, 0x69 a DSP data read, 0x6a a control register write, and
0x6c a codec register write. The sequence the software sends (Observed;
exact order from `--cart-log`):

```text
3c 6a 80 00                   control register (meaning unknown; sent 6 times)
3c 6a 8c 00                   control register (meaning unknown; sent twice)
3c 68 70 00, then 3c 69, 3d   read the chip version (4 bytes; 0s are accepted)
3c 6c 00 00 00 01             codec register 0x00 = 0x0001
3c 6c 00 06 00 00             codec register 0x06 = 0x0000
3c 6c 00 07 40 00             codec register 0x07 = 0x4000
3c 6c 00 01 00 08             codec register 0x01 = 0x0008
3c 68 e0 00 00 01 03 4b 00 00 00 0c    DSP memory D0:0x34b = 0x0000c (one word)
3c 68 e0 00 00 01 03 46 00 00 00 25    DSP memory D0:0x346 = 0x00025
3c 6c 00 14 00 00             bass   = 0
3c 6c 00 15 00 00             treble = 0
3c 6c 00 1e 00 00             loudness off
3c 6c 00 11 00 00             balance = centre
3c 6c 00 10 6a 00             volume = 0x6a (the default)
3c 6c 00 10 00 00             volume = 0 (mute; sent 6 times, around starting and stopping)
```

**Codec register writes** take the form `6c 00 rr hh ll`, which writes
`hhll` to register `rr`. The registers the player uses (Verified that the
player writes them; their effect is a guess):

| Register | Meaning | Values seen |
| --- | --- | --- |
| 0x10 | volume, in the high byte | 0x00 mutes, 0x6a is the default, and the slider moves it in steps |
| 0x11 | balance | 0x00 is centre |
| 0x14 | bass | 0x00 |
| 0x15 | treble | 0x00 |
| 0x1e | loudness | 0x00 |

The emulator scales its output by 1 dB per volume step relative to 0x6a,
with 0 meaning silence. The real chip's curve is not known.

**Reads:** the version read is the only read the software makes. The player
accepts zeros. The emulator ACKs every byte sent to 0x3c/0x3d.

**Power:** the decoder's DEMAND line (below) only rises after the first I2C
message is ACKed, which is how the emulator models "the decoder is on".

### 2.4 Audio data: SCI0, DEMAND and IRQ2

The compressed MPEG stream goes to the decoder on **SCI0 in clocked
synchronous mode**, as plain bytes of the MP3 file. The decoder has a
**DEMAND** line, read on **P6.6** (bit 6 of port 6, address 0xffff55), that
is high while it can take more data. The same line drives **IRQ2**, set by
CyOS to trigger on the rising edge (ISCRL = 0xa0). (Verified.)

The driver works like this. Its IRQ2 handler is in `00000000.dl`, around
0x166e in this project's disassembly. (Verified against the emulator.)

1. The player task fills a ring buffer with MPEG data read from the card.
2. When DEMAND rises, IRQ2 fires. The handler sends **32-byte bursts** on
   SCI0, polling TDRE between bytes, for as long as P6.6 reads high.
3. If the ring buffer is empty while DEMAND is high, the handler sends
   **zeros** (from a zero buffer around 0x52a4) and sets SCI0's BRR to 1.
   The decoder skips them as junk.
4. The handler returns when DEMAND goes low. The next rising edge starts
   the next round.

A model has to behave like a real decoder in a few ways:

- **Lower DEMAND before the buffer fills, while a full burst still fits.**
  The handler checks DEMAND between bursts, not between bytes, so bytes
  sent into a full buffer are lost. The emulator keeps a 2 KB FIFO. It
  lowers DEMAND when fewer than 64 bytes are free and raises it again when
  the FIFO is half empty, which gives the rising edge IRQ2 needs.
- **Consume the data at the stream's real bit rate, junk included.** CyOS
  feeds zeros while its buffer is empty and stops only when the decoder
  stops asking. A decoder that skips junk instantly keeps DEMAND high
  forever, the handler never returns, and the watchdog resets the Cybiko.
  The emulator's decoder plays skipped bytes as silence for as long as they
  would last at the last bit rate seen (128 kbps before the first frame).
- **Take the data at playback speed.** If the decoder takes it faster, the
  player skips ahead. If it takes it slower, playback stutters.

### 2.5 The SD card: SPI over SCI1 with DMA

The card is used in **SPI mode** on **SCI1** in clocked synchronous mode.
SCMR is set so bytes go MSB first, as SPI needs. Chip select is **P2.2**,
active low. Card detect is **P2.0**, which reads low while a card is in.
(Verified.)

**Start-up** (Verified):

1. With chip select high, at least 80 clocks: 10 bytes of 0xff.
2. CMD0 with chip select low. The card answers R1 = 0x01 (idle).
3. CMD55 + ACMD41, repeated until R1 = 0x00. The model also answers CMD1,
   which MMC cards use instead of ACMD41. Whether the driver falls back to
   CMD1 for an MMC was not tested.
4. CMD9: read the CSD (a 16-byte data block) to get the card's size.
   CMD10 (CID), CMD13, CMD16 (block length 512), CMD58 and CMD59 are
   handled by the model too, in case another path uses them.

**Reading** (Verified):

- Addresses are **byte addresses** (SD version 1). The argument of CMD17
  or CMD18 is the block number × 512. SDHC block addressing is not known
  to the driver, so cards over 2 GB cannot work.
- Single blocks use **CMD17**. Runs use **CMD18**, followed by **CMD12**
  after the last block. The player was seen reading music in runs of 9
  blocks.
- The card's reply: R1, then 0xff bytes until the start token **0xfe**,
  then 512 data bytes, then 2 CRC bytes. The driver's token wait treats any
  byte of 0x0f or below as a data error token.
- CRCs are not checked, but the model sends correct CRC7/CRC16 anyway.

**Writing** (Observed: the model supports it, but nothing tested so far
writes):

- **CMD24** writes one block and **CMD25** writes several. Each block is
  token 0xfe (0xfc for CMD25), 512 data bytes and 2 CRC bytes. The card
  answers with a data response of 0x05 (accepted), then busy.
- Formatting the card on the Cybiko and copying files to it presumably
  use writes (a guess).

**DMA.** The driver moves block data with the H8S DMA controller (Verified):

- **Channel 0A** copies each received byte from SCI1's RDR to the buffer.
  SCI1's RXI triggers it.
- **Channel 0B** keeps the clock running. It is in **idle mode** (DTIE
  set): it writes a fixed 0xff to TDR again and again, with the memory
  address held fixed, and counts ETCR down. When the count ends it raises
  **DEND0B**.
- The DEND0B handler (around 0xee6 in this project's disassembly) clears
  DTE0B and DTIE0B, writes **SCR = 0** to stop SCI1, and sets a "done" flag
  that the read routine polls.

Three details of the H8S matter here. Getting any of them wrong breaks
reads:

- **Idle mode must count and end.** If channel 0B never raises DEND, the
  driver times out after about a second, then retries with CMD12 and a CMD9
  with argument 0xffffffff.
- **Switching a channel into idle mode reloads the full ETCR count.**
- **Writing SCR = 0 while a byte is being clocked must still finish that
  byte.** The last data byte would be lost otherwise, because the DEND
  handler stops the SCI before RXI has fired for it.

**Size limits:**

- The CSD the model reports is version 1.0 with 512-byte blocks, which can
  describe at most 1 GB.
- CyOS reads the card as FAT16. It reads 16 KB clusters correctly; at 32 KB
  clusters (which FAT16 needs above 1 GB) it reads wrong data. Tried in the
  emulator with a 2 GB card and a CSD using 1024-byte blocks.
- So **1 GB** is the practical limit. The manual's "any capacity" meant the
  cards of 2001: 8 to 128 MB.

**File system** (Verified with images from `tools/cybiko_card.py` and with
the generated folder view):

- An MBR with one partition (type 4 or 6) and FAT16 in it. Long file names
  are supported.
- CyOS mounts the card as `/MP3/`. File Manager shows it as "Removable
  Disk".
- The MP3 Player lists the `.mp3` files at the top level. Subfolders are
  readable but not in its playlist.
- A long name with any character outside ASCII hides the file. Latin-1
  letters were tried.
- Applications (`.app`) on the card get desktop shortcuts and run from the
  card. That includes MXD clips.

## 3. The software on the Cybiko

`MP3.cap` on the CD (unpacked with `tools/cybiko_cap.py`) holds the
cartridge's software. The descriptions below come from the files' names,
their imports, and what they were seen doing. Most of them are a guess:

| File | Role |
| --- | --- |
| `00000000.dl` | low-level driver loaded first: IRQ2 handler, SCI0 feeding, ring buffer |
| `CartMan.dl` | cartridge manager: reads the ADC ID and loads the drivers for it |
| `DeviceMan.dl`, `InterfaceMan.dl` | device and interface registry for add-ons |
| `sddisk.dl` | SD/MMC card driver: SPI commands, DMA, block reads and writes. Its function table, as loaded in the emulator at 0x581c, has the command send at +0x1c, the token wait at +0x28, the CSD read at +0x34 and command-with-data at +0x38 |
| `sdflash.dl` | FAT file system on the card (mounted as `/MP3/`) |
| `FSAgent.dl` | hooks the card into CyOS's file system and desktop |
| `format_rd.dl` | formats the card ("Format..." in File Manager) |
| `mp3init.dl` | start-up hook that brings the cartridge up |
| `MP3AudioStream.dl`, `MP3Player.dl`, `CyPlayer.dl` | the decoder control (I2C) and the streaming player engine |
| `sound.dl` | the sound library MXD clips use (version 21.1.1) |
| `Mp3Player.app` | the MP3 Player application |
| `root.ico`, `install.js` | the pack's icon, and the EZ Loader install script |

`install.js` puts `*.app`, `InterfaceMan.dl`, `mp3init.dl`, `CyPlayer.dl` and
`format_rd.dl` in the root of the Cybiko's disk, and the other `.dl` files in
`Libraries/`. CyOS 1.5.08 also finds them when they are all in the root,
which is what this project's `/Cybiko/apps` copy does. (Verified.)

CyOS itself (1.5.08 in flash) has no cartridge code that works on its own:
its memory card scan is a stub. Everything comes from these files.

## 4. File formats

All multi-byte numbers in the CyOS formats are **big-endian**, as on the
H8S. The EZ Loader pack is a PC format and is **little-endian**.

### 4.1 EZ Loader packs (`.cap`, `.csu`)

EZ Loader was the PC program that installed software over the USB cable.
`.cap` holds an application or a set of them; `.csu` is a system update and
has the same format. (Verified for the CD's packs.)

```text
offset  size  field
0       3     "CAP"
3       20    unknown (version 1, a timestamp-like value, a check value)
0x17    4     number of entries (u32 LE)
0x1b    ...   entries, then one bzip2 stream with all the entries' data
```

Each entry:

```text
u32 LE  type: 1 folder, 2 file, 3 script
u32 LE  size of its data in the bzip2 stream, XOR 0x55555555
u32 LE  time (unknown encoding)
u32 LE  attributes
...     name: UTF-16 LE, under the 16-bit XOR chain with key 0x8228, no terminator
u32 LE  unknown
```

The name has no length field. A reader finds its end by trying lengths
until the bytes after the name and the u32 look like the next entry's type
(1 to 3, then three zero bytes), or like "BZh" after the last entry.

**The XOR chain:** each plain unit is the cipher unit XOR the previous
plain unit, and the first is XORed with the key: `p[0] = c[0] ^ key`,
`p[i] = c[i] ^ p[i-1]`. It runs on 16-bit units with key 0x8228 for names
and scripts, and on bytes with key 0x82 for files.

After bunzip2, the entries' data follow one another in order. Each one
starts with a 12-byte header: a 32-bit value that is not a CRC32 or a sum
of the data (unknown), then zeros. After the header:

- A **file** (type 2) is bytes under the 8-bit chain with key 0x82. Undone,
  it gives the file exactly as the Cybiko stores it: a CyOS archive, an
  inner `.cap`, or a `.boot` image starting `ff c0 ff ab`.
- A **script** (type 3) is UTF-16 LE text under the 16-bit chain with key
  0x8228. It is JScript that EZ Loader ran on the PC (`install.js`,
  `update.js`).
- A **folder** (type 1) starts a group of the entries that follow.

### 4.2 CyOS archives (`.app`, `.dl`)

Applications, libraries and MXD clips are all one container. (Verified;
`tools/cybiko_cap.py` reads and writes it.)

```text
offset  size     field
0       2        "Cy"
2       2        n: number of parts
4       2        size of the directory below, in bytes = (offset of the first part's data) - 6
6       10 × n   directory: u16 name offset, u32 data offset, u32 stored size
...              names, each NUL-terminated
...              the parts' data, one after another
```

Offsets count from the start of the file. Each part's stored data starts
with a method byte:

- **0, stored:** the part's bytes follow.
- **2, compressed:** a u32 with the unpacked size follows, then the bit
  stream described below.

The order of the parts is free. Names are not sorted, and the CD's MXD
clips put `music.mp3` first.

**Method 2 compression** is LZ77 with fixed prefix codes. Bits are read
**least significant bit first** from each byte. A field of several bits is
put together **most significant bit first** from successive bits: for n
bits, `v = (v << 1) | next_bit()`, n times. The decoder loops until the
unpacked size is reached:

```text
bit 0  ->  literal: the next 8 bits are a byte
bit 1  ->  match: a length code, then an offset code;
           distance = offset + length; copy `length` bytes from `distance` back
```

Since the distance is at least the length, a match never overlaps itself.
Lengths run from 2 to 256 and offsets from 0 to 8191. In the code tables
below, "prefix" is the bits as read, followed by the extra bits as a number:

| Length prefix | Value | | Offset prefix | Value |
| --- | --- | --- | --- | --- |
| `0` + 1 bit | 2-3 | | `000` + 10 bits | 1024-2047 |
| `10` + 1 bit | 4-5 | | `001` + 9 bits | 512-1023 |
| `1100` | 6 | | `010` + 8 bits | 256-511 |
| `1101` + 1 bit | 8-9 | | `011` + 6 bits | 64-127 |
| `11100` | 7 | | `100` + 7 bits | 128-255 |
| `11101` + 1 bit | 10-11 | | `1010` + 11 bits | 2048-4095 |
| `1111000` | 12 | | `1011` + 12 bits | 4096-8191 |
| `1111001` | 13 | | `1100` + 5 bits | 32-63 |
| `1111010` + 2 bits | 20-23 | | `1101` + 4 bits | 16-31 |
| `1111011` + 1 bit | 16-17 | | `11100` + 1 bit | 0-1 |
| `1111100` + 3 bits | 24-31 | | `11101` + 2 bits | 6-9 |
| `1111101` | 14 | | `111100` + 1 bit | 4-5 |
| `11111100` + 5 bits | 32-63 | | `111101` | 2 |
| `11111101` + 1 bit | 18-19 | | `111110` + 2 bits | 12-15 |
| `11111110` | 15 | | `1111110` | 3 |
| `111111110` + 6 bits | 64-127 | | `1111111` + 1 bit | 10-11 |
| `1111111110` + 7 bits | 128-255 | | | |
| `1111111111` | 256 | | | |

`cy_unpack` and `cy_pack` in `tools/cybiko_cap.py` are a reference
decoder and a greedy encoder. Its output is within 4% of Cybiko's own
compressor, and CyOS loads it. (Verified.)

### 4.3 MXD clips

An MXD clip is a CyOS application whose archive holds five parts. (Verified
by playing all six clips from the CD, and clips made by `tools/cybiko_mxd.py`,
in the emulator.)

| Part | Method on the CD | Contents |
| --- | --- | --- |
| `music.mp3` | 0 | the sound track: a plain MP3 file |
| `clip.pic` | 2 | the frames |
| `main.e` | 2 | the player program: 4311 bytes, the same in every clip |
| `root.inf` | 2 | the application's description |
| `0.ico` | 0 or 2 | the desktop icon |

**`music.mp3`** is an MP3 stream that the cartridge decodes. The CD's clips
use 128 kbps 44.1 kHz stereo, or 64 kbps 22.05 kHz mono (`free.app`). An ID3
tag at the start is fine. It is read from the card and streamed to the
decoder like any MP3 file.

**`clip.pic`:**

```text
u16 BE  frame period, in units of 0.1 ms (2000 = 200 ms = 5 frames a second)
u8      width  (160)
u8      height (100)
then for each frame:
  u8, u8  0, 0   (x, y? always 0)
  u8, u8  160, 100 (width, height again)
  4000 bytes     the picture
```

The picture is 160 × 100 pixels at 2 bits per pixel, row by row from the
top. Each byte holds four pixels, the leftmost in the top two bits. Values
run from 0 (white) to 3 (black). That is the same layout as the Cybiko's
screen memory. (Verified.)

The **frame period** is what the player waits between frames. It was tested
by changing it: 4000 gave 400 ms frames. At 1000 (10 fps) the Cybiko could
not keep up and showed a frame every 500-700 ms. The CD's clips use 2000,
1818 and 1905, and are about 150 frames for 30 s. The frames are not synced
to the music: picture and sound start together and run side by side.
(Verified in the emulator, whose CPU runs at the real speed.)

**`root.inf`** is CRLF text, one field per line. For the clip "flavor":

```text
root
flavor            <- the name under the icon (followed by a space on the CD)
(empty)
(empty)
sound.dl          <- library the clip needs
(empty)
Video clip        <- the application type shown in File Manager
21.1.1            <- version of sound.dl needed
1.5.1             <- CyOS version needed (guess)
sound.dl<21.1.1>
```

**`0.ico`** is an image with an 8-byte header, then 2-bit pixels in the same
layout as `clip.pic`, each row padded to whole bytes:

```text
u8 2          bits per pixel
u8 1          number of images (guess)
u8 25, u8 18  width and height
u8 0, u8 0    (x, y? always 0)
u8 25, u8 18  width and height again
126 bytes     18 rows of 7 bytes
```

**`main.e`** is the clip player, in a CyOS executable format that this
project has not decoded. It reads `clip.pic` and `music.mp3` from its own
archive and uses `sound.dl` to start the cartridge. Because it is Cybiko's
code it is not in this repository. `tools/cybiko_mxd.py` copies it from a CD
clip given with `--template`.

**Making clips.** `tools/cybiko_mxd.py VIDEO OUT.app --template flavor.app`
does the following:

1. Runs ffmpeg to scale the video to 160 × 100 (crop or pad), at 5 fps in
   grey.
2. Dithers each frame to four levels (Floyd-Steinberg by default).
3. Encodes the sound track as 64 kbps mono MP3, or 128 kbps stereo with
   `--audio hifi`, with no ID3 or Xing header.
4. Makes an icon from a frame a third of the way in.
5. Writes `root.inf`, and packs all five parts into a CyOS archive. Only
   `clip.pic` is compressed, the way the CD does it.

Clips of 30-40 s are what Cybiko shipped and what was tested. Longer ones
should work, since nothing in the format limits the length, but they have
not been tried.

## 5. Building your own: pitfalls

These cost the most time while bringing the cartridge up. They apply to any
emulator of the cartridge, and some to the Cybiko in general:

- **The MP3 Player takes about 10 seconds to open.** Key presses before then
  are lost. "No sound" was this more than once.
- **Nested DTC transfers.** The H8S data transfer controller can start a
  transfer while another one's memory access is in progress. Keep the "DMA
  access in progress" state per transfer, saved and restored around nested
  ones. Otherwise the outer transfer is cut short, and SD block reads stall
  for a second until the driver's timeout.
- **DMA idle mode** must count down and raise DEND (see
  [2.5](#25-the-sd-card-spi-over-sci1-with-dma)).
- **SCR = 0 mid-byte** must complete the byte (see
  [2.5](#25-the-sd-card-spi-over-sci1-with-dma)).
- **DEMAND hysteresis and pacing** (see
  [2.4](#24-audio-data-sci0-demand-and-irq2)): without them bytes are lost,
  or the watchdog fires.
- **Test with several FAT layouts.** Cluster size and layout change which
  code paths the driver takes. The nested DTC bug only showed with some
  images.

## 6. Open questions

- **The decoder part number**, and the meaning of control register values
  0x8000 / 0x8c00 and DSP words D0:0x346 / 0x34b. A MAS 3507D or MAS 3587F
  datasheet would settle it.
- **The volume curve** of codec register 0x10.
- **The pack header** (bytes 3-0x16), the per-entry time and trailing u32,
  and the 32-bit value in each file's 12-byte header.
- **The `main.e` format:** decoding it would allow a free clip player, so
  clips would not need a CD file.
- **Whether frames can be smaller than full screen**, using the per-frame
  x, y, width and height fields.
- **SD writes on real cards** (formatting, copying to the card) are
  modelled but untested.
