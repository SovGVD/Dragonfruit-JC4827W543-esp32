# Palm OS: native ROM calls

The 68k core runs a few hot pieces of the Palm OS ROM as C instead of 68k
code (`src/components/m68k/m68k_palm_hle.c`). Each one leaves registers,
flags and memory exactly as the ROM code would, so Palm OS can't tell the
difference; it just takes fewer emulated instructions. Turn it all off with
`cpu.native = 0` in `/PalmOS/palm.cfg` (default 1).

## What runs natively

| ROM code | ROMs | What it does |
|---|---|---|
| TrapDispatcher | all | Every system call: looks the selector up in the trap table and jumps to the handler, leaving the same stack and registers |
| MemMove, MemSet | all | Copy/fill in C straight from the dispatcher, back to the caller |
| MemSemaphoreReserve/Release | all three | The Memory Manager's kernel semaphore and storage write protect, on every Mem* call |
| 32-bit multiply helper | all three | The compiler's multiply, linked into the ROM 17–21 times; every copy |
| PrvBltConstCopy, PrvBltSwap, PrvBlit1To8Copy, PrvGetBltInfo, PrvBltMisAlignCopy_Generic, PrvConvert1To8 | 4.1 | Colour blitter loops: fills, selection highlight, 1-bit to 8-bit text and icons, colour-table compare, misaligned copies |
| FntLineWidth, Crc16CalcBlock, DmFindResource scan | 4.1 | Text width, CRC-16, resource lookup |
| SysLaunch RAM clear | 3.1 | The byte-by-byte RAM clear at boot (40% of 3.1's boot) |
| ScrRectangleRoutine, PrvMisAlignedForwardInnerBitBlt (6 loops), PrvCompressedInnerBitBlt, ScrDeCompressScanLine | 3.1 | Mono blitter: fills, bitmap copies, compressed bitmaps |
| FntLineWidth, Crc16CalcBlock | 3.1 | Text width, CRC-16 |

"4.1" means the Palm OS 4.1 colour ROM and the IIIx 4.1 ROM (same code);
"3.1" the IIIx 3.1 ROM.

System calls only go native when the CPU is in supervisor mode, not tracing,
the trap is not a library trap, and the trap table still points at the ROM's
own handler (an app that patches a trap with `SysSetTrapAddress` keeps its
patch). MemMove/MemSet fall back to the ROM code when memory isn't plain RAM
or video RAM, or when the destination is NULL (so the ROM reports the error).
The semaphore functions only go native when nothing would have to wait or
switch tasks (the semaphore is free or already ours, the kernel has nothing
pending) and not from an interrupt; otherwise the ROM code runs.

Loops and functions are matched by their exact bytes (the semaphore code also
by a hash of all the kernel code it calls), and the first word of each is
replaced in the PSRAM copy of the ROM by an F-line opcode (`$FFxx`, which
Palm OS never executes). A different ROM simply keeps its own code; the boot
log says what was found:

```
I (m68k) native loop PrvBltConstCopy rows: on
I (m68k) native loop SysLaunch RAM clear: not in this ROM
I (m68k) native loop 32-bit multiply: on, 17 copies
I (m68k) native ROM calls: MemMove found, MemSet found
```

The emulated cycles charged match the ROM code exactly for the dispatcher
and within about 10% for the rest (most within a few percent). On the board
the Palm timer runs on wall-clock time, so this only affects the speed figure
in the log.

On the device the native code sits in flash, apart from the system-call entry
points, so it costs no internal RAM beyond those. After native code moves the
PC (to a system call's handler, a function's return address, a loop's exit)
the JIT's interpreter burst stops there, so the JIT profiles that address and
compiles it once it's hot instead of interpreting it.

## Effect

Instructions the 68k core executes on the PC build, with the workload below
(boot runs through first-boot Setup to the launcher, 290M cycles; apps are the
launcher, Date Book, Memo typing, Address, Calc and Note Pad):

| ROM | Boot: ROM code only | Native | Change | Apps: ROM code only | Native | Change |
|---|---|---|---|---|---|---|
| Palm OS 4.1 colour | 13.97 M | 6.49 M | −54% | 13.54 M | 6.44 M | −52% |
| IIIx 4.1 | 11.79 M | 6.42 M | −46% | 10.31 M | 6.00 M | −42% |
| IIIx 3.1 | 7.98 M | 2.16 M | −73% | 10.36 M | 5.67 M | −45% |

What's left on top is spread thin: the AMX kernel's event and timer code,
pen polling while the pen is down (a busy loop, no gain from speeding it up),
Memory Manager bookkeeping (PrvHandleCheck, PrvChunkNew) and PrvGetLineInfo,
which is straight-line code with no loop to take over.

## Checking it against the ROM

The PC build can run every Nth native call both ways: natively, then from
the same starting state through the ROM code with interrupts held off. It
compares all registers, the flags, every byte of RAM and video RAM and, for
the dispatcher, the cycle count. Leaf calls (MemMove/MemSet) are allowed to
differ in the scratch registers D1/A0/A1 (the dispatcher already clobbers
them) and the condition codes; leaf calls and native loops and functions may
leave different bytes in the dead stack below the final stack pointer.

Checked with every second call (`--hle-check 2`) over the whole workload, and
every call through the boot (`--hle-check 1`), on all three ROMs: no
differences.

```sh
cmake -S src/host -B build-host && cmake --build build-host --target dragonfruit
./build-host/dragonfruit -r palm.rom -c 1100000000 --script tools/palm-workload.txt --hle-check 3
...
native ROM calls: 111405 dispatches, 2859 MemMove/MemSet, 81152 loops; checked 96274, 0 mismatches; ...
  PrvBltConstCopy rows                             475 checked, cycles rom 4708270 native 5038586 (+7.0%)
  ...
```

The lines after the total give, per native loop or function, how many calls
were checked and the emulated cycles each way.

`--no-hle` runs the ROM code only.

## Finding the next hot spot

```sh
./build-host/dragonfruit -r palm.rom -c 1100000000 --script tools/palm-workload.txt \
    --profile run.prof --profile-from 290000000
python3 tools/palm_profile.py palm.rom run.prof --top 40          # by ROM function
python3 tools/palm_profile.py palm.rom run.prof --disasm MemMove  # with per-instruction counts
                                                                  # (the busiest one if a name repeats)
```

`--profile` counts instructions and cycles per guest PC; `palm_profile.py`
groups them by the MacsBug names in the ROM and disassembles a function with
`m68k-linux-gnu-objdump` (package `binutils-m68k-linux-gnu`).
`--script FILE` feeds pen taps and keys to a headless run, one per line:
`<cycle> tap X Y`, `<cycle> down X Y`, `<cycle> up`,
`<cycle> key ASCII [KEYCODE [MODIFIERS]]`. `tools/palm-workload.txt` walks
through first-boot Setup and then uses a few built-in apps.

## Speed log on the board

Every 5 seconds the Palm CPU task logs:

```
I (main) EMU 21.40 MHz while busy (129% of real), busy 35% | native/s: 1800 calls, 40 MemMove/MemSet, 150 loops | PC=... cyc=...
```

- **MHz while busy**: emulated 68k cycles per second while Palm OS was running
  code (not asleep in STOP). A real Palm IIIx runs at 16.58 MHz.
- **busy**: share of wall-clock time Palm OS kept the CPU running.
- **native/s**: how often native code stood in for ROM code.

`EMU idle` means Palm OS slept the whole 5 seconds.
