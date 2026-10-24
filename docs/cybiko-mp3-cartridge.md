# The MP3 cartridge in the emulator

The Cybiko Xtreme's MP3 player cartridge (CY45P01) is emulated: its SD card
slot, its MP3 decoder and the MXD video clips. This page is about using it
here and how the emulator models it. The cartridge itself (pins, I2C decoder
control, the SD card over SPI with DMA, its software, pitfalls for anyone
building one) is documented in cybiko-docs'
[mp3-cartridge.md](https://github.com/SovGVD/cybiko-docs/blob/main/mp3-cartridge.md); the `.cap`, `.app` and MXD
clip formats in [file-formats.md](https://github.com/SovGVD/cybiko-docs/blob/main/file-formats.md).

## Using it

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
   - What the Cybiko does on the card (deleting, writing or renaming files,
     making folders) is saved to the folder each time the RAM disk is saved.
     A changed file's old contents stay in a hidden `.cyold*` file until
     the next start. Up to 512 KB of writes per start fit in memory.

   If you prefer a real disk image, which the Cybiko writes directly, make one
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

To make your own clips, see [Making MXD clips](#making-mxd-clips). On a PC, `cybiko_host --card FOLDER_OR_IMAGE --wav
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
| `tools/cybiko_mxd.py` | converts a video into an MXD clip, and a clip into an MP4 |

## How the emulator models it

Where the cartridge's behaviour is not known, or the emulator stands in for
hardware, it does this:

- **Detection**: ADC channel 3 reads 0 while the cartridge is in, 3ff
  without.
- **Decoder**: [minimp3](https://github.com/lieff/minimp3) decodes whatever
  MPEG audio it can; the real part is not known. Every I2C byte to 0x3c/0x3d
  is ACKed and reads give 0, which the player accepts. DEMAND only rises
  after the first I2C message is ACKed ("the decoder is on").
- **Volume**: codec register 0x10 scales the output by 1 dB per step
  relative to 0x6a, with 0 meaning silence. The real curve is not known.
- **DEMAND and pacing**: a 2 KB FIFO; DEMAND drops when fewer than 64 bytes
  are free and rises again when it is half empty. Junk the decoder skips
  plays as silence for as long as it would last at the last bit rate seen
  (128 kbps before the first frame). The firmware paces the decoder's input
  by emulated time and lets the sound have gaps when the emulation falls
  behind: draining in real time while the emulation ran at about 10% speed
  starved the player and the watchdog reset the Cybiko (reproduced on the PC
  by draining 12 times faster than the stream).
- **SD card**: the model answers CMD0, CMD1 (MMC), CMD9, CMD10, CMD12,
  CMD13, CMD16, CMD17, CMD18, CMD24, CMD25, CMD55/ACMD41, CMD58 and CMD59,
  and sends correct CRC7/CRC16 though the driver does not check them. The
  CSD it reports is version 1.0 with 512-byte blocks, so at most 1 GB.
  Writes are modelled but untested.
- **The card**: `cy_vfat` makes up a FAT16 disk on the fly from the
  `/Cybiko/card` folder; long names are folded to ASCII, because CyOS hides
  files with other characters. Writes stay in memory.

## Making MXD clips

`tools/cybiko_mxd.py VIDEO OUT.app --template flavor.app`
does the following:

1. Runs ffmpeg to scale the video to 160 × 100 (crop or pad), at 5 fps in
   grey.
2. Dithers each frame to four levels (Floyd-Steinberg by default).
3. Encodes the sound track as 64 kbps mono MP3, or 128 kbps stereo with
   `--audio hifi`, with no ID3 or Xing header.
4. Makes an icon from a frame a third of the way in.
5. Writes `root.inf`, and packs all five parts into a CyOS archive. Only
   `clip.pic` is compressed, the way the CD does it.

`tools/cybiko_mxd.py CLIP.app OUT.mp4` goes the other way. It plays the
frames at the clip's own frame rate over its sound track, scaled 4 × with
sharp pixels (`--zoom N`), so the CD's clips can be watched on a PC.

The clip player (`main.e`) is Cybiko's code, so it is not in this
repository: `--template` copies it from a CD clip. Clips of 30-40 s are
what Cybiko shipped and what was tested.
