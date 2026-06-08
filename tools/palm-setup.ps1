<#
  palm-setup.ps1 - drive the Palm IIIx through its 4-step first-boot Setup
  (welcome x2, digitizer calibration x3 targets, country/time, complete) to the
  launcher, entirely over the serial console. A cold boot (every flash/reset)
  lands in Setup, so run this after flashing to reach a usable launcher.

      ./flash.ps1 -Port COM5
      ./tools/palm-setup.ps1 -Reset -LaunchCalc

  The calibration crosshairs are drawn at fixed screen positions every boot;
  tapping them exactly gives an identity digitizer calibration, so subsequent
  injected taps map 1:1 to Palm screen pixels. Requires firmware with `fbdump`
  and ESP32 light sleep disabled (so the console stays alive while idle).
#>
param(
  [string]$Port = "COM5",
  [switch]$Reset,            # pulse RTS to reboot first (a flash already resets)
  [int]$BootWaitS = 15,      # seconds to wait for Palm OS to reach the welcome screen
  [switch]$LaunchCalc,       # after setup, tap the Calculator launcher icon
  [string]$Png = "$env:TEMP\palm.png"
)
. "$PSScriptRoot\m5term.ps1"

Open-M5 -Port $Port -Reset:$Reset -BootWaitS $BootWaitS

Write-Host "Setup 1-2/4: welcome screens (tap to continue)..."
Tap-M5 80 80 900            # welcome 1 -> 2
Tap-M5 80 80 900            # welcome 2 -> digitizer calibration

Write-Host "Setup 3/4: digitizer calibration (3 targets)..."
Tap-M5 10 10 900           # target 1: top-left crosshair
Tap-M5 144 148 900         # target 2: bottom-right crosshair
Tap-M5 79 60 900           # target 3: center verification crosshair

Write-Host "Setup 3/4: country/time -> Next..."
Tap-M5 55 153 900          # Next button

Write-Host "Setup 4/4: complete -> Done..."
Tap-M5 106 152 1300        # Done button -> launcher

if ($LaunchCalc) {
  Write-Host "Launching Calculator (row 1, col 2)..."
  Tap-M5 70 30 1300        # Calc icon on the launcher grid
}

Get-M5Fb -Render -Png $Png | Out-Null
Write-Host "Setup complete. Current screen -> $Png"
Close-M5
