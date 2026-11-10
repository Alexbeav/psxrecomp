param([switch]$NoOp,[ValidateSet('Prepare','Build')][string]$Step='Prepare',[int]$Attempt=1)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$true
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP="$root\tmp"
$env:TMP=$env:TEMP
$env:PYTHONDONTWRITEBYTECODE='1'
$env:GIT_TERMINAL_PROMPT='0'
$env:GCM_INTERACTIVE='Never'
$env:WORKBENCH_BUILD_JOBS='1'
$env:WORKBENCH_NO_FETCH='1'
$env:WORKBENCH_STUDIO_RECORD="$root\pin-cache\public-studio-record.json"
$env:WORKBENCH_RUN_ROOT="$root\pin-runs"
(Get-Process -Id $PID).PriorityClass='BelowNormal'
if($NoOp){'noop: established pinned builder, serial/headless and edition-local';exit 0}
$cache="$root\pin-cache"
New-Item -ItemType Directory -Path $cache -Force | Out-Null
if($Step -eq 'Prepare'){
 $pin='20ec7255304b7c20ea87f6e05577defa1b5e5948'
 $ui='f998b0a77558d660dc35f507ee2e93eade0f12c6'
 foreach($entry in @(@('c','core',$pin,'https://github.com/Alexbeav/psxrecomp'),@('u','ui',$ui,'https://github.com/Alexbeav/recomp-ui'))){
  $target=Join-Path $cache $entry[0]
  if(Test-Path "$target\.git"){throw 'Prepared source already exists; inspect it instead of overwriting'}
  & git init --quiet $target
  & git -C $target fetch --quiet --depth=1 "$root\$($entry[1])" $entry[2]
  & git -C $target checkout --quiet --detach $entry[2]
  & git -C $target remote add origin $entry[3]
  & git -C $target config gc.auto 0
  & git -C $target config maintenance.auto false
 }
 & git -C "$cache\c" submodule update --init --recursive
 New-Item -ItemType Directory -Path "$cache\records" -Force | Out-Null
 Copy-Item -LiteralPath 'Z:\Share\Projects\Recompilations\PS1\Pins\psxrecomp\test-20261022-i-ui\PIN.json' -Destination "$cache\records\PIN.json"
 Copy-Item -LiteralPath 'I:\Projects\Recompilations\PS1\Studio-inputs\toolchain-pin\TOOLCHAIN-PIN.toml' -Destination "$cache\records\TOOLCHAIN-PIN.toml"
 New-Item -ItemType Directory -Path "$cache\ui-input" -Force | Out-Null
 Copy-Item -LiteralPath 'I:\Projects\Recompilations\PS1\Studio-inputs\uis\b482-f998\UI.json' -Destination "$cache\ui-input\UI.json"
 Copy-Item -LiteralPath 'I:\Projects\Recompilations\PS1\Studio-inputs\uis\b482-f998\recomp-launcher-v1.dll' -Destination "$cache\ui-input\recomp-launcher-v1.dll"
 $base="$root\inputs\SDL3"
 foreach($file in Get-ChildItem -LiteralPath $base -File -Recurse){
  $relative=[IO.Path]::GetRelativePath($base,$file.FullName)
  $destination=Join-Path "$cache\ui-input\sdl-install" $relative
  New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
  New-Item -ItemType HardLink -Path $destination -Target $file.FullName | Out-Null
 }
 'prepared exact existing pin/source/UI inputs'
 exit 0
}
$env:WORKBENCH_PIN_ROOT="$cache\records"
$env:WORKBENCH_UI_ROOT="$cache\ui-input"
$env:PSX_STUDIO_PROJECT_ONLY='1'
$env:LOCALAPPDATA="$cache\appdata"
$env:APPDATA="$cache\roaming"
$env:RETCOMM_DATA_HOME="$cache\retcomm"
$env:HOME="$cache\home"
$env:USERPROFILE="$cache\home"
$tool="$root\inputs\toolchain\python\python.exe"
$arguments=@('-u','-B',"$PSScriptRoot\pin_entry.py",'build','--pin',"$cache\records\PIN.json",'--ui',"$cache\ui-input\UI.json",'--bios',"$root\inputs\title\SCPH1001.BIN",'--disc',"$root\inputs\title\Ape Escape (USA).cue",'--output',"$root\candidate\Ape-Pinned-$Attempt",'--source-repo',"$cache\c",'--ui-repo',"$cache\u",'--toolchain-pin',"$cache\records\TOOLCHAIN-PIN.toml",'--toolchain',"$root\inputs\toolchain",'--kit',"$root\inputs\title\recipe",'--kits-root',"$root\inputs\title",'--overlay-archives','I:\Projects\Recompilations\PS1\Studio-inputs\overlay-archives','--run-root',"$root\pin-runs",'--input-mode','portable','--platform','windows-x64','--keep-development-files')
$prefix=Join-Path $root "reports\ape-pin-build-$Attempt"
if(Test-Path "$prefix.log"){throw 'Evidence already exists; choose a fresh Attempt'}
$started=Get-Date
$quoted=@($arguments | ForEach-Object { '"'+$_+'"' })
$process=Start-Process -FilePath $tool -ArgumentList $quoted -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput "$prefix.log" -RedirectStandardError "$prefix.stderr.log" -PassThru
try{$process.PriorityClass='BelowNormal'}catch{if(-not $process.HasExited){throw}}
$peak=[uint64]0
while(-not $process.HasExited){
 $all=@(Get-CimInstance Win32_Process)
 $ids=[Collections.Generic.HashSet[uint32]]::new()
 [void]$ids.Add([uint32]$process.Id)
 do{$added=$false;foreach($row in $all){if($ids.Contains([uint32]$row.ParentProcessId) -and $ids.Add([uint32]$row.ProcessId)){$added=$true}}}while($added)
 $bytes=[uint64]0
 foreach($row in $all){if($ids.Contains([uint32]$row.ProcessId)){$bytes += [uint64]$row.WorkingSetSize}}
 if($bytes -gt $peak){$peak=$bytes}
 Start-Sleep -Milliseconds 500
 $process.Refresh()
}
$process.WaitForExit()
$record=[pscustomobject]@{command=$tool;arguments=$arguments;exit_code=$process.ExitCode;seconds=((Get-Date)-$started).TotalSeconds;sampled_peak_working_set_bytes=$peak;stdout="$prefix.log";stderr="$prefix.stderr.log";measurement='one actual Ape build using established PSX pin and v1 module'}
$record | ConvertTo-Json -Depth 4 | Set-Content "$prefix.record.json" -Encoding utf8
$record | ConvertTo-Json -Depth 4
exit $process.ExitCode
