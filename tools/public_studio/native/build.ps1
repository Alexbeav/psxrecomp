param([switch]$NoOp)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($NoOp){Write-Output 'noop: compile one owned native Studio frontend';exit 0}
$root='I:\Projects\Recompilations\PS1\Work\eagle\PS1B-165-studio-module-20261031'
(Get-Process -Id $PID).PriorityClass='BelowNormal'
$env:PATH="$root\inputs\toolchain\bin;"+$env:PATH
$env:TEMP="$root\tmp";$env:TMP=$env:TEMP
& "$root\inputs\toolchain\bin\cmake.exe" -S "$root\core\tools\public_studio\native" -B "$root\native-build" -G Ninja "-DCMAKE_CXX_COMPILER=$root/inputs/toolchain/bin/clang++.exe" "-DSDL3_DIR=$root/inputs/SDL3/lib/cmake/SDL3" "-DIMGUI_DIR=$root/ui/src/third_party/imgui" "-DSTB_DIR=$root/ui/src/third_party" -DCMAKE_BUILD_TYPE=Release
if($LASTEXITCODE){exit $LASTEXITCODE}
& "$root\inputs\toolchain\bin\cmake.exe" --build "$root\native-build" -j 1
exit $LASTEXITCODE
