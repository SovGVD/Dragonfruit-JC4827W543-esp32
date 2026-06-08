# tools/ — on-device test automation (serial console)

Host-side PowerShell helpers that drive the emulator's UART debug console
(`src/main/debug_console.c`) to test the device **without physical interaction** —
inject taps/keys and capture the Palm screen as a PNG. Built for autonomous
JIT/UI bug hunting (e.g. the Calculator).

Requires firmware built with the **`fbdump`** console command (streams the
160×160 framebuffer as hex). For unattended runs also build with **ESP32 light
sleep disabled** (otherwise the idle Palm auto-off light-sleeps the ESP32 and
the console goes dead until the physical power button).

## Scripts

| Script | Purpose |
|---|---|
| `m5term.ps1` | Dot-source library: `Open-M5`/`Close-M5`, `Tap-M5 X Y`, `Key-M5 HEX MOD`, `Type-M5 "text"`, `Get-M5Fb -Render -Png`, `Find-M5Cluster` (locate a crosshair/button by black-pixel bbox). |
| `palm-setup.ps1` | Drive the 4-step first-boot Setup (welcome, digitizer calibration, country/time, done) to the launcher. `-Reset` to reboot first, `-LaunchCalc` to open the Calculator. |
| `calc-test.ps1` | Type a calculator op-string (`"6*3=c..."`) via the keyboard path. Default: render the display after each `=` to numbered PNGs for corruption inspection. `-Measure`: soak the op-string and report JIT perf — delta-window **native%** (the robust metric; cumulative counters reset each boot), cache health (blocks/evict/drops), and EMU MHz — to compare JIT changes before/after. |

### Python helpers (cross-platform; require `pyserial`)

| Script | Purpose |
|---|---|
| `serial_console.py` | Driver for the firmware's line-protocol debug console (`--port COM5 <cmd>`, `-i` REPL, or `--capture N` for passive logging). Attaches **without** resetting the device, so it can inspect a running or sleeping board. Commands: `regs`, `irq`, `sleep`, `fb`, `tap X Y`, `penup`, `key HEX [MOD]`, `home`, `power`, `wake`. |
| `capture_boot.py` | Hard-resets the board (pulses RTS→EN) and streams ~N seconds of boot log to **stdout** so it can be teed to a file: `python tools/capture_boot.py COM5 18 > boot.log`. Use when the idf/miniterm monitor's un-redirectable output gets in the way. |

## Typical loop (per test build)

```powershell
./build.ps1
./flash.ps1 -Port COM5
./tools/palm-setup.ps1 -Reset -LaunchCalc        # -> Calculator open
./tools/calc-test.ps1 -Ops "6*7=c6*7=c..." -OutDir C:\tmp\calc
# inspect C:\tmp\calc\*.png for garbled digits / "-e-"
```

Coordinates are Palm screen space (0..159). Tapping the calibration crosshairs
exactly (palm-setup) yields identity calibration so injected taps map 1:1 to
pixels. `Find-M5Cluster` helps locate UI elements from a framebuffer dump.
