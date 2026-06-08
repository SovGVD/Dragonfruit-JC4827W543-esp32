param(
    [string]$Port = 'COM5',
    [int]$Baud = 115200,
    [string]$LogFile = 'serial_benchmark.log',
    [string]$CmdFile = 'serial_cmd.txt'
)

# Single owner of $Port. Continuously reads lines from the device and appends them
# to $LogFile with a ms timestamp. Also polls $CmdFile: any line written there is
# sent to the device (newline-terminated) and the file is cleared. This lets the
# host both tail the log AND inject debug-console commands through one port handle
# (the port is exclusive, so a second opener would fail). Reconnects on port drop.
$ErrorActionPreference = 'Stop'
$logPath = Join-Path (Get-Location) $LogFile
$cmdPath = Join-Path (Get-Location) $CmdFile
"--- serial_log start $Port @ $Baud ---" | Out-File -FilePath $logPath -Encoding utf8
if (Test-Path $cmdPath) { Remove-Item $cmdPath -Force }

while ($true) {
    $sp = New-Object System.IO.Ports.SerialPort($Port, $Baud)
    $sp.ReadTimeout = 200
    $sp.NewLine = "`n"
    try {
        $sp.Open()
        while ($sp.IsOpen) {
            # 1) Flush any queued commands to the device.
            if (Test-Path $cmdPath) {
                $cmds = @(Get-Content $cmdPath -ErrorAction SilentlyContinue)
                Remove-Item $cmdPath -Force -ErrorAction SilentlyContinue
                foreach ($c in $cmds) {
                    if ($c -ne $null -and $c.Trim().Length -gt 0) {
                        $sp.Write($c.Trim() + "`n")
                        $ts = (Get-Date).ToString('HH:mm:ss.fff')
                        "$ts >>> $($c.Trim())" | Out-File -FilePath $logPath -Append -Encoding utf8
                    }
                }
            }
            # 2) Drain inbound lines.
            try {
                $line = $sp.ReadLine()
                $ts = (Get-Date).ToString('HH:mm:ss.fff')
                "$ts $line" | Out-File -FilePath $logPath -Append -Encoding utf8
            } catch [System.TimeoutException] {
                # idle; loop back to check the command file
            }
        }
    } catch {
        $ts = (Get-Date).ToString('HH:mm:ss.fff')
        "$ts [logger] port error: $($_.Exception.Message)" | Out-File -FilePath $logPath -Append -Encoding utf8
        Start-Sleep -Milliseconds 500
    } finally {
        if ($sp.IsOpen) { $sp.Close() }
        $sp.Dispose()
    }
}
