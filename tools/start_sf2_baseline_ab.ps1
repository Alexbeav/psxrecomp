param(
    [Parameter(Mandatory = $true)]
    [string]$EnhancedSession,
    [string]$SessionName = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [int]$DebugPort = 19826
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Project = Join-Path $Repo 'lab\sf2\local\generated-disc1-r2-load-delay'
$Build = Join-Path $Project 'build-r8-scheduled-input'
$Exe = Join-Path $Build 'SCUS94451_Recompiled.exe'
$Game = Join-Path $Project 'game.toml'
$Bios = Join-Path $Build 'bios\openbios.bin'
$SourceSession = (Resolve-Path -LiteralPath $EnhancedSession).Path
$SourceCards = Join-Path $SourceSession 'cards'
$AbSession = Join-Path $Repo ("lab\sf2\local\baseline-ab-" + $SessionName)
$Cards = Join-Path $AbSession 'cards'

foreach ($Required in @($Exe, $Game, $Bios)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Required compatibility input is missing: $Required"
    }
}
if (-not (Test-Path -LiteralPath $SourceCards -PathType Container)) {
    throw "Enhanced session has no cards directory: $SourceCards"
}
if (Test-Path -LiteralPath $AbSession) {
    throw "Baseline A/B session already exists: $AbSession"
}

New-Item -ItemType Directory -Path $Cards -Force | Out-Null
Copy-Item -Path (Join-Path $SourceCards '*') -Destination $Cards -Recurse -Force

Write-Host 'Launching the frozen 4:3 compatibility executable.'
Write-Host "Source cards were copied read-only from: $SourceCards"
Write-Host "Baseline writes are isolated under: $AbSession"

$PriorDevInput = $env:PSX_DEV_INPUT
try {
    $env:PSX_DEV_INPUT = '0'
    & $Exe --game $Game --bios $Bios --no-launcher --renderer opengl `
        --window-title 'SF2 FROZEN 4:3 A-B' --debug-port $DebugPort `
        --memcard-dir $Cards
} finally {
    $env:PSX_DEV_INPUT = $PriorDevInput
}
