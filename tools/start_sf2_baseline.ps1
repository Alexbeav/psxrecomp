param([int]$DebugPort = 19811)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Project = Join-Path $Repo 'lab\sf2\local\generated-disc1-r2-load-delay'
$Build = Join-Path $Project 'build-r8-scheduled-input'
$Exe = Join-Path $Build 'SCUS94451_Recompiled.exe'
$Game = Join-Path $Project 'game.toml'
$Bios = Join-Path $Build 'bios\openbios.bin'
$Cards = Join-Path $Repo 'lab\sf2\local\baseline-ab-cards'
New-Item -ItemType Directory -Path $Cards -Force | Out-Null
& $Exe --game $Game --bios $Bios --no-launcher --renderer opengl `
    --window-title 'SF2 BASELINE' --debug-port $DebugPort --memcard-dir $Cards
