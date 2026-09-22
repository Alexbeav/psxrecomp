param(
    [string]$SessionName = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [int]$DebugPort = 19793
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Project = Join-Path $Repo 'lab\sf2\local\generated-disc1-r2-load-delay'
$Build = Join-Path $Project 'build-r8-scheduled-input'
$Exe = Join-Path $Build 'SCUS94451_Recompiled.exe'
$Game = Join-Path $Project 'game.toml'
$Bios = Join-Path $Build 'bios\openbios.bin'
$Session = Join-Path $Repo ("lab\sf2\local\human-mission1-capture-" + $SessionName)
$Cards = Join-Path $Session 'cards'
$Timeline = Join-Path $Session 'mission1.psxpad'
$Stdout = Join-Path $Session 'runtime.stdout.log'
$Stderr = Join-Path $Session 'runtime.stderr.log'
$Receipt = Join-Path $Session 'receipt.json'

foreach ($Required in @($Exe, $Game, $Bios)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Required input is missing: $Required"
    }
}
if (Test-Path -LiteralPath $Session) {
    throw "Capture session already exists: $Session"
}
New-Item -ItemType Directory -Path $Cards -Force | Out-Null

Write-Host "Starting authentic cold boot with isolated blank memory cards."
Write-Host "Play through Mission 1, stop at a stable post-mission state, then close the game window."
$Arguments = @(
    '--game', $Game,
    '--bios', $Bios,
    '--no-launcher',
    '--renderer', 'opengl',
    '--debug-port', $DebugPort,
    '--memcard-dir', $Cards,
    '--pad-record', $Timeline
)
$Process = Start-Process -FilePath $Exe -ArgumentList $Arguments -WorkingDirectory $Build `
    -RedirectStandardOutput $Stdout -RedirectStandardError $Stderr -PassThru
$Process.WaitForExit()
$Process.Refresh()
$ExitCode = $Process.ExitCode
if (-not (Test-Path -LiteralPath $Timeline -PathType Leaf)) {
    throw "Runtime exited without a PAD timeline; retain $Session for diagnosis."
}

$Bytes = [System.IO.File]::ReadAllBytes($Timeline)
if ($Bytes.Length -lt 32 -or [Text.Encoding]::ASCII.GetString($Bytes, 0, 7) -ne 'PSXPAD1') {
    throw 'PAD timeline header is invalid.'
}
$Samples = [BitConverter]::ToUInt64($Bytes, 20)
if ($Bytes.Length -ne (32 + 32 * $Samples)) {
    throw 'PAD timeline sample count does not match its file length.'
}
if ($null -ne $ExitCode -and $ExitCode -ne 0) {
    throw "Runtime exited with code $ExitCode; retain $Session for diagnosis."
}
$Result = [ordered]@{
    schema = 1
    result = 'capture_complete'
    exit_code = if ($null -eq $ExitCode) { 'unavailable' } else { $ExitCode }
    samples = $Samples
    timeline_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Timeline).Hash.ToLowerInvariant()
    executable_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Exe).Hash.ToLowerInvariant()
    game_config_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Game).Hash.ToLowerInvariant()
    bios_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Bios).Hash.ToLowerInvariant()
}
$Result | ConvertTo-Json | Set-Content -LiteralPath $Receipt -Encoding utf8
Write-Host "Capture finalized: $Session"
Write-Host "Do not edit or rename its contents; the lab will replay it twice from clean processes."
