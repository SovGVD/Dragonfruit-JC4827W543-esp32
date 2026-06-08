# Evaluate the CoreTraps.h enum with C semantics and print the trap whose value is
# 0xA2D9 (plus a few neighbours for context).
param([string]$Header = "reference\cloudpilot-emu\src\cloudpilot\emulator\palm\Platform\Incs\Core\CoreTraps.h",
      [int]$Target = 0xA2D9)

$text = Get-Content $Header -Raw
# Grab the big enum body (from 'enum' containing sysTrapBase to its closing brace).
$names = @{ 'sysTrapBase' = 0xA000 }   # name -> value (#define seeded)
$order = @()      # [pscustomobject]@{Name;Value}
$next = $null

foreach ($raw in ($text -split "`n")) {
    $line = $raw
    if ($line -match '//') { $line = $line.Substring(0, $line.IndexOf('//')) }
    # strip /* */ on a single line
    $line = [regex]::Replace($line, '/\*.*?\*/', '')
    foreach ($m in [regex]::Matches($line, '([A-Za-z_]\w*)\s*(=\s*([^,}]+))?\s*,?')) {
        $name = $m.Groups[1].Value
        if ($name -in @('enum','typedef','UInt16','SysTrapNumberTag')) { continue }
        if ([string]::IsNullOrWhiteSpace($name)) { continue }
        if ($m.Groups[2].Success) {
            $expr = $m.Groups[3].Value.Trim()
            # evaluate expr: hex, decimal, NAME, NAME +/- N, sysTrapBase ...
            $val = $null
            if ($expr -match '^0x[0-9A-Fa-f]+$') { $val = [Convert]::ToInt32($expr.Substring(2),16) }
            elseif ($expr -match '^\d+$') { $val = [int]$expr }
            else {
                $tok = [regex]::Matches($expr, '([A-Za-z_]\w*)|(0x[0-9A-Fa-f]+|\d+)|([+\-])')
                $acc = 0; $op = '+'
                foreach ($t in $tok) {
                    $s = $t.Value
                    if ($s -in @('+','-')) { $op = $s; continue }
                    $v = $null
                    if ($s -match '^0x[0-9A-Fa-f]+$') { $v = [Convert]::ToInt32($s.Substring(2),16) }
                    elseif ($s -match '^\d+$') { $v = [int]$s }
                    elseif ($names.ContainsKey($s)) { $v = $names[$s] }
                    else { $v = $null }
                    if ($v -eq $null) { $acc = $null; break }
                    if ($op -eq '+') { $acc += $v } else { $acc -= $v }
                }
                $val = $acc
            }
            if ($val -ne $null) { $next = $val }
        }
        if ($next -eq $null) { continue }
        $names[$name] = $next
        $order += [pscustomobject]@{ Name = $name; Value = $next }
        $next = $next + 1
    }
}

$order | Where-Object { $_.Value -ge ($Target-3) -and $_.Value -le ($Target+3) } |
    ForEach-Object { "{0:X4}  {1}" -f $_.Value, $_.Name }
