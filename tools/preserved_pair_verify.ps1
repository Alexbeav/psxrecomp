param([switch]$NoOp,[ValidateSet('Configure','Build','Test','Tier')][string]$Step='Configure',[int]$Attempt=1)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$core=Split-Path $PSScriptRoot -Parent
$root=Split-Path $core -Parent
$env:TEMP=Join-Path $root 'tmp';$env:TMP=$env:TEMP;$env:PYTHONDONTWRITEBYTECODE='1'
(Get-Process -Id $PID).PriorityClass='BelowNormal'
if($NoOp){'noop: one serial preserved replay engine/control payload';exit 0}
$build=Join-Path $root 'build-preserved-replay'
$project=Join-Path $root 'tmp/preserved-replay-project'
New-Item -ItemType Directory -Path $project -Force|Out-Null
$fragment=(Join-Path $core 'runtime/cmake/preserved_replay_tests.cmake').Replace('\','/')
@"
cmake_minimum_required(VERSION 3.20)
project(preserved_replay LANGUAGES C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
include(CTest)
include("$fragment")
"@|Set-Content (Join-Path $project 'CMakeLists.txt') -Encoding utf8
$sdk=Join-Path $root 'inputs/toolchain/bin'
$tool=Join-Path $sdk 'cmake.exe'
$arguments=switch($Step){
 'Configure'{@('-S',$project,'-B',$build,'-G','Ninja',"-DCMAKE_C_COMPILER=$sdk/clang.exe","-DCMAKE_CXX_COMPILER=$sdk/clang++.exe","-DCMAKE_MAKE_PROGRAM=$sdk/ninja.exe",'-DCMAKE_BUILD_TYPE=Release')}
 'Build'{@('--build',$build,'--parallel','1')}
 'Test'{$tool=@(Get-Command ctest.exe -CommandType Application -ErrorAction Stop)[0].Source;@('--test-dir',$build,'--output-on-failure','-V')}
 'Tier'{$tool=@(Get-Command python -CommandType Application -ErrorAction Stop)[0].Source;@('-X','utf8','-B',(Join-Path $core 'runtime/tests/test_overlay_tier_witness_runtime.py'),'--cc',(Join-Path $sdk 'clang.exe'))}
}
$prefix="preserved-replay-$Step-$Attempt"
$out=Join-Path $root "reports/$prefix.stdout.log";$err=Join-Path $root "reports/$prefix.stderr.log"
if((Test-Path $out)-or(Test-Path $err)){throw 'Evidence exists; choose a new Attempt'}
$started=Get-Date
$process=Start-Process -FilePath $tool -ArgumentList @($arguments|ForEach-Object{'"'+$_+'"'}) -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
$peak=[uint64]0
while(-not $process.HasExited){
 $all=@(Get-CimInstance Win32_Process)
 $ids=[Collections.Generic.HashSet[uint32]]::new();[void]$ids.Add([uint32]$process.Id)
 do{$added=$false;foreach($row in $all){if($ids.Contains([uint32]$row.ParentProcessId)-and $ids.Add([uint32]$row.ProcessId)){$added=$true}}}while($added)
 $bytes=[uint64]0;foreach($row in $all){if($ids.Contains([uint32]$row.ProcessId)){$bytes += [uint64]$row.WorkingSetSize}}
 if($bytes -gt $peak){$peak=$bytes}
 Start-Sleep -Milliseconds 150;$process.Refresh()
}
$process.WaitForExit()
$record=[ordered]@{step=$Step;command=$tool;arguments=$arguments;exit_code=$process.ExitCode;seconds=((Get-Date)-$started).TotalSeconds;sampled_peak_tree_working_set_bytes=$peak;stdout=$out;stderr=$err}
$record|ConvertTo-Json -Depth 5|Set-Content (Join-Path $root "reports/$prefix.json") -Encoding utf8
$record|ConvertTo-Json -Depth 5
exit $process.ExitCode
