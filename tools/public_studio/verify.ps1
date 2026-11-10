param([switch]$NoOp)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP = Join-Path $root 'tmp'
$env:TMP = $env:TEMP
$env:PYTHONDONTWRITEBYTECODE = '1'
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
if ($NoOp) { Write-Output 'noop: ready'; exit 0 }
& python -I -m py_compile "$root\core\tools\public_studio\backend.py" "$root\core\tools\public_studio\probe_disc.py" "$root\core\tools\public_studio\settings.py" "$root\core\tools\public_studio\selection.py" "$root\core\tools\public_studio\test_backend.py"
if ($LASTEXITCODE -ne 0) { throw 'py_compile failed' }
& python -B "$root\core\tools\public_studio\test_backend.py"
if ($LASTEXITCODE -ne 0) { throw 'controller tests failed' }
$parseErrors = $null
$tokens = $null
[System.Management.Automation.Language.Parser]::ParseFile("$root\core\tools\public_studio\setup_public.ps1", [ref]$tokens, [ref]$parseErrors) | Out-Null
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
Write-Output 'powershell parse: passed'
