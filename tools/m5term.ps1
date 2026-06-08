<#
  m5term.ps1 - serial-console driver library for the M5Paper Palm IIIx emulator.

  Dot-source this, then drive the on-device debug console (main/debug_console.c)
  over UART @115200: inject taps/keys and dump+render the 160x160 Palm
  framebuffer to a PNG for autonomous UI testing (no physical interaction).

      . tools/m5term.ps1
      Open-M5 -Reset -BootWaitS 15
      Tap-M5 70 30                 # tap Palm screen coords (0..159, identity post-calibration)
      Key-M5 36 0                  # inject ascii '6' (mod 0 = plain char)
      Get-M5Fb -Render -Png shot.png
      Close-M5

  Requires firmware built with the `fbdump` console command (added alongside
  these scripts). Coordinates are Palm screen space (0..159); after an identity
  digitizer calibration (see palm-setup.ps1) injected taps map 1:1 to pixels.
#>

$script:M5Port = $null

function Open-M5 {
  param([string]$Port = "COM5", [switch]$Reset, [int]$BootWaitS = 0)
  $p = New-Object System.IO.Ports.SerialPort $Port,115200,None,8,one
  $p.ReadTimeout = 900
  $p.Open()
  if ($Reset) { $p.DtrEnable = $false; $p.RtsEnable = $true; Start-Sleep -Milliseconds 250; $p.RtsEnable = $false }
  $script:M5Port = $p
  if ($BootWaitS -gt 0) { Start-Sleep -Seconds $BootWaitS }
  Clear-M5
}

function Close-M5 { if ($script:M5Port) { $script:M5Port.Close(); $script:M5Port = $null } }

function Clear-M5 {  # drain any pending output
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt 250) { try { [void]$script:M5Port.ReadExisting() } catch {}; Start-Sleep -Milliseconds 40 }
}

function Send-M5 {  # send one command; return collected "= " reply lines
  param([string]$Cmd, [int]$WaitMs = 200)
  $script:M5Port.WriteLine($Cmd)
  Start-Sleep -Milliseconds $WaitMs
  $out = @()
  try { $txt = $script:M5Port.ReadExisting() } catch { $txt = "" }
  foreach ($ln in ($txt -split "`n")) { $c = ($ln -replace '\x1b\[[0-9;]*m','').Trim(); if ($c -match '^= ') { $out += $c } }
  return $out
}

function Tap-M5 { param([int]$X, [int]$Y, [int]$WaitMs = 600)  # pen-down + pen-up at (X,Y)
  Send-M5 "tap $X $Y" 80 | Out-Null
  Send-M5 "penup" $WaitMs | Out-Null
}

function Key-M5 { param([string]$Hex, [string]$Mod = "0", [int]$WaitMs = 150)  # EvtEnqueueKey
  Send-M5 "key $Hex $Mod" $WaitMs | Out-Null
}

function Type-M5 {  # type an ASCII string as plain keyboard keys (e.g. calc "6*3=c")
  param([string]$Text, [int]$WaitMs = 160)
  foreach ($ch in $Text.ToCharArray()) { Key-M5 ('{0:X2}' -f [int][char]$ch) "0" $WaitMs }
}

function Get-M5Bytes { param($Rows, [int]$Y)  # the 20 byte values of framebuffer row Y
  $r = $Rows[$Y]; $b = @()
  for ($k = 0; $k -lt 20; $k++) { $b += [Convert]::ToInt32($r.Substring($k*2,2),16) }
  return $b
}
function Test-M5Pixel { param($Bytes, [int]$X)  # is pixel X (0..159) black? (MSB = leftmost)
  return ((($Bytes[($X -shr 3)] -shr (7 - ($X % 8))) -band 1) -ne 0)
}

function Save-M5Png { param($Rows, [string]$Png)  # render 160x160 1bpp rows -> 3x PNG
  Add-Type -AssemblyName System.Drawing
  $bmp = New-Object System.Drawing.Bitmap 160,160
  for ($y = 0; $y -lt [Math]::Min(160,$Rows.Count); $y++) {
    if ($Rows[$y].Length -lt 40) { continue }
    $b = Get-M5Bytes $Rows $y
    for ($x = 0; $x -lt 160; $x++) {
      $bmp.SetPixel($x, $y, $(if (Test-M5Pixel $b $x) { [System.Drawing.Color]::Black } else { [System.Drawing.Color]::White }))
    }
  }
  $big = New-Object System.Drawing.Bitmap 480,480
  $g = [System.Drawing.Graphics]::FromImage($big)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
  $g.DrawImage($bmp,0,0,480,480); $g.Dispose()
  $big.Save($Png); $bmp.Dispose(); $big.Dispose()
}

function Get-M5Fb {  # dump framebuffer; returns 160 hex row strings. -Render writes a PNG.
  param([switch]$Render, [string]$Png = "$env:TEMP\m5fb.png", [int]$SettleMs = 250)
  Start-Sleep -Milliseconds $SettleMs   # let the screen finish redrawing before the dump
  Clear-M5
  $script:M5Port.WriteLine("fbdump")
  $rows = @(); $deadline = (Get-Date).AddSeconds(7)
  while ((Get-Date) -lt $deadline) {
    try { $ln = $script:M5Port.ReadLine() } catch { continue }
    $ln = ($ln -replace '\x1b\[[0-9;]*m','').Trim()
    if ($ln -match '^FB:([0-9A-Fa-f]{40})$') { $rows += $matches[1] }
    elseif ($ln -match 'FBEND') { break }
  }
  if ($Render) { Save-M5Png $rows $Png }
  return $rows
}

function Find-M5Cluster {  # bbox + midpoint of black pixels in a region (locate a crosshair/button)
  param($Rows, [int]$X0 = 0, [int]$X1 = 159, [int]$Y0 = 0, [int]$Y1 = 159)
  $xs = @(); $ys = @()
  for ($y = $Y0; $y -le $Y1 -and $y -lt $Rows.Count; $y++) {
    if ($Rows[$y].Length -lt 40) { continue }
    $b = Get-M5Bytes $Rows $y
    for ($x = $X0; $x -le $X1; $x++) { if (Test-M5Pixel $b $x) { $xs += $x; $ys += $y } }
  }
  if (-not $xs) { return $null }
  $xmin = ($xs | Measure -Min).Minimum; $xmax = ($xs | Measure -Max).Maximum
  $ymin = ($ys | Measure -Min).Minimum; $ymax = ($ys | Measure -Max).Maximum
  return [pscustomobject]@{ N = $xs.Count; XMid = [int](($xmin + $xmax) / 2); YMid = [int](($ymin + $ymax) / 2);
                            XMin = $xmin; XMax = $xmax; YMin = $ymin; YMax = $ymax }
}
