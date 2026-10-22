<#
  calc-test.ps1 - exercise the Palm Calculator over the serial console to drive
  the JIT (the Calculator surfaced JIT-only codegen bugs under repeated ops).
  Assumes the Calculator is already open
  (run: ./tools/palm-setup.ps1 -Reset -LaunchCalc  first).

  Two modes:

  DEBUG (default) - type the op-string once and render the 160x160 display to a
  numbered PNG after each '=' (and a final.png), so the screen can be inspected
  for corruption (garbled digits / "-e-"). Use -ShotEachKey to shoot every key.

      ./tools/calc-test.ps1 -Ops "6*3=c7*8=c9*9=c55*55=c" -OutDir C:\tmp\calc

  MEASURE (-Measure) - soak the op-string for -Loops passes (after one warm-up
  pass) while capturing the async firmware PERF telemetry the debug mode discards,
  then report JIT throughput. The serial helpers (Key-M5/Send-M5) keep only the
  "= " command replies; the perf signal is in the firmware's async ESP_LOGI lines
  (the JIT "native=..% blocks=.. evict=.. drops=.." telemetry + the "EMU x.xx MHz
  while busy (.. % of real)" readout), so measure mode reads the raw stream itself. Reports:
    - delta-window native%  (delta native_cyc / (delta native+interp) over the run;
      the cumulative counters reset each boot, so a delta is the robust metric),
    - JIT cache health (blocks / evict / drops),
    - EMU MHz over the busy samples (noisier; ~+-3% run-to-run).
  Only the final frame is rendered in measure mode (mid-soak fbdumps would stall
  the run). The "EMU MHz" line only prints while the emulated Palm did some work
  in the 5 s window; a static screen logs "EMU idle", so native% is the headline number.

      ./tools/palm-setup.ps1 -Reset -LaunchCalc
      ./tools/calc-test.ps1 -Measure -Tag "baseline"
      # ... change code, rebuild, reflash, palm-setup again ...
      ./tools/calc-test.ps1 -Measure -Tag "after"

  Op-string chars: digits 0-9 ; + - * / operators ; = equals ; c clear ; . point.
  NOTE: this drives the calc via the keyboard/Graffiti path. If a bug reproduces
  only via on-screen button TAPS, use Tap-M5 with button coords instead.
#>
param(
  [string]$Port = "COM5",
  [string]$Ops = "6*3=c7*8=c9*9=c55*55=c123*9=c8*8=c",
  [int]$KeyWaitMs = 160,
  [string]$OutDir = "$env:TEMP\calc",
  [switch]$ShotEachKey,            # render after EVERY key, not just each '='
  [switch]$Measure,                # capture + report JIT perf telemetry (soak mode)
  [int]$Loops = 5,                 # measured passes over the op-string (measure mode)
  [string]$Tag = "run"             # label for the measure-mode report
)
. "$PSScriptRoot\m5term.ps1"

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
Open-M5 -Port $Port

if (-not $Measure) {
  # ---- DEBUG mode: type once, screenshot per '=' (or per key) ----
  $step = 0
  foreach ($ch in $Ops.ToCharArray()) {
    Key-M5 ('{0:X2}' -f [int][char]$ch) "0" $KeyWaitMs
    if ($ShotEachKey -or ($ch -eq '=')) {
      $step++
      $png = Join-Path $OutDir ("step{0:D2}.png" -f $step)
      Get-M5Fb -Render -Png $png | Out-Null
      Write-Host ("step {0,2}  after '{1}'  -> {2}" -f $step, $ch, $png)
    }
  }
  $png = Join-Path $OutDir "final.png"
  Get-M5Fb -Render -Png $png | Out-Null
  Write-Host "final -> $png"
  Close-M5
  return
}

# ---- MEASURE mode: soak + capture async perf telemetry ----
$log = New-Object System.Text.StringBuilder
function Drain { [void]$log.Append($script:M5Port.ReadExisting()) }

Start-Sleep -Milliseconds 200; [void]$script:M5Port.ReadExisting()

# One warm-up pass (let the JIT cache settle) BEFORE the measured window.
foreach ($ch in $Ops.ToCharArray()) {
  $script:M5Port.WriteLine("key " + ('{0:X2}' -f [int][char]$ch) + " 0")
  Start-Sleep -Milliseconds $KeyWaitMs; Drain
}
[void]$log.Clear()   # discard warm-up output; start the measured window clean

$swAll = [Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $Loops; $i++) {
  foreach ($ch in $Ops.ToCharArray()) {
    $script:M5Port.WriteLine("key " + ('{0:X2}' -f [int][char]$ch) + " 0")
    Start-Sleep -Milliseconds $KeyWaitMs
    Drain
  }
}
$elapsed = $swAll.Elapsed.TotalSeconds
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 2000) { Drain; Start-Sleep -Milliseconds 120 }   # trailing telemetry

$png = Join-Path $OutDir "final.png"
Get-M5Fb -Render -Png $png | Out-Null
Close-M5

# --- parse telemetry ---
$text = $log.ToString()
$tel = [regex]::Matches($text, 'native_cyc=(\d+) interp_cyc=(\d+) native=(\d+)% blocks=(\d+) evict=(\d+) (async|sync) drops=(\d+)')
$emu = [regex]::Matches($text, 'EMU ([\d.]+) MHz while busy \((\d+)% of real\)')
$idle = ([regex]::Matches($text, 'EMU idle')).Count

Write-Host ""
Write-Host "================ CALC SOAK [$Tag] ================" -ForegroundColor Cyan
Write-Host ("window: {0:N1}s, telemetry samples: {1}, EMU-busy: {2}, EMU-idle: {3}" -f $elapsed, $tel.Count, $emu.Count, $idle)
if ($tel.Count -ge 2) {
  $f = $tel[0]; $l = $tel[$tel.Count-1]
  $dNat = [int64]$l.Groups[1].Value - [int64]$f.Groups[1].Value
  $dInt = [int64]$l.Groups[2].Value - [int64]$f.Groups[2].Value
  $tot = $dNat + $dInt
  $deltaPct = if ($tot -gt 0) { [math]::Round($dNat * 100.0 / $tot, 1) } else { 0 }
  Write-Host ("native% (last cumulative): {0}%   delta-window native%: {1}%  (d-native_cyc={2}, d-interp_cyc={3})" -f $l.Groups[3].Value, $deltaPct, $dNat, $dInt)
  Write-Host ("blocks: {0} -> {1}    evict: {2} -> {3}    drops: {4} -> {5}    mode: {6}" -f `
      $f.Groups[4].Value, $l.Groups[4].Value, $f.Groups[5].Value, $l.Groups[5].Value, `
      $f.Groups[7].Value, $l.Groups[7].Value, $l.Groups[6].Value)
} else {
  Write-Host "WARNING: <2 telemetry samples captured; cannot compute delta (count=$($tel.Count))" -ForegroundColor Yellow
}
if ($emu.Count -gt 0) {
  $mhz = $emu | ForEach-Object { [double]$_.Groups[1].Value }
  $pct = $emu | ForEach-Object { [int]$_.Groups[2].Value }
  Write-Host ("EMU MHz (busy): min={0} max={1} avg={2}   %ofreal avg={3}" -f `
      ($mhz|Measure -Min).Minimum, ($mhz|Measure -Max).Maximum, [math]::Round(($mhz|Measure -Average).Average,2), [math]::Round(($pct|Measure -Average).Average,0))
} else {
  Write-Host "EMU MHz: no busy lines (calc soak is mostly idle between keys) -> native% is the metric"
}
Write-Host ("final screenshot: {0}" -f $png)
Write-Host "==================================================" -ForegroundColor Cyan
