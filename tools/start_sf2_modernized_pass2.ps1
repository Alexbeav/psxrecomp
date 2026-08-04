param(
    [int]$DebugPort = 19820,
    [string]$BuildName = 'build-modern-pass2'
)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Project = Join-Path $Repo 'lab\sf2\local\generated-disc1-r2-load-delay'
$Build = Join-Path $Project $BuildName
$Exe = Join-Path $Build 'SCUS94451_Recompiled.exe'
$ConfigName = if ($BuildName -eq 'build-modern-pass2') {
    'game-modern-pass2.toml'
} else {
    "game-modern-pass2-$BuildName.toml"
}
$Game = Join-Path $Project $ConfigName
$Bios = Join-Path $Build 'bios\openbios.bin'
$Cards = Join-Path $Repo 'lab\sf2\local\modern-pass2-cards'
if (-not (Test-Path -LiteralPath $Exe)) {
    throw 'Run tools\build_sf2_modernized_pass2.ps1 first.'
}
New-Item -ItemType Directory -Path $Cards -Force | Out-Null
$PriorDevInput = $env:PSX_DEV_INPUT
try {
    $env:PSX_DEV_INPUT = '0'
    & $Exe --game $Game --bios $Bios --no-launcher --renderer opengl `
        --window-title 'SF2 MODERN PASS 2' --debug-port $DebugPort `
        --memcard-dir $Cards
} finally {
    $env:PSX_DEV_INPUT = $PriorDevInput
}
