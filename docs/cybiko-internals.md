# How the emulator models the Cybiko Xtreme

How this project's Cybiko Xtreme emulator is built: which file models which
part of the machine, the choices it makes where the hardware is not known,
and what it adds that the real machine does not have. The user guide is
[cybiko.md](cybiko.md).

The machine itself (chips, memory map, keyboard matrix, screen, radio
co-processor, flash, boot sequence, RAM disk format) is documented in
[cybiko-docs](https://github.com/SovGVD/cybiko-docs):
[hardware](https://github.com/SovGVD/cybiko-docs/blob/main/hardware.md),
[CyOS](https://github.com/SovGVD/cybiko-docs/blob/main/cyos.md),
[file formats](https://github.com/SovGVD/cybiko-docs/blob/main/file-formats.md),
[radio protocol](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md),
[MP3 cartridge](https://github.com/SovGVD/cybiko-docs/blob/main/mp3-cartridge.md).
Most of it was found with this emulator.

The machine model follows MAME's `cybikoxt` driver and its H8 peripheral
models (Tim Schuerewegen, Olivier Galibert). The code is in
`src/components/cybiko/`, the CPU core in `src/components/h8s/`.

## Where each part is

| Part | Files | Notes |
|---|---|---|
| Memory map, bus timing, interrupt controller, ports | `cybiko.c`, `include/cybiko.h` | `cy_bus_timing`, `port_input`, `port_output` |
| Keyboard | `cybiko.c` (`keyboard_read`), key codes in `include/cybiko.h` | |
| LCD (HD66421) | `cy_devices.c`, `cybiko.c` (`cybiko_lcd_render`) | |
| Real-time clock (PCF8593) | `cy_devices.c` | |
| Speaker | `cy_periph.c` (`cy_tpu_speaker_tone`), `cybiko.c` (`speaker_update`) | |
| SCI, TPU, TMR, WDT, A/D, DTC | `cy_periph.c` | |
| DMA controller | `cy_dma.c` | |
| Radio co-processor (AVR) | `cy_rf.c` | |
| Flash (SST39VF400A) and the unit ID | `cy_devices.c`, `cy_rf.c` | |
| CyOS update install | `cybiko_flash_stage_boot` | |
| RAM disk | `cy_disk.c` | |
| Standby | `cybiko.c` (`cybiko_run`) | |
| MP3 cartridge | `cy_cart.c`, see [cybiko-mp3-cartridge.md](cybiko-mp3-cartridge.md) | |
| Native routines | `cy_hle.c` | see below |

## Choices the emulator makes

Where the hardware is not known, or a host stands in for it:

- **USB controller** (200000-200003): writes are ignored, reads give 0.
- **Key matrix**: as on a real Cybiko, a row's keys show only when the bus
  has held the row's address since the read before. The model counts a DMA
  or DTC read of the same address as the previous one as held. A CPU read
  never counts, because instruction fetches come in between. CyOS reads
  each row with a 12-read DMA burst, so its reads see the keys from the
  second read on. A lone CPU read of the matrix sees no key, as on the real
  machine (the first cybiko-doom did that; the real machine gave no key or
  noise, the model always gives no key). Whether a CPU loop running from
  on-chip RAM would see the keys on the real machine is not known; the
  model says no. See cybiko-docs'
  [hardware.md](https://github.com/SovGVD/cybiko-docs/blob/main/hardware.md#reading-the-matrix-on-a-real-cybiko).
- **DRAM timing**: fast page mode costs 2 states within a row and 4 on a
  row change; the emulator counts 3 on average.
- **Battery** (PA6): always reported charged. The other A/D channels read
  0, except channel 3 (cartridge detect) and channel 0 (radio signal, see
  the radio).
- **LCD shades**: `level = 31 − (palette − contrast + 3)`, clamped to 0-31,
  0 black and 31 white. It gives four distinct shades at every contrast
  CyOS offers; the real curve is not known.
- **Clock**: runs from the host's wall clock; when CyOS sets the time the
  difference is stored (`state.bin` on the board).
- **Speaker**: TPU tones are computed from the timer registers; for tones
  CyOS makes by toggling P15 in software the emulator counts the edges over
  each slice of emulated time and turns the rate into a tone.
- **Radio**: received bytes are paced at air speed (2400 B/s) and ADC
  channel 0 reports a signal, or CyOS never selects a gate. The model only
  delivers a packet while SCI2 is running and listening (module stop).
  Packets go between boards over ESP-NOW
  ([cybiko-radio-protocol.md](cybiko-radio-protocol.md)).
- **Unit ID**: every copy of a flash dump has the same ID, so the emulator
  writes a fresh ID and CRC at start-up (on the board, from the ESP32's MAC,
  `rf.id`) and puts the original back before saving the flash.
- **CyOS updates** (`.boot`): installed by putting the file where the
  kernel's header was (601864 in the 1.5.08 dump) and letting the flash
  loader's own code run. `avr*.boot` is skipped: the AVR is not emulated.
- **Standby**: the timers stop and the speaker is silenced while the H8S is
  in software standby; IRQ7 (On/Off) wakes it. The WOVF flag is kept across
  a watchdog reset. CyOS's watchdog resets have only been seen when the
  emulator starved it of time.
- **RAM disk on the SD card**: files in `/Cybiko/apps/` are written into the
  RAM disk image while the machine is off; CyOS finds them on its start-up
  scan.

## What the emulator adds

These are not on the real machine. They are here so nobody mistakes them for
hardware:

- **Bigger RAM disk** (800000-dfffff, up to 6 MB). CyOS 1.5.08 creates its
  RAM disk driver with "2000 blocks of 256 bytes, 512 KB at 400000". When the
  loader prints `Starting 0x`, the emulator finds that code in the freshly
  unpacked kernel and changes the base, size and block count, so CyOS gets a
  bigger disk in the same format. A 512 KB disk image keeps working.
- **Native routines** (`cy_hle.c`). Hot pieces of code are swapped for C:
  the RAM disk page checksum (half of all instructions until the desktop
  appears), the boot ROM's 32-bit multiply, the boot ROM's row blit (draws
  one row of a 2-bit bitmap: copy, transparent copy through a mask table, or
  XOR, optionally mirrored; about 60% of all instructions in a scrolling
  game), the CyOS loop that sends a 40-byte row to the LCD (about 20%), the
  CyOS loop that copies bytes out of a file's 8 KB buffer (a quarter of all
  instructions while a video plays), a CyOS memset loop, the boot ROM's
  memset and its loop that fills a row of a bitmap, a copy loop and a clear loop in a CyOS module
  (the file system code that runs for each file on the MP3 card while the
  desktop loads; with 150+ files they were most of the work), the boot ROM's LZ
  decompressor (its bit-by-bit decoding loop, 44% of all instructions in a
  video, and its back-reference copy), the boot ROM's loop that turns 1-bit
  icons and text into 2-bit pixels (most of the work while the desktop draws),
  and two loops in a CyOS module the
  video player uses: one sends a buffer to the MP3 decoder on SCI0 waiting
  for each byte to go out, the other spins until an interrupt handler
  clears a flag. The send loop runs its bytes in one go while nothing else
  can happen before the last one: it works out SCI0's frame timing, status
  bits and the slice end as the CPU would, and goes back to the bus at the
  next event of any other peripheral. The checker still finds about 1% of
  calls a few states off (an event's DMA/DTC work lands a little earlier or
  later), so this one is not exact. The first word of
  each is replaced by an opcode the H8S does not have; the core traps on
  it, runs the C version and charges the states the original takes, so
  emulated time is unchanged. Loops stop at the top of an iteration when the
  slice ends or an interrupt is pending, and the trap resumes them. All but
  the blit leave registers, flags, memory and states exactly as the original
  (unit tests in test_cybiko.c check this); the blit's states are within a
  few percent. A loop that only waits for an interrupt (the spin loop) lets
  the rest of the slice go by in whole rounds, since nothing it reads can
  change before an interrupt is taken. They are found by matching code
  (the decompressor also by a hash of the ROM code and jump tables it
  follows), so other ROM or CyOS versions just run unpatched. Code in CyOS
  modules, loaded after start-up, is looked for when it first runs: a write
  to SCI0's transmit register or a DMA start, and a look at the program
  counter every 50 ms of emulated time, make the emulator scan the 64 KB page
  around that code (and 32 KB either side) for the module patterns, each page
  at most once every 10 s. A trap met at an address it did not place (the
  module was loaded again elsewhere) is taken up if the code around it
  matches. The PC runner built with
  `-DCY_HLE_CHECK` runs every 200th call (`CY_HLE_CHECK_EVERY`) both ways
  and compares registers, flags, states and all of RAM (the spin loop one
  round at a time); `--blit-fuzz N` adds N random blit calls.
- **Polling skips.** When the same instruction reads a timer counter or a
  serial status register again within 256 states and sees the same value,
  the time until the register can change passes at once (CyOS's delay and
  clock loops, the boot loader printing, the MP3 player waiting for each
  byte to go out). Timers are caught up only when a timer register is
  touched or a timer event is due.
- **Whole SPI blocks at once.** When DMA feeds SCI1 in clocked sync mode
  (the MP3 card's sector reads and writes) and nothing but the DMA end can
  interrupt, the emulator moves the bytes up to the end of the shorter DMA
  block in one step instead of one event per byte. Each byte still costs the
  same states and goes through the same DMA and card code. Serial frames
  that no interrupt or DMA waits on are not events at all; they finish when
  a register is read or the next real event comes.
- **The slice ends when it must.** A register write that brings an event
  closer (a timer compare, a serial start, a DMA stop) shortens the current
  slice to that event, instead of ending it, so the CPU runs on to the next
  event without a round trip through the scheduler.
- **Unit ID** from the board's MAC (above).
- **Radio over ESP-NOW** ([cybiko-radio-protocol.md](cybiko-radio-protocol.md)).
- **The MP3 card as a folder**: the card is made up on the fly from
  `/Cybiko/card` ([cybiko-mp3-cartridge.md](cybiko-mp3-cartridge.md)).

Open questions about the hardware are kept in cybiko-docs. If you have a
real Cybiko and can check any of them, please open an issue there.
