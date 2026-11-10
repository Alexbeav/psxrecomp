param([switch]$NoOp,[switch]$SourceBusBuild,[switch]$VerifyBuildKind,[ValidateSet('ConfigureEmitters','BuildEmitters','Prepare','RetailBios','RetailStamp','OpenBios','OpenStamp','Generate','Configure','Build','ModuleCheck','Record','Play')][string]$Step='ConfigureEmitters',[int]$Attempt=1,[ValidateSet('b','n')][string]$BuildName='b',[string]$ProductDirectory='',[int]$AnchorVblank=0)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$core=Split-Path $PSScriptRoot -Parent
$root=Split-Path $core -Parent
$pair=Join-Path (Split-Path $root -Parent) 'PS1B-165-pair'
$project=Join-Path $pair 'p'
$wrapperSha=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$wrapperCommit=(& git -C $core log -1 --format=%H -- tools/preserved_consumer_verify.ps1).Trim()
$env:TEMP=Join-Path $pair 'tmp';$env:TMP=$env:TEMP;$env:PYTHONDONTWRITEBYTECODE='1';$env:CMAKE_BUILD_PARALLEL_LEVEL='1'
(Get-Process -Id $PID).PriorityClass='BelowNormal'

# Classify only a configured, non-LTO runtime/resource/link graph. Every
# unknown line, regeneration or other compilation retains the full-build kind.
function Get-ConsumerBuildKind([string[]]$Plan,[hashtable]$Cache,[string[]]$GraphFlags) {
 $full=[pscustomobject]@{kind='full-or-unknown';reason='unproven graph or options'}
 if($Cache['PSX_RUNTIME_IPO'] -notin @('OFF','FALSE','0')){return $full}
 if($Cache['CMAKE_INTERPROCEDURAL_OPTIMIZATION'] -in @('ON','TRUE','1')){return $full}
 $flags=(@($Cache.Values)+@($GraphFlags)) -join ' '
 if($flags -match '(?i)(?:^|[\s;])(?:-flto(?:[=\s]|$)|/GL\b|/LTCG\b|-fprofile-(?:generate|use|instr-generate|instr-use))'){return $full}
 $seen=0
 foreach($line in @($Plan)){
  $line=($line -replace '^\[\d+/\d+\]\s*','').Trim()
  if(-not $line){continue}
  if($line -eq 'ninja: no work to do.'){$seen++;continue}
  if($line -match '^Building (?:C|CXX) object CMakeFiles/psx-runtime\.dir/.*/runtime/src/[^ ]+\.(?:c|cpp)\.obj$' -or
     $line -eq 'Building C object CMakeFiles/psx-runtime.dir/psx_bios_registry.c.obj' -or
     $line -eq 'Building RC object CMakeFiles/psx-runtime_app_icon.dir/psx-runtime_app_icon.rc.obj' -or
     $line -eq 'Linking CXX executable Ape_Modern_Pair.exe'){$seen++;continue}
  return $full
 }
 if(-not $seen){return $full}
 return [pscustomobject]@{kind='runtime-only';reason='configured runtime/resource/link plan, no generated/BIOS/dependency work or LTO'}
}
if($NoOp){
 if($VerifyBuildKind){
  $cache=@{PSX_RUNTIME_IPO='OFF';CMAKE_C_FLAGS_RELEASE='-O1 -DNDEBUG'}
  $compile='[1/2] Building C object CMakeFiles/psx-runtime.dir/I_/owned/core/runtime/src/spu.c.obj'
  $link='[2/2] Linking CXX executable Ape_Modern_Pair.exe'
  $cases=@(
   @{name='runtime';plan=@($compile,$link);flags=@();expected='runtime-only'},
   @{name='registry-resource';plan=@('Building C object CMakeFiles/psx-runtime.dir/psx_bios_registry.c.obj','Building RC object CMakeFiles/psx-runtime_app_icon.dir/psx-runtime_app_icon.rc.obj',$link);flags=@();expected='runtime-only'},
   @{name='no-work';plan=@('ninja: no work to do.');flags=@();expected='runtime-only'},
   @{name='generated';plan=@('Building C object CMakeFiles/psx-runtime.dir/generated/game.c.obj',$link);flags=@();expected='full-or-unknown'},
   @{name='dependency';plan=@('Building C object _deps/lib.dir/object.c.obj');flags=@();expected='full-or-unknown'},
   @{name='reconfigure';plan=@('Re-running CMake...');flags=@();expected='full-or-unknown'},
   @{name='LTO';plan=@($compile,$link);flags=@('-O1 -flto=thin');expected='full-or-unknown'},
   @{name='unknown';plan=@('Generating sources');flags=@();expected='full-or-unknown'},
   @{name='empty';plan=@();flags=@();expected='full-or-unknown'})
  foreach($case in $cases){$got=Get-ConsumerBuildKind $case.plan $cache $case.flags;if($got.kind -ne $case.expected){throw "classifier failed: $($case.name)"}}
  $cache.PSX_RUNTIME_IPO='ON'
  if((Get-ConsumerBuildKind @($compile,$link) $cache @()).kind -ne 'full-or-unknown'){throw 'classifier accepted IPO'}
  "CLASSIFIER_CONTROLS_PASS=$($cases.Count+1)"
 }
 'noop: one serial modern consumer payload; no system toolchain activation';exit 0
}

. 'Z:\Share\Projects\Recompilations\PS1\Tools\eagle\EAGLE-FOREGROUND-CHECK.ps1'
$hostState=[ordered]@{recorded=(Get-Date -Format o);foreground=Get-EagleForeground;slots=Get-EagleSlots;free_memory_bytes=([uint64](Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory*1024);claim=@(Get-Content -LiteralPath 'Z:\Share\Projects\Recompilations\PS1\Programme\RESOURCE-STATE.md'|Where-Object{$_ -like '*[[]ps1b165-autonomous[]]*'})}

$memoryFloor=if($Step -eq 'Build'){7415885824+1GB}else{1264185344+1GB}
$buildKind=[ordered]@{kind=if($Step -eq 'Build'){'full-or-unknown'}else{'existing non-build kind'};reason='initial kind';floor_bytes=$memoryFloor}
function Write-ConsumerRefusal([string]$Reason) {
 $refusalPath=Join-Path $root "reports/modern-consumer-$Step-$Attempt.refusal.json"
 if(Test-Path $refusalPath){throw 'Refusal evidence exists; choose a new attempt'}
 [ordered]@{step=$Step;attempt=$Attempt;source_head=(& git -C $core rev-parse HEAD).Trim();result='not_started';wrapper_commit=$wrapperCommit;wrapper_sha256=$wrapperSha;reason=$Reason;host_check=$hostState;build_kind=$buildKind;required_memory_bytes=$memoryFloor;read=(Get-Date -Format o)}|
  ConvertTo-Json -Depth 7|Set-Content $refusalPath -Encoding utf8
}
if($hostState.claim.Count -eq 0){Write-ConsumerRefusal 'own claim missing';throw 'Own active resource claim is missing'}
# Non-build behavior is unchanged. Build admission follows the configured graph
# below; failed or unknown classification retains the original full-build floor.
if($Step -ne 'Build' -and $hostState.free_memory_bytes -lt $memoryFloor){Write-ConsumerRefusal 'memory below measured kind plus margin';throw 'Free memory is below measured payload floor plus margin'}
$consumerBuild=Join-Path $pair $BuildName
if($ProductDirectory){
 if($Step -notin @('ModuleCheck','Record','Play')){throw 'Delivered product is read-only verification only'}
 $consumerBuild=(Resolve-Path -LiteralPath $ProductDirectory).Path
 $productExecutables=@(Get-ChildItem -LiteralPath $consumerBuild -Filter '*.exe' -File)
 if($productExecutables.Count -ne 1){throw 'Expected one delivered game executable'}
}
$sdk=Join-Path $root 'inputs/toolchain/bin'
$env:PATH="$sdk;$root/inputs/SDL3/bin;"+$env:PATH
$tool=Join-Path $sdk 'cmake.exe';$working=$root
$cc=@("-DCMAKE_C_COMPILER=$sdk/clang.exe","-DCMAKE_CXX_COMPILER=$sdk/clang++.exe","-DCMAKE_MAKE_PROGRAM=$sdk/ninja.exe")
$arguments=switch($Step){
 'ConfigureEmitters'{@('-S',"$core/recompiler",'-B',"$pair/e",'-G','Ninja')+$cc+@('-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF','-DPSXRECOMP_ENABLE_CHD=OFF','-DPSXRECOMP_STATIC_CLI=ON')}
 'BuildEmitters'{@('--build',"$pair/e",'--parallel','1','--target','psxrecomp-game','psxrecomp-bios')}
 'Prepare'{$tool=@(Get-Command python -CommandType Application)[0].Source;@('-X','utf8','-B',"$core/tools/prepare_disc.py","$project/disc/Ape Escape (USA).cue",'--config',"$project/game.toml",'--project-root',$project)}
 'RetailBios'{$tool=Join-Path $pair 'e/psxrecomp-bios.exe';$working=$core;@('--config','bios/SCPH1001.toml')}
 'OpenBios'{$tool=Join-Path $pair 'e/psxrecomp-bios.exe';$working=$core;@('--config','bios/OpenBIOS.toml')}
 'RetailStamp'{@("-DROOT=$core",'-DPROFILE=bios/SCPH1001.toml','-P',"$core/tools/bios_emitter_fingerprint.cmake")}
 'OpenStamp'{@("-DROOT=$core",'-DPROFILE=bios/OpenBIOS.toml','-P',"$core/tools/bios_emitter_fingerprint.cmake")}
 'Generate'{$tool=Join-Path $pair 'e/psxrecomp-game.exe';$working=$project;@('--config',"$project/game.toml")}
 'Configure'{@('-S',$project,'-B',$consumerBuild,'-G','Ninja')+$cc+@('-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG','-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG',"-DSDL3_DIR=$root/inputs/SDL3/lib/cmake/SDL3","-DZLIB_ROOT=$root/inputs/toolchain/deps",'-DPSX_LAUNCHER_MODULE=ON','-DPSX_STATIC_RUNTIME=OFF',"-DRECOMP_UI_ROOT=$root/ui","-DRECOMP_LAUNCHER_MODULE_FILE=$root/build-ui/recomp-launcher-v2.dll",'-DBUILD_TESTING=OFF','-DPSX_ENABLE_VULKAN=OFF')+@($(if($SourceBusBuild){'-DPSX_SOURCE_BUS_BOUNDARY=ON'}else{'-DPSX_SOURCE_BUS_BOUNDARY=OFF'}),$(if($SourceBusBuild){'-DPSX_NATIVE_CPU_SPANS=OFF'}else{'-DPSX_NATIVE_CPU_SPANS=ON'}))}
 'Build'{@('--build',$consumerBuild,'--parallel','1','--target','psx-runtime')}
 'Record' {$tool=@(Get-Command python -CommandType Application)[0].Source;@('-X','utf8','-B',"$core/tools/preserved_player_verify.py",'record','--attempt',$Attempt,'--build-name',$BuildName)}
 'Play' {$tool=@(Get-Command python -CommandType Application)[0].Source;@('-X','utf8','-B',"$core/tools/preserved_player_verify.py",'play','--attempt',$Attempt,'--build-name',$BuildName)}
 'ModuleCheck'{$tool=if($ProductDirectory){$productExecutables[0].FullName}else{Join-Path $consumerBuild 'Ape_Modern_Pair.exe'};@('--launcher-module-check')}
}
if($ProductDirectory -and $Step -in @('Record','Play')){$arguments+=@('--product-dir',$consumerBuild)}
if($AnchorVblank -and $Step -eq 'Record'){$arguments+=@('--anchor-vblank',$AnchorVblank)}

if($Step -eq 'Build'){
 $planPath=Join-Path $root "reports/modern-consumer-Build-$Attempt.plan.log"
 $planError=Join-Path $root "reports/modern-consumer-Build-$Attempt.plan.stderr.log"
 if((Test-Path $planPath)-or(Test-Path $planError)){throw 'Build plan evidence exists; choose a new attempt'}
 $cache=@{}
 $cachePath=Join-Path $consumerBuild 'CMakeCache.txt'
 if(Test-Path $cachePath){
  foreach($line in Get-Content -LiteralPath $cachePath){
   if($line -match '^([^#/:][^:]*):[^=]+=(.*)$'){$cache[$Matches[1]]=$Matches[2]}
  }
 }
 # No-op graph inspection never invokes a compiler. A failed inspection is
 # unknown, not permission to use a smaller resource estimate.
 $ninja=Join-Path $sdk 'ninja.exe'
 $planExit=$null
 try{
  $planProcess=Start-Process -FilePath $ninja -ArgumentList @('-C',('"'+$consumerBuild+'"'),'-n','psx-runtime') -WorkingDirectory $root -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput $planPath -RedirectStandardError $planError
  $planExit=$planProcess.ExitCode
 }catch{
  [IO.File]::WriteAllText($planError,"No-op graph inspection failed: "+$_.Exception.Message)
 }
 $plan=if(Test-Path $planPath){@(Get-Content -LiteralPath $planPath | Where-Object {$_ -notlike 'ninja: Entering directory*'})}else{@()}
 $graphFlags=@()
 $graphFile=Join-Path $consumerBuild 'build.ninja'
 if(Test-Path $graphFile){$graphFlags=@(Get-Content -LiteralPath $graphFile | Where-Object {$_ -match '^\s*(FLAGS|LINK_FLAGS)\s*='})}
 $classification=Get-ConsumerBuildKind $plan $cache $graphFlags
 if($null -eq $planExit -or $planExit -ne 0){$classification=[pscustomobject]@{kind='full-or-unknown';reason='no-op graph inspection failed'}}
 $buildKind.reason=$classification.reason
 $reference=Join-Path $root 'reports/modern-consumer-Build-70.json'
 if($classification.kind -eq 'runtime-only' -and (Test-Path $reference)){
  $measured=Get-Content -LiteralPath $reference -Raw|ConvertFrom-Json
  if($measured.exit_code -eq 0 -and $measured.sampled_peak_tree_working_set_bytes -eq 371990528){
   $memoryFloor=[uint64]$measured.sampled_peak_tree_working_set_bytes+1GB
   $buildKind=[ordered]@{kind='runtime-only';reason=$classification.reason;measured_peak_bytes=$measured.sampled_peak_tree_working_set_bytes;margin_bytes=1GB;reference=$reference;reference_sha256=(Get-FileHash $reference -Algorithm SHA256).Hash.ToLowerInvariant();floor_bytes=$memoryFloor}
  }
 }
 $buildKind['plan']=$planPath
 $buildKind['plan_stderr']=$planError
 # Persist the exact later reading used for admission, not an earlier preflight.
 $hostState.recorded=Get-Date -Format o
 $hostState.foreground=Get-EagleForeground
 $hostState.slots=Get-EagleSlots
 $hostState.free_memory_bytes=[uint64](Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory*1024
 $hostState.claim=@(Get-Content -LiteralPath 'Z:\Share\Projects\Recompilations\PS1\Programme\RESOURCE-STATE.md'|Where-Object{$_ -like '*[[]ps1b165-autonomous[]]*'})
 if(-not $hostState.claim.Count){Write-ConsumerRefusal 'own claim missing';throw 'Own active resource claim is missing'}
 if($hostState.free_memory_bytes -lt $memoryFloor){Write-ConsumerRefusal 'memory below measured kind plus margin';throw 'Free memory is below measured payload floor plus margin'}
}
$prefix="modern-consumer-$Step-$Attempt"
$out=Join-Path $root "reports/$prefix.stdout.log";$err=Join-Path $root "reports/$prefix.stderr.log"
if((Test-Path $out)-or(Test-Path $err)){throw 'Evidence exists; choose new Attempt'}
$started=Get-Date
$process=Start-Process -FilePath $tool -ArgumentList @($arguments|ForEach-Object{'"'+$_+'"'}) -WorkingDirectory $working -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
$peak=[uint64]0
while(-not $process.HasExited){
 $all=@(Get-CimInstance Win32_Process)
 $ids=[Collections.Generic.HashSet[uint32]]::new();[void]$ids.Add([uint32]$process.Id)
 do{$added=$false;foreach($row in $all){if($ids.Contains([uint32]$row.ParentProcessId)-and $ids.Add([uint32]$row.ProcessId)){$added=$true}}}while($added)
 $bytes=[uint64]0;foreach($row in $all){if($ids.Contains([uint32]$row.ProcessId)){$bytes += [uint64]$row.WorkingSetSize}}
 if($bytes -gt $peak){$peak=$bytes}
 Start-Sleep -Milliseconds 250;$process.Refresh()
}
$process.WaitForExit()
if($process.ExitCode -eq 0 -and $Step -in @('RetailStamp','OpenStamp')){
 $value=([IO.File]::ReadAllText($out)).Trim()
 if($value -notmatch '^[a-f0-9]{64}$'){throw 'Fingerprint command did not produce one SHA256'}
 $stem=if($Step -eq 'RetailStamp'){'SCPH1001'}else{'OpenBIOS'}
 [IO.File]::WriteAllText((Join-Path $core "generated/$stem.emitter.sha"),$value+"`n",[Text.UTF8Encoding]::new($false))
}
$record=[ordered]@{step=$Step;wrapper_commit=$wrapperCommit;wrapper_sha256=$wrapperSha;source_head=(& git -C $core rev-parse HEAD).Trim();build_name=$BuildName;host_check=$hostState;build_kind=$buildKind;command=$tool;arguments=$arguments;working_directory=$working;exit_code=$process.ExitCode;seconds=((Get-Date)-$started).TotalSeconds;sampled_peak_tree_working_set_bytes=$peak;stdout=$out;stderr=$err}
$record|ConvertTo-Json -Depth 5|Set-Content (Join-Path $root "reports/$prefix.json") -Encoding utf8
$record|ConvertTo-Json -Depth 5
exit $process.ExitCode
