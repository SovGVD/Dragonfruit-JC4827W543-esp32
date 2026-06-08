#requires -Version 5
<#
.SYNOPSIS
  Flash the built firmware to the M5 Paper from the Windows host using esptool.

.DESCRIPTION
  Docker Desktop on Windows can't reach COM ports, so the build runs in the
  container (build.ps1) and flashing runs here on the host with the host's
  esptool. Offsets, files and flash mode are read from build/flasher_args.json
  so they always match what was just built.

.PARAMETER Port
  Serial port of the M5 Paper's CP210x bridge. Default COM5.

.PARAMETER Baud
  Flash baud rate. Default 460800.

.PARAMETER Monitor
  After flashing, open a serial monitor (Ctrl+] to exit).

.EXAMPLE
  ./flash.ps1 -Port COM5 -Monitor
#>
[CmdletBinding()]
param(
    [string] $Port = 'COM5',
    [int]    $Baud = 460800,
    [switch] $Monitor
)

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

$argsFile = Join-Path $PSScriptRoot 'build/flasher_args.json'
if (-not (Test-Path $argsFile)) {
    throw "build/flasher_args.json not found. Run ./build.ps1 first."
}

$fa = Get-Content $argsFile -Raw | ConvertFrom-Json

# Flash settings (mode/freq/size) as produced by the build.
$writeArgs = @($fa.write_flash_args)

# offset -> file pairs (paths are relative to build/).
$pairs = @()
foreach ($p in $fa.flash_files.PSObject.Properties) {
    $offset = $p.Name
    $file   = Join-Path $PSScriptRoot ("build/" + $p.Value)
    if (-not (Test-Path $file)) { throw "Missing artifact: $file" }
    $pairs += $offset
    $pairs += $file
}

Write-Host "==> Flashing $Port @ $Baud" -ForegroundColor Cyan
Write-Host "    settings: $($writeArgs -join ' ')"
for ($i = 0; $i -lt $pairs.Count; $i += 2) {
    Write-Host ("    {0}  {1}" -f $pairs[$i], $pairs[$i+1])
}

& python -m esptool --chip esp32 --port $Port --baud $Baud write-flash @writeArgs @pairs
if ($LASTEXITCODE -ne 0) {
    Write-Host "Flash failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "Flash OK." -ForegroundColor Green

if ($Monitor) {
    Write-Host "==> Opening serial monitor on $Port @ 115200 (Ctrl+] to exit)" -ForegroundColor Cyan
    & python -m serial.tools.miniterm --raw $Port 115200
}
