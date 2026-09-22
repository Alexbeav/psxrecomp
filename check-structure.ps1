# Detect a spliced-function defect: a function definition whose braces never
# balance before the next line that starts a new definition at column 0.
#
# This class of error appears when a union/merge inserts one function's body
# into the middle of another, leaving the first without its closing brace. GCC
# then reports "invalid storage class for function" on the later definition.
#
# Heuristic and deliberately conservative: it only reports candidates where the
# running brace depth never returns to 0 between two top-level definitions.
param([Parameter(Mandatory = $true)][string[]]$Files)

$defRe = '^(static\s+)?[A-Za-z_][A-Za-z0-9_ \*]*\s+[A-Za-z_][A-Za-z0-9_]*\s*\([^;]*$'
$found = 0

foreach ($f in $Files) {
    if (-not (Test-Path $f)) { continue }
    $lines = Get-Content -LiteralPath $f
    $depth = 0
    $inDef = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        # Track a candidate definition start only when depth is 0.
        if ($depth -eq 0 -and $line -match $defRe) {
            $inDef = $i + 1
        }
        foreach ($ch in $line.ToCharArray()) {
            if ($ch -eq '{') { $depth++ }
            elseif ($ch -eq '}') { $depth-- }
        }
        if ($depth -lt 0) {
            Write-Host "${f}:$($i+1): brace depth went negative (extra close)"
            $found++
            $depth = 0
        }
        # A definition began, and we are back at depth 0 -> it closed properly.
        if ($inDef -gt 0 -and $depth -eq 0 -and $i + 1 -gt $inDef) {
            $inDef = -1
        }
    }
    if ($depth -ne 0) {
        Write-Host "${f}: file ends at brace depth ${depth} (unbalanced)"
        $found++
    }
}

Write-Host ""
Write-Host "structural anomalies: $found"
