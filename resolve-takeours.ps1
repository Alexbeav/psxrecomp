# Take-ours resolver for a known-repeated conflict shape.
#
# Used where the same semantic choice applies to many identical hunks and the
# choice has been established by evidence (leading symbol exists and is defined
# in the merged tree; the other side's symbol does not). Every hunk it touches
# is required to CONTAIN the marker regex given on the command line, so it can
# never silently affect an unrelated hunk.
param(
    [Parameter(Mandatory = $true)][string]$File,
    [Parameter(Mandatory = $true)][string]$MustContain,   # regex; theirs-side must match
    [Parameter(Mandatory = $true)][string]$Reason,
    [switch]$DryRun
)

$lines = [System.Collections.Generic.List[string]](Get-Content -LiteralPath $File)
$out   = [System.Collections.Generic.List[string]]::new()
$i = 0; $taken = 0; $hunkNo = 0; $untouched = 0

while ($i -lt $lines.Count) {
    if ($lines[$i] -match '^<<<<<<< ') {
        $hunkNo++
        $ours = @(); $theirs = @()
        $i++
        while ($i -lt $lines.Count -and $lines[$i] -notmatch '^=======$' -and $lines[$i] -notmatch '^\|\|\|\|\|\|\|') { $ours += $lines[$i]; $i++ }
        if ($i -lt $lines.Count -and $lines[$i] -match '^\|\|\|\|\|\|\|') {
            $i++; while ($i -lt $lines.Count -and $lines[$i] -notmatch '^=======$') { $i++ }
        }
        if ($i -lt $lines.Count -and $lines[$i] -match '^=======$') { $i++ }
        while ($i -lt $lines.Count -and $lines[$i] -notmatch '^>>>>>>> ') { $theirs += $lines[$i]; $i++ }
        if ($i -lt $lines.Count) { $i++ }

        $theirsText = ($theirs -join "`n")
        if ($theirsText -match $MustContain) {
            foreach ($l in $ours) { $out.Add($l) }   # take ours
            $taken++
            Write-Host "  hunk $hunkNo : took OURS  ($Reason)"
        } else {
            # Not the shape we're authorised to change — leave it conflicted.
            $out.Add('<<<<<<< HEAD'); foreach ($l in $ours) { $out.Add($l) }
            $out.Add('======='); foreach ($l in $theirs) { $out.Add($l) }
            $out.Add('>>>>>>> 452cc0c')
            $untouched++
        }
    } else { $out.Add($lines[$i]); $i++ }
}

if (-not $DryRun) { [IO.File]::WriteAllLines($File, $out) }
Write-Host ""
Write-Host "took ours: $taken   left conflicted: $untouched"
