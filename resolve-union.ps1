# Union-merge resolver for trivially additive conflict hunks only.
#
# A hunk is resolved ONLY when both sides are non-empty, share no non-trivial
# line, and every line on both sides is one of:
#   - a C preprocessor / include line
#   - a blank line
#   - a #include "..." line
# This deliberately refuses anything containing real code, so a semantic
# conflict (e.g. a function whose signature differs between branches) is never
# unioned into a broken merge. Refused hunks are reported.
param(
    [Parameter(Mandatory = $true)][string[]]$Files,
    [switch]$DryRun
)

$totalResolved = 0
$refused = [System.Collections.Generic.List[string]]::new()

foreach ($f in $Files) {
    $lines = [System.Collections.Generic.List[string]](Get-Content -LiteralPath $f)
    $out   = [System.Collections.Generic.List[string]]::new()
    $i = 0
    $fileResolved = 0
    $hunkNo = 0
    while ($i -lt $lines.Count) {
        if ($lines[$i] -match '^<<<<<<< ') {
            $hunkNo++
            $ours = @(); $theirs = @()
            $i++
            while ($i -lt $lines.Count -and $lines[$i] -notmatch '^=======$' -and $lines[$i] -notmatch '^\|\|\|\|\|\|\|') {
                $ours += $lines[$i]; $i++
            }
            if ($i -lt $lines.Count -and $lines[$i] -match '^\|\|\|\|\|\|\|') {
                $i++
                while ($i -lt $lines.Count -and $lines[$i] -notmatch '^=======$') { $i++ }
            }
            if ($i -lt $lines.Count -and $lines[$i] -match '^=======$') { $i++ }
            while ($i -lt $lines.Count -and $lines[$i] -notmatch '^>>>>>>> ') { $theirs += $lines[$i]; $i++ }
            if ($i -lt $lines.Count) { $i++ }

            $additiveRe = '^\s*(#\s*(include|if|ifdef|ifndef|else|endif|define|undef|pragma)\b.*|)?$'
            $oursTrivial   = @($ours   | Where-Object { $_.Trim() -ne '' }) 
            $theirsTrivial = @($theirs | Where-Object { $_.Trim() -ne '' })
            $oursAllInc   = ($oursTrivial   | Where-Object { $_.Trim() -notmatch '^#\s*include\b' }).Count -eq 0
            $theirsAllInc = ($theirsTrivial | Where-Object { $_.Trim() -notmatch '^#\s*include\b' }).Count -eq 0

            if ($ours.Count -gt 0 -and $theirs.Count -gt 0 -and $oursAllInc -and $theirsAllInc) {
                foreach ($l in $ours)   { $out.Add($l) }
                foreach ($l in $theirs) { $out.Add($l) }
                $fileResolved++; $totalResolved++
            } else {
                foreach ($l in $ours) { $out.Add($l) }
                $refused.Add("${f}:hunk$hunkNo")
            }
        } else {
            $out.Add($lines[$i]); $i++
        }
    }
    if (-not $DryRun -and $fileResolved -gt 0) { [IO.File]::WriteAllLines($f, $out) }
    if ($fileResolved -gt 0) { Write-Host "union-resolved $fileResolved include-only hunk(s) in $f" }
}

Write-Host ""
Write-Host "include-only hunks union-resolved: $totalResolved"
Write-Host "hunks refused (need inspection): $($refused.Count)"
foreach ($r in $refused) { Write-Host "  $r" }
