param([switch]$NoOp,[ValidateSet('Configure','Build','Test','LoaderBuild','LoaderTest','CoreConfigure','CoreBuild','CoreCheck')][string]$Step='Configure',[int]$Attempt=1)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $root 'tmp';$env:TMP=$env:TEMP;$env:PYTHONDONTWRITEBYTECODE='1'
$env:PATH="$root\inputs\toolchain\bin;$root\inputs\SDL3\bin;"+$env:PATH
(Get-Process -Id $PID).PriorityClass='BelowNormal'
if($NoOp){'noop: one serial headless module payload';exit 0}
$build=Join-Path $root $(if($Step -like 'Core*'){'build-core-single'}else{'build-ui'})
$logs=Join-Path $root 'reports'
if($Step -like 'Core*'){
 $project=Join-Path $root 'tmp\core-project'
 New-Item -ItemType Directory -Path $project -Force | Out-Null
 $corePath=(Join-Path $root 'core').Replace('\','/')
 @"
cmake_minimum_required(VERSION 3.20)
project(studio_core_compile LANGUAGES C CXX)
set(PSXRECOMP_ROOT "$corePath")
include("$corePath/runtime/runtime.cmake")
psxrecomp_add_runtime_target(psx-runtime EXE_NAME "Studio_Core_Check")
"@ | Set-Content (Join-Path $project 'CMakeLists.txt') -Encoding utf8
}
$identityRows=@(& git -C (Join-Path $root 'ui') ls-files -- src cmake CMakeLists.txt recomp_ui.cmake | Sort-Object | ForEach-Object {
    $relative=$_
    $digest=(Get-FileHash -LiteralPath (Join-Path $root "ui/$relative") -Algorithm SHA256).Hash.ToLower()
    "$digest  $relative"
})
foreach($header in @('psx_menu_host.h','host_overlay.h')) {
    $digest=(Get-FileHash -LiteralPath (Join-Path $root "core/runtime/include/$header") -Algorithm SHA256).Hash.ToLower()
    $identityRows += "$digest  core-contract/$header"
}
$identityBytes=[Text.Encoding]::UTF8.GetBytes(($identityRows -join "`n")+"`n")
$moduleIdentity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($identityBytes)).ToLower()
[IO.File]::WriteAllBytes((Join-Path $root "reports/module-source-$moduleIdentity.txt"),$identityBytes)
$tool=Join-Path $root 'inputs\toolchain\bin\cmake.exe'
$arguments=switch($Step){
 'CoreConfigure' {@('-S',$project,'-B',$build,'-G','Ninja',"-DCMAKE_C_COMPILER=$root\inputs\toolchain\bin\clang.exe","-DCMAKE_CXX_COMPILER=$root\inputs\toolchain\bin\clang++.exe","-DCMAKE_MAKE_PROGRAM=$root\inputs\toolchain\bin\ninja.exe","-DSDL3_DIR=$root\inputs\SDL3\lib\cmake\SDL3","-DZLIB_ROOT=$root\inputs\toolchain\deps",'-DCMAKE_BUILD_TYPE=Release','-DPSX_LAUNCHER_MODULE=ON','-DPSX_STATIC_RUNTIME=OFF',"-DRECOMP_UI_ROOT=$root\ui","-DRECOMP_LAUNCHER_MODULE_FILE=$root\build-ui\recomp-launcher-v2.dll",'-DBUILD_TESTING=ON')}
 'CoreBuild' {@('--build',$build,'--parallel','1','--target','psx-runtime','--','-k','0')}
 'CoreCheck' {$tool=Join-Path $build 'Studio_Core_Check.exe';@('--launcher-module-check')}
 'Configure' {@('-S',"$root\ui",'-B',$build,'-G','Ninja',"-DCMAKE_C_COMPILER=$root\inputs\toolchain\bin\clang.exe","-DCMAKE_CXX_COMPILER=$root\inputs\toolchain\bin\clang++.exe","-DCMAKE_MAKE_PROGRAM=$root\inputs\toolchain\bin\ninja.exe","-DCMAKE_PREFIX_PATH=$root\inputs\SDL3",'-DCMAKE_BUILD_TYPE=Release','-DRECOMP_UI_BUILD_LAUNCHER_MODULE=ON',"-DRECOMP_LAUNCHER_V2_BUILD_IDENTITY=$moduleIdentity",'-DRECOMP_UI_ENABLE_MODS=ON',"-DRECOMP_UI_PSXRECOMP_ROOT=$root\core",'-DBUILD_TESTING=ON')}
 'Build' {@('--build',$build,'--parallel','1','--target','recomp-launcher-module','recomp-ui-launcher-module-contract-test','recomp-ui-launcher-mouse-source-test','recomp-ui-launcher-negcon-mode-test','recomp-ui-launcher-record-replay-test','recomp-ui-launcher-pad-mode-test','--','-k','0')}
 'Test' {$tool=@(Get-Command ctest.exe -CommandType Application -ErrorAction Stop)[0].Source;@('--test-dir',$build,'--output-on-failure','-R','recomp-ui-launcher-(module-contract|mouse-source|negcon-mode|record-replay|pad-mode)$')}
 'LoaderBuild' {@('--build',$build,'--parallel','1','--target','recomp-ui-module-loader-probe')}
 'LoaderTest' {$tool=@(Get-Command ctest.exe -CommandType Application -ErrorAction Stop)[0].Source;@('--test-dir',$build,'--output-on-failure','-V','-R','recomp-ui-module-loader-real$')}
}
$prefix=if($Attempt -eq 1){"module-$Step"}else{"module-$Step-$Attempt"}
$stdout=Join-Path $logs "$prefix.stdout.log"
$stderr=Join-Path $logs "$prefix.stderr.log"
if((Test-Path $stdout) -or (Test-Path $stderr)){throw 'Evidence logs already exist; choose a new Attempt number'}
$quoted=@($arguments | ForEach-Object { '"'+$_+'"' })
$started=Get-Date
$process=Start-Process -FilePath $tool -ArgumentList $quoted -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
try{$process.PriorityClass='BelowNormal'}catch{if(-not $process.HasExited){throw}}
$peak=[uint64]0;$maxProcesses=0
while(-not $process.HasExited){
 $all=@(Get-CimInstance Win32_Process)
 $ids=[Collections.Generic.HashSet[uint32]]::new();[void]$ids.Add([uint32]$process.Id)
 do{$added=$false;foreach($row in $all){if($ids.Contains([uint32]$row.ParentProcessId) -and $ids.Add([uint32]$row.ProcessId)){$added=$true}}}while($added)
 $owned=@($all | Where-Object {$ids.Contains([uint32]$_.ProcessId)})
 $bytes=[uint64]0
 foreach($row in $owned){$bytes += [uint64]$row.WorkingSetSize}
 if($bytes -gt $peak){$peak=$bytes};if($owned.Count -gt $maxProcesses){$maxProcesses=$owned.Count}
 Start-Sleep -Milliseconds 300
 $process.Refresh()
}
$process.WaitForExit()
$record=[pscustomobject]@{step=$Step;command=$tool;arguments=$arguments;exit_code=$process.ExitCode;seconds=((Get-Date)-$started).TotalSeconds;sampled_peak_working_set_bytes=$peak;max_observed_processes=$maxProcesses;stdout=$stdout;stderr=$stderr}
$record | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $logs "$prefix-record.json") -Encoding utf8
$record | ConvertTo-Json -Depth 4
if($process.ExitCode -ne 0){Get-Content $stdout -Tail 15;Get-Content $stderr -Tail 20;exit $process.ExitCode}
