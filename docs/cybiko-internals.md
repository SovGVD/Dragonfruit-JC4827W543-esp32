# How the Cybiko Xtreme works

A technical reference to the Cybiko Xtreme (2001) as this project emulates
it: the chips, the memory map, how the screen, keys, clock, sound and radio
are wired, how it boots, and how CyOS stores files. It is written for anyone
who wants to write their own emulator, poke at a real machine or just
understand the thing. The user guide is [cybiko.md](cybiko.md); the MP3
cartridge, the `.app` archive format and MXD video clips have their own
document, [cybiko-mp3-cartridge.md](cybiko-mp3-cartridge.md).

Nothing here comes from Cybiko Inc. documents. The sources are MAME's
`cybikoxt` driver and its H8 peripheral models (Tim Schuerewegen, Olivier
Galibert), the Hitachi H8S/2323 hardware manual, the chip datasheets, MAME's
imgtool, and running CyOS 1.5.08 in this emulator and watching what it does.
Each statement is marked:

- **Verified**: CyOS 1.5.08 or the boot ROM was seen doing this in the
  emulator, or it was checked against a dump.
- **MAME / datasheet**: taken from MAME or a datasheet and not contradicted by
  CyOS so far, but not separately checked.
- **Guess**: a reasonable reading that nothing has confirmed.

Unmarked lines describe what the emulator does, which is a fact about this
code, not about the hardware. The code is in `src/components/cybiko/`: the
file named at the start of each section has the details.

## Contents

1. [The machine at a glance](#1-the-machine-at-a-glance)
2. [Memory map](#2-memory-map)
3. [Bus timing](#3-bus-timing)
4. [Interrupts](#4-interrupts)
5. [I/O ports and pins](#5-io-ports-and-pins)
6. [Keyboard](#6-keyboard)
7. [Screen: HD66421](#7-screen-hd66421)
8. [Real-time clock: PCF8593](#8-real-time-clock-pcf8593)
9. [Sound](#9-sound)
10. [Serial ports and the debug console](#10-serial-ports-and-the-debug-console)
11. [Radio](#11-radio)
12. [Flash and the unit ID](#12-flash-and-the-unit-id)
13. [Boot sequence](#13-boot-sequence)
14. [The RAM disk](#14-the-ram-disk)
15. [Power and standby](#15-power-and-standby)
16. [Other on-chip peripherals](#16-other-on-chip-peripherals)
17. [What the emulator adds](#17-what-the-emulator-adds)
18. [Open questions](#18-open-questions)

## 1. The machine at a glance

| Part | Chip | Notes |
|---|---|---|
| CPU | Hitachi H8S/2323 | 18.432 MHz, advanced mode (24-bit addresses), mode 7 |
| Boot ROM | 32 KB | `cyrom150.bin`, boot loader v1.5.0 |
| RAM | 2 MB DRAM | at 400000; the first 512 KB is kept alive by the backup battery |
| Flash | SST39VF400A, 512 KB | `cyos_v1508.bin`: CyOS, its loader and the unit ID |
| Screen | Hitachi HD66421 | 160×100, 2 bits a pixel (four shades) |
| Clock | Philips PCF8593 | on a bit-banged I2C bus |
| Radio | AT90S2313 AVR plus an RF chip | the AVR talks to the H8S over SCI2 |
| Speaker | piezo on P15 | square waves from TPU channel 1 or from software |
| Keyboard | matrix read through the bus | at e00000 |
| Cartridge | rear connector on port 2 | the MP3 player module (see the cartridge doc) |

**MAME / datasheet** for the chip list. The clock rate, mode and RAM size are
**Verified**: the boot ROM prints the detected DRAM size, and CyOS's timing
(clock, sound pitch, radio baud rate) only comes out right at 18.432 MHz.

The CPU is an H8S/2000 core: sixteen 16-bit registers that pair into eight
32-bit ones (ER0-ER7, ER7 is the stack pointer), big-endian, with a 16 MB
address space. Every peripheral (timers, serial ports, A/D, DMA, interrupt
controller) is part of the chip; the board adds only the devices in the
table. The CPU core is `src/components/h8s/`.

## 2. Memory map

File: `cybiko.c`, `include/cybiko.h`.

| Address | Size | What |
|---|---|---|
| 000000-03ffff | 32 KB, mirrored | boot ROM |
| 100000-100001 | 2 bytes | HD66421 LCD: 100000 index register, 100001 data |
| 200000-200003 | | USB controller (the emulator ignores writes, reads 0) |
| 400000-5fffff | 2 MB | DRAM. 400000-47ffff is the RAM disk |
| 600000-7fffff | 512 KB, mirrored | SST39VF400A flash, 16 bits wide |
| 800000-dfffff | up to 6 MB | **not real hardware**: the emulator's bigger RAM disk ([§17](#17-what-the-emulator-adds)) |
| e00000-efffff | | keyboard matrix ([§6](#6-keyboard)) |
| ffdc00-fffbff | 8 KB | on-chip RAM |
| fffe80-ffffff | | on-chip I/O registers |

**MAME / datasheet**, with the flash and RAM ranges **Verified** (the boot
ROM scans flash at 600000, CyOS runs from RAM at 4xxxxx).

The address space is split into eight 2 MB "areas" for bus timing (area *n*
starts at *n* × 200000). Area 0 is the ROM, area 2 the DRAM, area 3 the
flash.

Notable I/O registers outside the peripherals (H8S/2323 manual):

| Address | Register |
|---|---|
| fffec4-fffece | IPRA-IPRK, interrupt priorities |
| fffed0 / fffed1 | ABWCR, ASTCR (bus width, 2/3-state access per area) |
| fffed2-fffed3 | WCRH/WCRL (wait states per area) |
| fffed4-fffed5 | BCRH/BCRL (BCRH low bits pick area 2 as DRAM) |
| fffeb0-fffebf | port data direction registers (write only) |
| ffff2c-ffff2f | ISCR (IRQ edge/level), IER, ISR |
| ffff38 | SBYCR (standby control) |
| ffff39 | SYSCR |
| ffff3b | MDCR, reads 87: mode 7 |
| ffff3c-ffff3d | MSTPCR (module stop: 1 = peripheral switched off) |
| ffff50-ffff5f | port input pins (PORT1-PORTG) |
| ffff60-ffff6f | port output registers (DR) |

## 3. Bus timing

File: `cybiko.c` (`cy_bus_timing`).

How long a memory access takes is set per area by three registers:

- **ABWCR** bit *n*: area *n* is 8 bits wide (a 16-bit access takes two
  cycles).
- **ASTCR** bit *n*: area *n* uses 3-state cycles, otherwise 2-state.
- **WCR** two bits per area: 0-3 wait states added to a 3-state cycle.

After reset every area is 8-bit, 3-state with 3 waits (all registers ff),
which is why the boot ROM's first job is to set the bus up. Area 2 becomes
DRAM when BCRH says so; DRAM with fast page mode takes 2 states within a row
and 4 on a row change, counted by the emulator as 3 on average (**Guess**
for the average). The boot ROM and on-chip RAM take one state. **MAME /
datasheet** for the rules; the DRAM detection is **Verified** in that the
boot ROM prints "10-bit shift" and "2048K".

Getting these costs right matters: CyOS's busy-wait delays and sound pitch
depend on them.

## 4. Interrupts

File: `cybiko.c` (interrupt controller).

The H8S has 92 vectors, each a 32-bit address in a table at 000000 (so in
the boot ROM). Priorities come from the IPR registers, a 3-bit level per
group; the CPU takes an interrupt when its level is above the I2/I1/I0 mask
in EXR (interrupt control mode 2) or simply when not masked by CCR (mode 0).

| Vector | Source | Used for |
|---|---|---|
| 0 | reset | |
| 7 | NMI | |
| 8-11 | TRAPA #0-3 | software traps |
| 16-23 | IRQ0-IRQ7 pins | IRQ2: MP3 cartridge; IRQ7: On/Off button |
| 25 | WOVI | watchdog interval |
| 28 | ADI | A/D conversion done |
| 32-59 | TPU channels 0-5 | timers; channel 1 makes sound |
| 64-71 | TMR0, TMR1 | |
| 80-91 | SCI0-SCI2, four each (ERI, RXI, TXI, TEI) | cartridge audio, console, radio |

IRQ pins can be level or edge triggered (ISCR, two bits per pin: low level,
falling, rising, both). The On/Off button is wired to PA7, which doubles as
IRQ7 (**MAME / datasheet**); it is the only way out of software standby
([§15](#15-power-and-standby)). Interrupts can also start DTC or DMA
transfers instead of running code ([§16](#16-other-on-chip-peripherals)).

## 5. I/O ports and pins

File: `cybiko.c` (`port_input`, `port_output`).

| Pin | Direction | Connected to |
|---|---|---|
| P15 | out | speaker ([§9](#9-sound)) |
| port 2 | both | cartridge connector: I2C to the MP3 decoder, SD chip select, card detect |
| P66 | in | MP3 decoder DEMAND line |
| PA6 | in | 1 = battery charged (the emulator always reports charged) |
| PA7 | in | On/Off button, 0 = pressed (also IRQ7) |
| PF1 | out | RTC I2C clock (SCL) |
| PF6 | both | RTC I2C data (SDA): pulled low while PF6 outputs 1 or floats |
| A/D channel 3 | in | cartridge detect: 0 with a cartridge in, 3ff without |

The I2C wiring of the RTC (an inverted data line) follows MAME and is
**Verified** in that CyOS reads and sets the time correctly. Port 2 and
P66 are described pin by pin in the
[cartridge doc](cybiko-mp3-cartridge.md#2-the-hardware). Pins a port does
not have read as 1 (port 3 bits 6-7, port 5 bits 4-7, port G bits 5-7).

## 6. Keyboard

File: `cybiko.c` (`keyboard_read`), key codes in `include/cybiko.h`.

The keyboard is a matrix read through the address bus. A 16-bit read at
e00000 + 2 × *sel* returns the keys of every row whose bit in *sel* is 0,
ANDed together, active low (a pressed key reads 0). CyOS scans rows 0 to 9
one at a time by clearing a single bit in the low ten bits of *sel*; higher
bits are left 0. **Verified.**

| Row | Bit 0 | Others |
|---|---|---|
| 0 | F7 | M 8, ) 9, "," 10, K 11, I 12, O 13, L 14 |
| 1 | F6 | G 1, B 2, N 3, H 4, Y 5, U 6, J 7 |
| 2 | F5 | D 8, C 9, ( 10, V 11, F 12, R 13, T 14 |
| 3 | F4 | Q 1, A 2, Z 3, X 4, S 5, W 6, E 7 |
| 4 | F3 | Enter 3, Select 4, Menu 5, Space 6 |
| 5 | F2 | Tab 7, Del 8, Ins 9, Esc 10 |
| 6 | F1 | Up 11, Right 12, Down 13, Left 14 |
| 7 | | Fn 15 |
| 8 | | Shift 15 |
| 9 | Help | . 1, ! 2, ; 3, P 4 |

MAME puts Help, "." and P on address line A13 (row 13). On CyOS that cannot
work: because CyOS leaves the higher address bits low while scanning, a key
in row 13 shows up in every row it scans. Row 9 is the only place they
behave, so they are in row 9 here (**Verified** that row 9 works; the real
wiring is a **Guess**).

Menu, Ins, `( ) , ; !` are not in MAME. They were found by pressing every
free position of rows 0-9 in a CyOS text field and seeing what it typed
(**Verified**); no other free position does anything. Ins is the key MAME
calls "As". What Fn and Shift add on these keys:

| Key | Fn | Shift |
|---|---|---|
| . | / | \ |
| , | ' | ~ |
| ; | : | \| |
| ! | ? | ^ |
| ( | [ | ` |
| ) | ] | a smiley |

On/Off is not in the matrix: it is PA7/IRQ7.

## 7. Screen: HD66421

File: `cy_devices.c` (LCD), `cybiko.c` (`cybiko_lcd_render`).

The HD66421 is a 160×100 controller with 2 bits per pixel. It has an index
register at 100000 and a data port at 100001: write the register number to
100000, then read or write its value at 100001.

| Register | Meaning |
|---|---|
| 0 | control; bit 6 = display on |
| 1 | control; bit 1 picks auto-increment of x (1) or y (0) |
| 2 | x address, in bytes (0-39) |
| 3 | y address, in rows (0-99) |
| 4 | display RAM data: writes go to (x, y), then the address steps |
| c-f | palette: the drive level for pixel values 0-3 |
| 10 | contrast |

Display RAM is 100 rows of 40 bytes, four pixels to a byte with the leftmost
pixel in the top two bits. **RAM row 0 is the bottom line of the screen**
(**Verified**: CyOS's screens come out upside down otherwise). When x passes 39 it wraps
to 0 and y steps; y wraps from 99 to 0.

The shade of each pixel value is its palette level against the contrast. The
emulator uses `level = 31 − (palette − contrast + 3)`, clamped to 0-31, with
0 black and 31 white. It gives four distinct shades at every contrast setting
CyOS offers, which is all that has been checked: the formula is a **Guess**. With bit 6 of register 0 clear the screen is blank.

## 8. Real-time clock: PCF8593

File: `cy_devices.c` (RTC).

The PCF8593 sits on a software I2C bus (PF1 clock, PF6 data). Its registers
1-6 hold hundredths, seconds, minutes, hours, day plus year-mod-4, and month
plus weekday, all in BCD; bit 7 of the hours register selects 12-hour mode.
The chip only knows the year modulo 4, so CyOS keeps the rest itself. The
emulator runs the clock from the host's wall clock, and when CyOS sets the
time it stores the difference. **MAME / datasheet**, **Verified** that CyOS
shows the right date and time.

## 9. Sound

File: `cy_periph.c` (`cy_tpu_speaker_tone`), `cybiko.c` (`speaker_update`).

The speaker is a piezo on P15, which is also TPU channel 1's output TIOCB1.
CyOS makes tones two ways (**Verified**):

- **TPU**: channel 1 in PWM mode 2 (TGRA period, TGRB duty) or toggling
  TIOCB1 on each compare match. The frequency is the timer clock divided by
  the period (halved when toggling).
- **Software**: flipping P15 in a loop. The emulator counts the edges over
  each slice of emulated time and turns the rate into a tone.

There is one square-wave voice and no volume control on the real machine
(**Guess** for the volume). The MP3 cartridge has its own decoder and
headphone output.

## 10. Serial ports and the debug console

File: `cy_periph.c` (SCI).

The H8S has three serial ports (SCI0-2), each asynchronous or clocked
synchronous, with the baud rate from SMR and BRR.

| Port | Used for |
|---|---|
| SCI0 | MP3 cartridge: MPEG data to the decoder (clocked) |
| SCI1 | debug console (async); in clocked mode, the cartridge's SD card |
| SCI2 | the radio AVR, 36 kbaud 8N1, fed by the DTC |

On SCI1 in async mode the boot loader prints its progress and CyOS prints
log lines. The boot text is in [§13](#13-boot-sequence). **Verified.** On the
real machine these pins come out on the serial/USB connector (**Guess** for
which pins).

## 11. Radio

File: `cy_rf.c`.

The radio is an Atmel AT90S2313 microcontroller that drives the RF chip and
talks to the H8S over SCI2 at 36 kbaud, 8N1. CyOS's RF driver is a kernel
object called `rfdriver` (in RAM at 49b900-49c760 once the kernel runs).
CyOS sends to the AVR with the DTC. Worked out from CyOS 1.5.08, so the
byte values are **Verified**, the meanings partly **Guess** as marked:

From the H8S to the AVR:

| Bytes | Meaning |
|---|---|
| `01 02 ch` | tune to channel *ch* (CyOS uses 2) |
| `01 03 xx xx` | unknown setting (CyOS sends `00 00`) |
| `30 f` + 51 bytes | send a short packet: 50 bytes plus a spare byte the DTC count includes; *f* is 0 or 1, meaning unknown |
| `cf f` + 201 bytes | send a long packet: 200 bytes plus the spare byte |

From the AVR to the H8S:

| Bytes | Meaning |
|---|---|
| `03` | the packet went out. CyOS waits up to 50 ms for a short packet and 100 ms for a long one |
| `32` + 50 bytes | a short packet was heard |
| `c8` + 200 bytes | a long packet was heard |
| `13` / `11` | XOFF / XON while CyOS is sending |

Every packet starts with the 4-byte destination ID (`ffffffff` for
broadcast) and the 4-byte sender ID. The rest is CyOS's own network
protocol, which the emulator passes through untouched: emulating the AVR is
enough for CyOS's chat, games and file transfer to work between machines.
This emulator carries packets between boards over ESP-NOW (see
[cybiko.md](cybiko.md#radio)).

## 12. Flash and the unit ID

File: `cy_devices.c` (flash), `cy_rf.c` (ID).

The SST39VF400A is a 512 KB, 16-bit NOR flash. Its commands are the usual
JEDEC unlock sequences at word addresses 5555 and 2aaa: `aa 55 a0` then a
word programs it, `aa 55 80 aa 55 10` erases the chip, `aa 55 80 aa 55 30`
erases a sector, `aa 55 90` reads the ID (maker 00bf, device 2780).
**MAME / datasheet**; CyOS's own writes work.

The flash holds the second-stage loader, the compressed CyOS image and, at
the top, a per-machine record:

| Offset | What |
|---|---|
| 7f800 | magic `ba0bab15` |
| 7f818 | the unit's 32-bit Cybiko ID (its radio address) |
| 7fffc | CRC-32 over the record |

The boot ROM and the kernel both check the CRC. **Verified.** Every machine
needs its own ID or they cannot tell each other apart on the radio, so the
emulator writes a fresh ID and CRC at start-up (on the board, from the
ESP32's MAC) and puts the original back before saving the flash.

## 13. Boot sequence

What the boot loader prints on SCI1 with the `cybikoxt` dumps
(**Verified**):

```
Starting CyOS boot loader v1.5.0
Detecting DRAM interface settings... 10-bit shift
Detecting DRAM size... 2048K
Scanning flash: plain image found
Preparing to load CyOS
Got header: magic 1234ABCD : plain image
Starting 0x6001c8...
Got header: magic = 1C0FFAB, compressed_size = 117922, plain_size = 216348: LZSS compressed image
decompressing...
Got header: magic = 1234ABCD, compressed_size = 4865016, plain_size = 6297700: plain image
Starting 0x4a3bf8...
```

So there are three stages:

1. **Boot ROM** (000000): sets up the bus, sizes the DRAM, scans the flash
   for an image header and jumps into flash at 6001c8. Images start with a
   magic word: `1234ABCD` for a plain image, `1C0FFAB` for an LZSS-compressed
   one.
2. **Flash loader** (6001c8): finds the LZSS-compressed kernel in flash
   (118 KB packed, 216 KB unpacked), unpacks it into RAM and jumps to it.
3. **CyOS kernel** (4a3bf8): starts its objects, scans the RAM disk
   ([§14](#14-the-ram-disk)), and opens the desktop or, on a blank RAM disk,
   the first-run setup (welcome, nickname, contrast, clock).

The sizes in the third header look like garbage (**Guess**: a header the
loader prints without using). The boot ROM also uses a shift-and-add 32-bit
multiply to unscramble data, which the emulator replaces with native code
([§17](#17-what-the-emulator-adds)).

## 14. The RAM disk

File: `cy_disk.c`.

CyOS keeps every user file (applications, games, messages, settings) in a
RAM disk: the first 512 KB of DRAM, at 400000, kept alive by the backup
battery. When the battery runs flat everything is gone. The format matches
MAME's imgtool `cybikoxt` module and was **Verified** against a RAM disk CyOS
wrote.

The disk is a row of **258-byte pages** from offset 0. Each page is a 16-bit
big-endian checksum and a 256-byte block. The checksum, over the 256 bytes
*b[0..255]*:

```c
uint32_t v = 0;
for (i = 0; i < 256; i++) {
    v = (v ^ b[i] ^ i) << 1;
    v |= (v >> 16) & 1;
}
checksum = (uint16_t)v;
```

The first 5 pages are not used for files. A block is in use when bit 7 of
its first byte is set and the checksum matches. A file block:

| Offset | Size | Meaning |
|---|---|---|
| 0 | 1 | flags; bit 7 = in use |
| 1 | 1 | number of data bytes in this block |
| 2 | 2 | file ID, big endian, the same in every block of a file: the number of its first block, counted from the first file page |
| 4 | 2 | part number, 0 for the first block |
| *part 0 only:* | | |
| 6 | 1 | attributes (20 for a plain file) |
| 7 | | file name, NUL terminated (up to about 58 characters) |
| 45 | 1 | always 7b in files CyOS wrote (meaning unknown) |
| 46 | 2 | parent folder ID, ffff at the top level |
| 4a | 4 | date: seconds since 1900-01-01, big endian |
| 4e | 178 | data |
| *later parts:* | | |
| 6 | 250 | data |

Blocks of a file can be anywhere on the disk and in any order; the part
numbers put them back together. CyOS rebuilds its tables by scanning every
page at start-up, so a file written into a RAM disk image while the machine
is off shows up on the next boot. That is how this project installs
applications from `/Cybiko/apps/`.

Applications themselves (`.app`) and libraries (`.dl`) are CyOS archives: a
`Cy` header, a directory of parts and LZ-compressed data. The format is in
[cybiko-mp3-cartridge.md §4.2](cybiko-mp3-cartridge.md#42-cyos-archives-app-dl).

## 15. Power and standby

File: `cybiko.c` (`cybiko_run`).

When the user switches the Cybiko off, CyOS does not cut the power: it sets
bit 7 of SBYCR and executes `SLEEP`, which puts the H8S in **software
standby**. All clocks stop, the DRAM keeps its contents, and only an
external interrupt (IRQ7, the On/Off button) starts the chip again. The
emulator stops the timers while in standby and silences the speaker.
**MAME / datasheet** for the mechanism; the emulator's test programs use it,
but switching CyOS off and on has not been checked step by step.

The watchdog (WDT) can reset the machine if software stops servicing it; the
emulator keeps the WOVF flag across that reset as the chip does. CyOS's
watchdog resets have only been seen when the emulator starved it of time
(see the cartridge doc's pitfalls).

## 16. Other on-chip peripherals

File: `cy_periph.c`, `cy_dma.c`. All **MAME / datasheet**, used by CyOS as
noted.

- **TPU** (6 channels, 16-bit): channel 1 makes sound; CyOS's system tick
  probably comes from another channel (**Guess**).
- **TMR** (2 channels, 8-bit, can chain into 16 bits).
- **WDT**: watchdog or interval timer (vector 25).
- **A/D**: 8 channels, 10 bits. Channel 3 detects the cartridge. Other
  channels read 0 here (**Guess**: one of them reads the battery).
- **DMA controller**: 2 channels, each two short-address subchannels (A, B)
  or one full-address channel, started by software, a peripheral interrupt
  or the DREQ pin. The cartridge's SD card uses it. Registers at fffee0 +
  16*c* (MAR, IOAR, ETCR for A and B), DMAWER ff00, DMATCR ff01, DMACR
  ff02+2*c*, DMABCR ff06.
- **DTC** (data transfer controller): moves a byte per interrupt using a
  descriptor in on-chip RAM, so a serial port can be fed without running
  code. CyOS feeds the radio's SCI2 with it.
- **Module stop** (MSTPCR): a set bit switches a peripheral off. CyOS uses
  it, so a peripheral must be ignored while stopped (the radio model checks
  that SCI2 is running and listening before delivering a packet).

## 17. What the emulator adds

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
  video, and its back-reference copy), and two loops in a CyOS module the
  video player uses: one sends a buffer to the MP3 decoder on SCI0 waiting
  for each byte to go out, the other spins until an interrupt handler
  clears a flag. The first word of
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
- **Unit ID** from the board's MAC ([§12](#12-flash-and-the-unit-id)).
- **Radio over ESP-NOW** ([§11](#11-radio)).
- **The MP3 card as a folder**: the card is made up on the fly from
  `/Cybiko/card` (see the cartridge doc).

## 18. Open questions

- The real wiring of Help, "." and P (row 9 works, MAME says A13).
- What the radio's flag byte *f* and the `01 03` command mean.
- Which A/D channel reads the battery, and its scale.
- The meaning of byte 45 (always 7b) in a RAM disk file header.
- The exact HD66421 shade formula.
- Which pins of the serial/USB connector carry SCI1.

If you have a real Cybiko and can check any of these, please open an issue.
