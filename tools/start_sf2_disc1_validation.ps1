param(
    [string]$SessionName = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [int]$DebugPort = 19824
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Project = Join-Path $Repo 'lab\sf2\local\generated-disc1-r2-load-delay'
$Build = Join-Path $Project 'build-modern-pass2'
$Exe = Join-Path $Build 'SCUS94451_Recompiled.exe'
$Game = Join-Path $Project 'game-modern-pass2.toml'
$Bios = Join-Path $Build 'bios\openbios.bin'
$Settings = Join-Path $Build 'settings.toml'
$Monitor = Join-Path $Repo 'tools\sf2_disc1_validation_monitor.py'
$Session = Join-Path $Repo ("lab\sf2\local\human-disc1-pass2-" + $SessionName)
$Cards = Join-Path $Session 'cards'
$Timeline = Join-Path $Session 'disc1.psxpad'
$Evidence = Join-Path $Session 'evidence.json'

foreach ($Required in @($Exe, $Game, $Bios, $Settings, $Monitor)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Required input is missing: $Required"
    }
}
if (Test-Path -LiteralPath $Session) {
    throw "Validation session already exists: $Session"
}
New-Item -ItemType Directory -Path $Cards -Force | Out-Null

Write-Host 'Starting the isolated SF2 pass-2 Disc 1 validation build.'
Write-Host 'Play Missions 1-8 naturally, then close the game at a stable post-Mission-8 state.'
Write-Host 'If a visual/input defect appears, note the mission and circumstance; use the 4:3 baseline afterward for A/B.'
Write-Host "Bounded evidence directory: $Session"

$Arguments = @(
    '--game', $Game,
    '--bios', $Bios,
    '--no-launcher',
    '--renderer', 'opengl',
    '--window-title', 'SF2 DISC 1 PASS 2 VALIDATION',
    '--debug-port', $DebugPort,
    '--memcard-dir', $Cards,
    '--pad-record', $Timeline
)
$MonitorArguments = @(
    $Monitor,
    '--port', $DebugPort,
    '--out', $Evidence,
    '--repo', $Repo,
    '--exe', $Exe,
    '--game', $Game,
    '--bios', $Bios,
    '--settings', $Settings,
    '--timeline', $Timeline
)

$PriorDevInput = $env:PSX_DEV_INPUT
try {
    $env:PSX_DEV_INPUT = '0'
    $Runtime = Start-Process -FilePath $Exe -ArgumentList $Arguments `
        -WorkingDirectory $Build -PassThru
    $MonitorArguments += @('--pid', $Runtime.Id)
    $SemanticMonitor = Start-Process -FilePath 'python' `
        -ArgumentList $MonitorArguments -WorkingDirectory $Repo `
        -WindowStyle Hidden -PassThru
    $Runtime.WaitForExit()
    $Runtime.Refresh()
    if (-not $SemanticMonitor.WaitForExit(30000)) {
        throw "Semantic monitor did not finalize; retain $Session for diagnosis."
    }
    $SemanticMonitor.Refresh()
    if ($SemanticMonitor.ExitCode -ne 0) {
        throw "Semantic monitor exited with code $($SemanticMonitor.ExitCode); retain $Session for diagnosis."
    }
} finally {
    $env:PSX_DEV_INPUT = $PriorDevInput
}

if (-not (Test-Path -LiteralPath $Evidence -PathType Leaf)) {
    throw "Runtime exited without semantic evidence; retain $Session for diagnosis."
}
$Result = Get-Content -LiteralPath $Evidence -Raw | ConvertFrom-Json
if ($Result.result -ne 'capture_complete' -or
    -not $Result.timeline.structurally_valid) {
    throw "Validation evidence is incomplete; retain $Session for diagnosis."
}

Write-Host "Disc 1 validation capture finalized: $Session"
Write-Host "PAD samples: $($Result.timeline.samples)"
Write-Host "Wide presents: $($Result.present.counts.wide)"
Write-Host "Wide fallbacks: $($Result.present.counts.wide_fellback)"
Write-Host "Fullscreen rectangles expanded: $($Result.last_snapshot.gpu.ws.fullscreen_rect.expanded)"
