param()
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Project = Join-Path $Repo 'lab\sf2\local\generated-disc1-r2-load-delay'
$BaselineBuild = Join-Path $Project 'build-r8-scheduled-input'
$ModernBuild = Join-Path $Project 'build-modern-pass1'
$ModernConfig = Join-Path $Project 'game-modern-pass1.toml'
$Profile = Join-Path $Repo 'lab\sf2\modernization'
$Framework = Join-Path $Repo 'dist\psxrecomp-cli-windows-x86_64\framework'
$SdlSource = Join-Path $Project 'build-r7-cd-seek\_deps\sdl3-src'

$env:PYTHONUTF8 = '1'
python (Join-Path $Repo 'tools\build_cli.py') release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item -Path (Join-Path $Framework '*') -Destination (Join-Path $Project 'psxrecomp') -Recurse -Force

$ConfigText = Get-Content -LiteralPath (Join-Path $Project 'game.toml') -Raw
$ConfigText = [regex]::Replace($ConfigText, '(?m)^renderer\s*=.*$', 'renderer = "opengl"')
$VideoHeader = @'
[video]
supersampling = 4
antialiasing = true
texture_filtering = "nearest"
'@
$ConfigText = [regex]::Replace($ConfigText, '(?m)^\[video\]\s*$', $VideoHeader)
if ($ConfigText -match '(?m)^\[controller\]\s*$') {
    $ControllerHeader = @'
[controller]
mouse_pad = true
mouse_counts_per_frame = 12
'@
    $ConfigText = [regex]::Replace($ConfigText, '(?m)^\[controller\]\s*$', $ControllerHeader)
} else {
    $ConfigText += @'

[controller]
default_mode = "digital"
mouse_pad = true
mouse_counts_per_frame = 12
'@
}
Set-Content -LiteralPath $ModernConfig -Value $ConfigText -Encoding utf8

cmake -S $Project -B $ModernBuild -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DPSX_RECOMP_UI=OFF -DPSX_DEBUG_TOOLS=ON `
    "-DFETCHCONTENT_SOURCE_DIR_SDL3=$SdlSource"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $ModernBuild --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item -LiteralPath (Join-Path $Profile 'keybinds.ini') -Destination (Join-Path $ModernBuild 'keybinds.ini') -Force
Copy-Item -LiteralPath (Join-Path $Profile 'settings.toml') -Destination (Join-Path $ModernBuild 'settings.toml') -Force
if (Test-Path -LiteralPath (Join-Path $BaselineBuild 'cache')) {
    Copy-Item -Path (Join-Path $BaselineBuild 'cache\*') -Destination (Join-Path $ModernBuild 'cache') -Recurse -Force
}
Write-Host "Modernized executable: $(Join-Path $ModernBuild 'SCUS94451_Recompiled.exe')"
Write-Host "Baseline executable remains: $(Join-Path $BaselineBuild 'SCUS94451_Recompiled.exe')"
