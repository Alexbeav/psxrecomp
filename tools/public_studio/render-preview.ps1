param([switch]$NoOp,[ValidateSet("empty","selected","settings")][string]$State="empty")
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $root 'tmp'; $env:TMP=$env:TEMP
(Get-Process -Id $PID).PriorityClass='BelowNormal'
if($NoOp){'noop: headless preview render'; exit 0}
$browser='C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
$url=([Uri](Join-Path $root 'reports\studio-retro-preview.html')).AbsoluteUri
$url+="#"+$State
$profile=Join-Path $root 'tmp\preview-browser-profile'
$shot=Join-Path $root ("reports\studio-retro-"+$State+".png")
$arguments=@('--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check','--disable-background-networking',"--user-data-dir=`"$profile`"",'--window-size=1100,820',"--screenshot=`"$shot`"",$url)
$process=Start-Process -FilePath $browser -ArgumentList $arguments -WindowStyle Hidden -PassThru
$process.PriorityClass='BelowNormal'
if(-not $process.WaitForExit(45000)){throw "Headless browser did not exit within45s; process $($process.Id) retained for inspection"}
if($process.ExitCode -ne 0){throw "Headless browser exit $($process.ExitCode)"}
Get-Item -LiteralPath $shot | Select-Object FullName,Length

