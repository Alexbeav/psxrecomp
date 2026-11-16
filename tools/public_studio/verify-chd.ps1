param([switch]$NoOp,[int]$Attempt=1)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP="$root\tmp"
$env:TMP=$env:TEMP
$env:PYTHONDONTWRITEBYTECODE='1'
$env:PYTHONUTF8='1'
$env:WORKBENCH_BUILD_JOBS='1'
$env:WORKBENCH_STUDIO_RECORD="$root\pin-cache\public-studio-record.json"
$env:PSX_STUDIO_PROJECT_ONLY='1'
(Get-Process -Id $PID).PriorityClass='BelowNormal'
if($NoOp){'noop: actual selected-pin CHD reader, local inputs, no game start';exit 0}
$prefix="$root\reports\chd-reader-$Attempt"
if(Test-Path "$prefix.log"){throw 'Evidence exists; choose a fresh Attempt'}
$input="$root\inputs\chd\Metal Gear Solid (USA) (Disc 1) (Rev 1).chd"
$before=(Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLower()
$started=Get-Date
& "$root\inputs\toolchain\python\python.exe" -u -B "$PSScriptRoot\pin_entry.py" build --reader-only --pin "$root\pin-cache\records\PIN.json" --bios "$root\inputs\chd\SCPH5552.BIN" --bios-candidates "$root\inputs\chd\bios-candidates.json" --disc $input --output "$root\tmp\chd-unused-product" --source-repo "$root\pin-cache\c" --ui-repo "$root\pin-cache\u" --toolchain-pin "$root\pin-cache\records\TOOLCHAIN-PIN.toml" --toolchain "$root\inputs\toolchain" --kits-root "$root\inputs\title" --overlay-archives 'I:\Projects\Recompilations\PS1\Studio-inputs\overlay-archives' --run-root "$root\pin-runs" 1>"$prefix.log" 2>"$prefix.stderr.log"
$code=$LASTEXITCODE
$after=(Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLower()
[pscustomobject]@{exit_code=$code;seconds=((Get-Date)-$started).TotalSeconds;input_sha256=$before;input_unchanged=($before -eq $after);stdout="$prefix.log";stderr="$prefix.stderr.log";measurement='actual selected-core decoder reads staged MGS CHD and chooses USA BIOS after EU first'} | ConvertTo-Json | Set-Content "$prefix.record.json" -Encoding utf8
Get-Content "$prefix.log" -Tail 6
Get-Content "$prefix.stderr.log" -Tail 12
exit $code
