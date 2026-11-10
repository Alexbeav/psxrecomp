param([switch]$NoOp,[ValidateSet('baseline','generate','use','runtime-configure','recompiler-configure','runtime-build','recompiler-build','ui-build','runtime-test','recompiler-test','ui-test','disc-geometry','thread-control','thread-preservation','source-adapter','cd-pause-preservation','spu-signed-nibble','spu-source-profile','cd-source-profile','state-defer-preservation','display-packet','mdec-source-profile')][string]$Phase='baseline',[int]$Attempt=1,[string]$ExcludeTests='',[switch]$KeepGoing,[string]$Target='',[string]$IncludeTests='',[switch]$NoSanitize,[string]$SourceRoot='')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$core=Split-Path $PSScriptRoot -Parent
$root=Split-Path $core -Parent
$env:TEMP=Join-Path (Join-Path (Split-Path $root -Parent) 'PS1B-165-pair') 'tmp'
$env:TMP=$env:TEMP
$env:PYTHONDONTWRITEBYTECODE='1'
$env:PYTHONUTF8='1'
$priorDriverPath=$env:PATH
(Get-Process -Id $PID).PriorityClass='BelowNormal'
if($NoOp){'noop: serial source/consumer verification; no game or visible window';exit 0}
. 'Z:\Share\Projects\Recompilations\PS1\Tools\eagle\EAGLE-FOREGROUND-CHECK.ps1'
$hostState=[ordered]@{
 recorded=(Get-Date -Format o)
 foreground=Get-EagleForeground
 slots=Get-EagleSlots
 free_memory_bytes=([uint64](Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory*1024)
 claim=@(Get-Content -LiteralPath 'Z:\Share\Projects\Recompilations\PS1\Programme\RESOURCE-STATE.md'|Where-Object{$_ -like '*[[]ps1b165-autonomous[]]*'})
}
if($hostState.claim.Count -eq 0){throw 'Own active PS1B-165 resource claim is missing'}
# Measured peak 1,264,185,344 bytes plus 1 GiB margin for these serial builds.
if($hostState.free_memory_bytes -lt (1264185344+1GB)){throw 'Free memory is below the measured build floor plus margin'}
$sourceHead=(& git -C $core rev-parse HEAD).Trim()
$sourceDirty=@(& git -C $core status --porcelain)
$sdk=Join-Path $root 'inputs/toolchain/bin'
$env:PATH="$sdk;$root/inputs/SDL3/bin;"+$env:PATH
$tool=@(Get-Command python -CommandType Application)[0].Source
$arguments=@('-I','-X','utf8','-B',"$core/tools/runtime_hot_pgo.py",$Phase,'--config',"$root/reports/consumer-790-config.json",'--jobs','1')
$prefix="consumer-790-$Phase-$Attempt"
if($Phase -eq 'display-packet'){
 if($Target -notin @('before','after')){throw 'display-packet needs -Target before or after'}
 $config=Get-Content -LiteralPath "$root/reports/consumer-790-config.json" -Raw|ConvertFrom-Json
 $compiler=($config.configure_args|Where-Object{$_ -like '-DCMAKE_C_COMPILER=*'}) -replace '^-DCMAKE_C_COMPILER=',''
 $pair=Split-Path $config.build -Parent
 $packetRoot=Join-Path $pair "tmp/raw-color-public-$Target"
 $prefix="upstream-display-packet-$Target-$Attempt"
 $arguments=@('-I','-X','utf8','-B',"$packetRoot/runtime/tests/test_display_ring_capture.py",'--cc',$compiler,'--work-dir',"$packetRoot/native-$Attempt")
}elseif($Phase -in @('disc-geometry','thread-control','thread-preservation','source-adapter','cd-pause-preservation','spu-signed-nibble','spu-source-profile','cd-source-profile','state-defer-preservation','mdec-source-profile')){
 $prefix="upstream-$Phase-$Attempt"
 if($Phase -eq 'mdec-source-profile'){
  $arguments=@('-I','-X','utf8','-B',"$core/tools/verify_mdec_source_profile.py",'--attempt',$Attempt)
  if($IncludeTests){$arguments+=@('--include',$IncludeTests)}
 }elseif($Phase -eq 'cd-source-profile'){
  $arguments=@('-I','-X','utf8','-B',"$core/tools/verify_cd_source_profile.py",'--attempt',$Attempt)
  if($IncludeTests){$arguments+=@('--include',$IncludeTests)}
 }elseif($Phase -eq 'spu-source-profile'){
  $arguments=@('-I','-X','utf8','-B',"$core/tools/verify_spu_source_profile.py",'--attempt',$Attempt)
  if($IncludeTests){$arguments+=@('--include',$IncludeTests)}
 }elseif($Phase -eq 'state-defer-preservation'){
  $arguments=@('-I','-X','utf8','-B',"$core/tools/verify_state_defer_preservation.py",'--attempt',$Attempt)
 }elseif($Phase -eq 'spu-signed-nibble'){
  $arguments=@('-I','-X','utf8','-B',"$core/tools/verify_spu_signed_nibble.py",'--attempt',$Attempt)
  if($NoSanitize){$arguments+='--no-sanitize'}
  if($SourceRoot){$arguments+=@('--source-root',$SourceRoot)}
 }elseif($Phase -eq 'cd-pause-preservation'){
  $arguments=@('-I','-X','utf8','-B',"$core/tools/verify_cd_pause_preservation.py",'--attempt',$Attempt)
 }elseif($Phase -eq 'disc-geometry'){
  $arguments=@('-I','-X','utf8','-B',"$core/tools/upstream_geometry_verify.py",'--attempt',$Attempt)
 }elseif($Phase -eq 'source-adapter'){
  $arguments=@('-I','-X','utf8','-B',"$core/tools/verify_studio_source_pair.py",'--attempt',$Attempt)
 }else{
  $config=Get-Content -LiteralPath "$root/reports/consumer-790-config.json" -Raw|ConvertFrom-Json
  $compiler=($config.configure_args|Where-Object{$_ -like '-DCMAKE_C_COMPILER=*'}) -replace '^-DCMAKE_C_COMPILER=',''
  $env:PATH=(Split-Path $compiler -Parent)+';'+$env:PATH
  $arguments=@('-I','-X','utf8','-B',"$core/runtime/tests/test_changethread_in_handler.py",'--cc',$compiler)
  if($Phase -eq 'thread-control'){$arguments+=@('--control','--src-root',"$root/tmp/thread-before/runtime")}
 }
}elseif($Phase.Contains('-')){
 $kind,$operation=$Phase.Split('-')
 $config=Get-Content -LiteralPath "$root/reports/consumer-790-config.json" -Raw|ConvertFrom-Json
 $pair=Split-Path $config.build -Parent
 $build=if($kind -eq 'ui'){Join-Path $root 'build-ui'}else{Join-Path $pair $(if($kind -eq 'runtime'){'rt'}else{'rc'})}
 $tool=Join-Path $sdk 'cmake.exe'
 $prefix="upstream-tree-$Phase-$Attempt"
 if($operation -eq 'configure'){
  $arguments=@('-S',"$core/$kind",'-B',$build,'-G','Ninja')
  $arguments+=@($config.configure_args|Where-Object{$_ -like '-DCMAKE_*COMPILER=*' -or $_ -like '-DCMAKE_MAKE_PROGRAM=*' -or $_ -like '-DSDL3_DIR=*' -or $_ -like '-DZLIB_ROOT=*' -or $_ -like '-DRECOMP_*' -or $_ -like '-DFETCHCONTENT_SOURCE_DIR_*'})
  $arguments+=@('-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG','-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG','-DCMAKE_OBJECT_PATH_MAX=180','-DBUILD_TESTING=ON','-DPSX_LAUNCHER_MODULE=ON','-DPSX_STATIC_RUNTIME=OFF','-DPSX_ENABLE_VULKAN=OFF')
  if($kind -eq 'recompiler'){$arguments+=@("-DPSX_RUNTIME_HOT_PGO_TEST_SCRATCH_ROOT=$pair/pgo")}
 }elseif($operation -eq 'build'){
  $arguments=@('--build',$build,'--parallel','1')
  if($Target){$arguments+=@('--target',$Target)}
  if($KeepGoing){$arguments+=@('--','-k','0')}
 }
 else{
  $tool=Join-Path $sdk 'ctest.exe'
  if(-not(Test-Path -LiteralPath $tool)){$tool=@(Get-Command ctest.exe -CommandType Application)[0].Source}
  $compiler=($config.configure_args|Where-Object{$_ -like '-DCMAKE_CXX_COMPILER=*'}) -replace '^-DCMAKE_CXX_COMPILER=',''
  $env:PATH=(Split-Path $compiler -Parent)+';'+$env:PATH
  $env:SDL_AUDIODRIVER='dummy'
  $arguments=@('--test-dir',$build,'--output-on-failure','--parallel','1')
  if($IncludeTests){$arguments+=@('--tests-regex',$IncludeTests)}
  if($ExcludeTests){$arguments+=@('--exclude-regex',$ExcludeTests)}
 }
}
$out=Join-Path $root "reports/$prefix.stdout.log"
$err=Join-Path $root "reports/$prefix.stderr.log"
if((Test-Path -LiteralPath $out)-or(Test-Path -LiteralPath $err)){throw 'Evidence exists; use a new attempt'}
$started=Get-Date
$process=Start-Process -FilePath $tool -ArgumentList @($arguments|ForEach-Object{'"'+$_+'"'}) -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
$peak=[uint64]0
while(-not $process.HasExited){
 $all=@(Get-CimInstance Win32_Process)
 $ids=[Collections.Generic.HashSet[uint32]]::new();[void]$ids.Add([uint32]$process.Id)
 do{$added=$false;foreach($row in $all){if($ids.Contains([uint32]$row.ParentProcessId)-and $ids.Add([uint32]$row.ProcessId)){$added=$true}}}while($added)
 $bytes=[uint64]0;foreach($row in $all){if($ids.Contains([uint32]$row.ProcessId)){$bytes += [uint64]$row.WorkingSetSize}}
 if($bytes -gt $peak){$peak=$bytes}
 Start-Sleep -Milliseconds 250
 $process.Refresh()
}
$process.WaitForExit()
$env:PATH=$priorDriverPath
$record=[ordered]@{wrapper_commit=(& git -C $core log -1 --format=%H -- tools/upstream_pgo_verify.ps1).Trim();wrapper_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant();phase=$Phase;source_head=$sourceHead;source_dirty=$sourceDirty;host_check=$hostState;command=$tool;arguments=$arguments;exit_code=$process.ExitCode;seconds=((Get-Date)-$started).TotalSeconds;sampled_peak_tree_working_set_bytes=$peak;stdout=$out;stderr=$err}
$record|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $root "reports/$prefix.json") -Encoding utf8
[ordered]@{phase=$Phase;attempt=$Attempt;exit_code=$process.ExitCode;seconds=$record.seconds;record=(Join-Path $root "reports/$prefix.json")}|ConvertTo-Json
exit $process.ExitCode
