# Palm OS: native ROM calls

The 68k core runs a few hot pieces of the Palm OS ROM as C instead of 68k
code (`src/components/m68k/m68k_palm_hle.c`). Each one leaves registers,
flags and memory exactly as the ROM code would, so Palm OS can't tell the
difference; it just takes fewer emulated instructions. Turn it all off with
`cpu.native = 0` in `/PalmOS/palm.cfg` (default 1).

## What runs natively

| ROM code | How it is found | What it does |
|---|---|---|
| TrapDispatcher | TRAP #15 vector points at its known 66 bytes | Every system call: looks the selector up in the trap table and jumps to the handler, leaving the same stack and registers |
| MemMove, MemSet | MacsBug name in the ROM | Copy/fill in C straight from the dispatcher, back to the caller |
| PrvBltConstCopy row loop | exact code bytes | Solid rectangle fill |
| PrvBltSwap word loop | exact code bytes | Colour swap (selection highlight) |
| PrvBlit1To8Copy row loop | exact code bytes | 1-bit bitmaps drawn on the 8-bit screen (text, icons) |
| PrvGetBltInfo table compare | exact code bytes | Compares two 256-entry colour tables on every blit |
| PrvBltMisAlignCopy_Generic word loop | exact code bytes | Bitmap copy across word boundaries |
| PrvConvert1To8 | exact code bytes | 1-bit to 8-bit expansion |

System calls only go native when the CPU is in supervisor mode, not tracing,
the trap is not a library trap, and the trap table still points at the ROM's
own handler (an app that patches a trap with `SysSetTrapAddress` keeps its
patch). MemMove/MemSet fall back to the ROM code when memory isn't plain RAM
or video RAM, or when the destination is NULL (so the ROM reports the error).

The loops are matched by their exact bytes, and the first word of each is
replaced in the PSRAM copy of the ROM by an F-line opcode (`$FFxx`, which
Palm OS never executes). A different ROM simply keeps its own loops; the boot
log says which were found:

```
I (m68k) native loop PrvBltConstCopy rows: on
I (m68k) native loop PrvBltSwap words: not in this ROM
I (m68k) native ROM calls: MemMove found, MemSet found
```

The loops were taken from the Palm OS 4.1 colour ROM. The dispatcher and
MemMove/MemSet work on any Palm OS 3.x/4.x ROM whose TrapDispatcher matches.

The emulated cycles charged match the dispatcher exactly and the loops and
MemMove/MemSet approximately (within a few percent). On the board the Palm
timer runs on wall-clock time, so this only affects the speed figure in the log.

## Effect

Instructions the 68k core executes on the PC build, Palm OS 4.1 colour ROM:

| Run | ROM code only | Native | Change |
|---|---|---|---|
| Boot through Setup to the launcher (290M cycles) | 13.97 M | 9.25 M | −34% |
| Launcher, Date Book, Memo typing, Address, Calc, Note Pad | 13.54 M | 8.95 M | −34% |

In the app run the dispatcher and MemMove/MemSet account for 11 points and
the blitter loops for the other 23. What's left on top is mostly the AMX
kernel's semaphore calls around the Memory Manager, spread thin.

## Checking it against the ROM

The PC build can run every Nth native call both ways: natively, then from
the same starting state through the ROM code with interrupts held off. It
compares all registers, the flags, every byte of RAM and video RAM and, for
the dispatcher, the cycle count. Leaf calls (MemMove/MemSet) are allowed to
differ in the scratch registers D1/A0/A1 (the dispatcher already clobbers
them), the condition codes and their own dead stack frame.

```sh
cmake -S src/host -B build-host && cmake --build build-host --target dragonfruit
./build-host/dragonfruit -r palm.rom -c 1100000000 --script tools/palm-workload.txt --hle-check 3
...
native ROM calls: 140719 dispatches, 2847 MemMove/MemSet, 11310 loops; checked ..., 0 mismatches
```

`--no-hle` runs the ROM code only.

## Finding the next hot spot

```sh
./build-host/dragonfruit -r palm.rom -c 1100000000 --script tools/palm-workload.txt \
    --profile run.prof --profile-from 290000000
python3 tools/palm_profile.py palm.rom run.prof --top 40          # by ROM function
python3 tools/palm_profile.py palm.rom run.prof --disasm MemMove  # with per-instruction counts
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
