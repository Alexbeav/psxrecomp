param(
    [string]$BuildName = 'build-modern-pass2',
    [switch]$GuestProjection
)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Project = Join-Path $Repo 'lab\sf2\local\generated-disc1-r2-load-delay'
$BaselineBuild = Join-Path $Project 'build-r8-scheduled-input'
$CanonicalModernBuild = Join-Path $Project 'build-modern-pass2'
$ModernBuild = Join-Path $Project $BuildName
$ConfigName = if ($BuildName -eq 'build-modern-pass2') {
    'game-modern-pass2.toml'
} else {
    "game-modern-pass2-$BuildName.toml"
}
$ModernConfig = Join-Path $Project $ConfigName
$Profile = Join-Path $Repo 'lab\sf2\modernization'
$Package = Join-Path $Repo 'dist\psxrecomp-cli-windows-x86_64'
$Framework = Join-Path $Package 'framework'
$Recompiler = Join-Path $Package 'libexec\psxrecomp-game.exe'
$SdlSource = Join-Path $Project 'build-r7-cd-seek\_deps\sdl3-src'

$env:PYTHONUTF8 = '1'
python (Join-Path $Repo 'tools\build_cli.py') release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item -Path (Join-Path $Framework '*') `
    -Destination (Join-Path $Project 'psxrecomp') -Recurse -Force

$ConfigText = Get-Content -LiteralPath (Join-Path $Project 'game.toml') -Raw
$VideoHeader = @'
[video]
renderer = "opengl"
supersampling = 4
antialiasing = true
texture_filtering = "nearest"
aspect_ratio = "16:9"
'@
$ConfigText = [regex]::Replace(
    $ConfigText, '(?ms)^\[video\]\s*\r?\n.*\z', $VideoHeader + "`r`n")
$ConfigText = [regex]::Replace(
    $ConfigText, '(?m)^(bios_hle\s*=\s*false\s*)$',
    ('$1' + "`r`nfast_boot = true"))
$ConfigText += @"

[controller]
default_mode = "digital"
mouse_pad = true

[controller.mouse_camera]
enabled = true
facing_site = "0x80053464"
facing_expected = "0x8EA30034"
application_state_addr = "0x8011EE90"
player_pointer_addr = "0x8012A574"
player_state_offset = "0x20"
wrapper_offset = "0xE0"
base_offset = "0xA4"
owner_offset = "0xDC"
desired_pitch_offset = "0x8E8"
rendered_pitch_offset = "0x918"
vector_x_offset = "0xCC"
vector_y_offset = "0xD0"
vector_z_offset = "0xD4"
controller_reg = 18
chase_yaw_sensitivity = 0.75
chase_pitch_sensitivity = 1.0
aim_yaw_sensitivity = 1.0
aim_pitch_sensitivity = 1.0
invert_y = false

[widescreen]
native_wide = true
gte_game_mode = true
nw_phase_backdrop = false
nw_full_mirror = true
nw_guest_projection = $($GuestProjection.IsPresent.ToString().ToLowerInvariant())
nw_world_min_polygons = 64

[widescreen.cull]
auto_screen_x = true
screen_h_imms = ["0xE0", "0xF0", "0xF1"]
"@
Set-Content -LiteralPath $ModernConfig -Value $ConfigText -Encoding utf8

Push-Location $Project
try {
    & $Recompiler --config $ModernConfig
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

cmake -S $Project -B $ModernBuild -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DPSX_RECOMP_UI=OFF -DPSX_DEBUG_TOOLS=ON `
    "-DFETCHCONTENT_SOURCE_DIR_SDL3=$SdlSource"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $ModernBuild --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item -LiteralPath (Join-Path $Profile 'keybinds.ini') `
    -Destination (Join-Path $ModernBuild 'keybinds.ini') -Force
Copy-Item -LiteralPath (Join-Path $Profile 'settings-pass2.toml') `
    -Destination (Join-Path $ModernBuild 'settings.toml') -Force
$CacheSource = $BaselineBuild
if ($ModernBuild -ne $CanonicalModernBuild -and
    (Test-Path -LiteralPath (Join-Path $CanonicalModernBuild 'cache'))) {
    $CacheSource = $CanonicalModernBuild
}
if (Test-Path -LiteralPath (Join-Path $CacheSource 'cache')) {
    $CacheDestination = Join-Path $ModernBuild 'cache'
    New-Item -ItemType Directory -Path $CacheDestination -Force | Out-Null
    robocopy (Join-Path $CacheSource 'cache') $CacheDestination /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -gt 7) {
        throw "Overlay cache copy failed with robocopy code $LASTEXITCODE"
    }
}
if ($CacheSource -eq $CanonicalModernBuild) {
    $CaptureManifest = Join-Path $CacheSource 'overlay_captures.json'
    $CaptureFragments = Join-Path $CacheSource 'overlay_captures.json.d'
    if (Test-Path -LiteralPath $CaptureManifest -PathType Leaf) {
        Copy-Item -LiteralPath $CaptureManifest -Destination $ModernBuild -Force
    }
    if (Test-Path -LiteralPath $CaptureFragments -PathType Container) {
        Copy-Item -Path (Join-Path $CaptureFragments '*') `
            -Destination (Join-Path $ModernBuild 'overlay_captures.json.d') `
            -Recurse -Force
    }
}
Write-Host "Pass-2 executable: $(Join-Path $ModernBuild 'SCUS94451_Recompiled.exe')"
Write-Host "Pass-2 configuration: $ModernConfig"
Write-Host "Frozen compatibility executable: $(Join-Path $BaselineBuild 'SCUS94451_Recompiled.exe')"
