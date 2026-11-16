param([switch]$NoOp,[ValidateSet('CompileMouse','RunMouse')][string]$Step='CompileMouse',[int]$Players=2,[ValidateSet("mouse","negcon","guncon")][string]$Device="mouse",[int]$Attempt=1)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $root 'tmp'
$env:TMP=$env:TEMP
$env:PYTHONDONTWRITEBYTECODE='1'
$env:PATH="$root\inputs\toolchain\bin;"+$env:PATH
(Get-Process -Id $PID).PriorityClass='BelowNormal'
if($NoOp){'noop: serial headless production core test';exit 0}
$output=Join-Path $root "tmp\sio-$Device-p$Players.exe"
$prefix=Join-Path $root "reports\core-$Step-$Device-p$Players-$Attempt"
if(Test-Path "$prefix.stdout.log"){throw 'Choose unused Attempt'}
$tool=if($Step -eq 'CompileMouse'){Join-Path $root 'inputs\toolchain\bin\clang.exe'}else{$output}
$arguments=if($Step -eq 'CompileMouse'){@('-std=c11','-O1',"-DPSX_MAX_PLAYERS=$Players","-I$root\core\runtime\include","$root\core\runtime\tests\test_sio_$Device.c",'-o',$output)}else{@()}
$started=Get-Date
& $tool @arguments 1>"$prefix.stdout.log" 2>"$prefix.stderr.log"
$code=$LASTEXITCODE
[pscustomobject]@{step=$Step;players=$Players;command=$tool;arguments=$arguments;exit_code=$code;seconds=((Get-Date)-$started).TotalSeconds;stdout="$prefix.stdout.log";stderr="$prefix.stderr.log"} | ConvertTo-Json -Depth 4 | Set-Content "$prefix.record.json"
Get-Content "$prefix.stdout.log"
Get-Content "$prefix.stderr.log"
exit $code
