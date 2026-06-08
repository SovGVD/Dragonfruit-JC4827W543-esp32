#requires -Version 5
<#
.SYNOPSIS
  Build the Dragonfruit (Palm IIIx emulator) firmware inside the ESP-IDF Docker container.

.DESCRIPTION
  Wraps `docker compose run --rm idf idf.py ...`. With no arguments it runs a
  build. Any arguments are passed straight through to idf.py.

.EXAMPLE
  ./build.ps1                 # idf.py build
  ./build.ps1 fullclean       # idf.py fullclean
  ./build.ps1 size            # idf.py size
#>
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $IdfArgs
)

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

if (-not $IdfArgs -or $IdfArgs.Count -eq 0) {
    $IdfArgs = @('build')
}

Write-Host "==> docker compose run --rm idf idf.py $($IdfArgs -join ' ')" -ForegroundColor Cyan
& docker compose run --rm idf idf.py @IdfArgs
$code = $LASTEXITCODE
if ($code -ne 0) {
    Write-Host "Build failed (exit $code)." -ForegroundColor Red
    exit $code
}
Write-Host "Build OK. Artifacts in ./build. Flash with: ./flash.ps1 -Port COM5" -ForegroundColor Green
