param(
  [Parameter(Mandatory)] [string]$Job
)
# Runs one replay job on a Windows fleet host and writes its five-part record.
# Launched detached (Win32_Process.Create, below-normal priority, session 0), so it
# outlives the ssh session that started it and can never show a window.
# job.json: id, program, arguments[], cwd, out, wrapper_commit, env{}.
# Writes <out>\wrapper-birth.json at start and <out>\wrapper-result.json at the end
# (command, exit code, duration, log paths). The measurement is read from the product's
# own result files by the caller.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# One physical PS1B-526 driver at a time, including smaller public diagnostics.
# The capacity controller remains alive while another owned payload finishes.
$driverMutex = [Threading.Mutex]::new($false, 'Local\PS1B526-Eagle-Serial-Driver')
$driverOwned = $false
try {
try { $driverOwned = $driverMutex.WaitOne() }
catch [Threading.AbandonedMutexException] { $driverOwned = $true }

function Quote-Arg([string]$a) {
  if ($a.Length -gt 0 -and $a -notmatch '[\s"]') { return $a }
  $s = $a -replace '(\\*)"', '$1$1\"'
  $s = $s -replace '(\\+)$', '$1$1'
  return '"' + $s + '"'
}

$j = Get-Content $Job -Raw -Encoding utf8 | ConvertFrom-Json
$out = [string]$j.out
New-Item -ItemType Directory -Force -Path $out | Out-Null
$tmp = Join-Path $out 'tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TEMP = $tmp; $env:TMP = $tmp; $env:PYTHONDONTWRITEBYTECODE = '1'; $env:PYTHONUNBUFFERED = '1'
if ($j.PSObject.Properties['env'] -and $j.env) { foreach ($p in $j.env.PSObject.Properties) { Set-Item -Path "env:$($p.Name)" -Value ([string]$p.Value) } }

$argList = @($j.arguments | ForEach-Object { [string]$_ })
$argString = ($argList | ForEach-Object { Quote-Arg $_ }) -join ' '
$stdout = Join-Path $out 'wrapper-stdout.log'
$stderr = Join-Path $out 'wrapper-stderr.log'
$start = Get-Date
$timer = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath ([string]$j.program) -ArgumentList $argString -WorkingDirectory ([string]$j.cwd) `
  -RedirectStandardOutput $stdout -RedirectStandardError $stderr -NoNewWindow -PassThru
$null = $p.Handle
try { $p.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal } catch { }
[ordered]@{
  id = [string]$j.id; wrapper_pid = $PID; child_pid = $p.Id; start = $start.ToString('o')
  program = [string]$j.program; arguments = $argList; cwd = [string]$j.cwd
  wrapper_commit = [string]$j.wrapper_commit; host = $env:COMPUTERNAME
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'wrapper-birth.json') -Encoding utf8
$p.WaitForExit()
$timer.Stop()
[ordered]@{
  id = [string]$j.id; exit_code = $p.ExitCode; duration_s = [Math]::Round($timer.Elapsed.TotalSeconds, 1)
  start = $start.ToString('o'); end = (Get-Date).ToString('o')
  command = ((Quote-Arg ([string]$j.program)) + ' ' + $argString)
  wrapper_commit = [string]$j.wrapper_commit; stdout = $stdout; stderr = $stderr
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'wrapper-result.json') -Encoding utf8
exit 0
} finally {
    if ($driverOwned) { $driverMutex.ReleaseMutex() }
    $driverMutex.Dispose()
}
