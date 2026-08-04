param(
    [string]$SessionName = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [int]$DebugPort = 19825
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Project = Join-Path $Repo 'lab\sf2\local\generated-disc1-r2-load-delay'
$Build = Join-Path $Project 'build-modern-pass2'
$Exe = Join-Path $Build 'SCUS94451_Recompiled.exe'
$Game = Join-Path $Project 'game-modern-pass2.toml'
$Bios = Join-Path $Build 'bios\openbios.bin'
$Monitor = Join-Path $Repo 'tools\sf2_widescreen_cull_capture.py'
$Session = Join-Path $Repo ("lab\sf2\local\widescreen-cull-" + $SessionName)
$Cards = Join-Path $Session 'cards'
$Evidence = Join-Path $Session 'evidence'

foreach ($Required in @($Exe, $Game, $Bios, $Monitor)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Required input is missing: $Required"
    }
}
if (Test-Path -LiteralPath $Session) {
    throw "Capture session already exists: $Session"
}
New-Item -ItemType Directory -Path $Cards -Force | Out-Null

Write-Host 'Starting the aspect-correct SF2 native-wide diagnostic build.'
Write-Host 'This capture does not write guest RAM or force application state.'
Write-Host "Bounded evidence directory: $Evidence"

$Arguments = @(
    '--game', $Game,
    '--bios', $Bios,
    '--no-launcher',
    '--renderer', 'opengl',
    '--window-title', 'SF2 WIDESCREEN CULL CAPTURE',
    '--debug-port', $DebugPort,
    '--memcard-dir', $Cards
)

$PriorDevInput = $env:PSX_DEV_INPUT
try {
    $env:PSX_DEV_INPUT = '0'
    $Runtime = Start-Process -FilePath $Exe -ArgumentList $Arguments `
        -WorkingDirectory $Build -PassThru
    python $Monitor --port $DebugPort --out $Evidence --exe $Exe --game $Game
    if ($LASTEXITCODE -ne 0) {
        throw "Cull monitor exited with code $LASTEXITCODE; retain $Session."
    }
    $Runtime.WaitForExit()
} finally {
    $env:PSX_DEV_INPUT = $PriorDevInput
}

Write-Host "Widescreen cull capture finalized: $Session"
