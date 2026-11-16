param([switch]$NoOp)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($NoOp){Write-Output 'noop: copy/hash-check two accepted games, retain text evidence, remove three exact owned batch folders';exit 0}
$root='I:\Projects\Recompilations\PS1\Work\eagle\PS1B-165-studio-module-20261031'
$test2=[IO.Path]::GetFullPath('I:\Documents\Desktop\Recomps\PS1\Test2')
$batch=Join-Path $test2 'build-20261031-194221-d1b601'
$records=Join-Path $root 'reports\test2-clean-delivery'
New-Item -ItemType Directory -Path $records | Out-Null
$products=@(
 @{Source=(Join-Path $batch '01-Metal Gear Solid _USA_ _Rev 1_-Recovered\windows-x64');Name='Metal Gear Solid (USA)';Exe='Metal_Gear_Solid_USA_Disc_1_Rev_1.exe';SHA='6661a1791125e3744641a52c26b7cd489fe3873f3ae55643c20b1ccedf2d919b'},
 @{Source=(Join-Path $batch '02-Resident Evil 2 - Dual Shock Ver. _USA_ _Disc 1_-Recovered\windows-x64');Name='Resident Evil 2 - Dual Shock Ver. (USA)';Exe='Resident_Evil_2_Dual_Shock_Ver_USA_Disc_1.exe';SHA='3af3ede57b3f5fd962dc4f75b25b86ae8e15ea6277fbbdc42ec21623db95e51c'}
)
function Assert-NoGameProcess {
 $processes=@(Get-CimInstance Win32_Process | Where-Object {$_.ExecutablePath -and $_.ExecutablePath.StartsWith($test2+'\',[StringComparison]::OrdinalIgnoreCase)})
 if($processes.Count){throw ('User game is still running; leave its source intact: '+($processes.ProcessId -join ','))}
}
Assert-NoGameProcess
foreach($product in $products){
 $source=[IO.Path]::GetFullPath($product.Source)
 $target=[IO.Path]::GetFullPath((Join-Path $test2 $product.Name))
 if(-not $source.StartsWith($batch+'\',[StringComparison]::OrdinalIgnoreCase) -or [IO.Directory]::GetParent($target).FullName -ne $test2){throw 'Unexpected product path'}
 if(Test-Path -LiteralPath $target){throw "Destination already exists: $target"}
 if((Get-FileHash -LiteralPath (Join-Path $source $product.Exe)).Hash -ne $product.SHA){throw 'Linked executable identity differs'}
 $reparse=@(Get-ChildItem -LiteralPath $source -Recurse -Force | Where-Object {($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0})
 if($reparse.Count){throw 'Product contains a reparse point'}
 Copy-Item -LiteralPath $source -Destination $target -Recurse
 $manifest=@()
 foreach($file in Get-ChildItem -LiteralPath $source -File -Recurse -Force){
  $relative=[IO.Path]::GetRelativePath($source,$file.FullName)
  $hash=(Get-FileHash -LiteralPath $file.FullName).Hash
  if((Get-FileHash -LiteralPath (Join-Path $target $relative)).Hash -ne $hash){throw "Copied file differs: $relative"}
  $manifest+=[pscustomobject]@{File=$relative;Bytes=$file.Length;SHA256=$hash}
 }
 if(@(Get-ChildItem -LiteralPath $target -File -Recurse -Force).Count -ne $manifest.Count){throw 'Copied product file count differs'}
 $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $records ($product.Name+'-files.json')) -Encoding utf8
 Write-Output "VERIFIED $target ($($manifest.Count) files)"
}
$oldNames=@('batch-20261031-183729-66d21c','build-20261031-191108-b65dda','build-20261031-194221-d1b601')
$deletion=@()
foreach($name in $oldNames){
 $path=[IO.Path]::GetFullPath((Join-Path $test2 $name))
 if([IO.Directory]::GetParent($path).FullName -ne $test2 -or -not $path.StartsWith($test2+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Deletion target escaped Test2'}
 $evidence=Join-Path $records $name
 New-Item -ItemType Directory -Path $evidence | Out-Null
 foreach($fileName in @('build.log','batch-record.json','PIN.json')){if(Test-Path -LiteralPath (Join-Path $path $fileName)){Copy-Item -LiteralPath (Join-Path $path $fileName) -Destination (Join-Path $evidence $fileName)}}
 $work=Join-Path $path '.work'
 if(Test-Path -LiteralPath $work){foreach($run in Get-ChildItem -LiteralPath $work -Directory){$from=Join-Path $run.FullName 'evidence';if(Test-Path -LiteralPath $from){$to=Join-Path $evidence $run.Name;New-Item -ItemType Directory -Path $to | Out-Null;foreach($file in Get-ChildItem -LiteralPath $from -File){if($file.Extension -in @('.log','.json','.txt')){Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $to $file.Name)}}}}}
 $bytes=(Get-ChildItem -LiteralPath $path -File -Recurse -Force | Measure-Object -Property Length -Sum).Sum
 $deletion+=[pscustomobject]@{Action='remove owned delivered batch';Bytes=$bytes;Path=$path}
}
$deletion | Export-Csv -LiteralPath (Join-Path $records 'PS1B-165-deletion-list.tsv') -Delimiter "`t" -NoTypeInformation -Encoding utf8
Assert-NoGameProcess
foreach($entry in $deletion){
 $resolved=[IO.Path]::GetFullPath($entry.Path)
 if([IO.Directory]::GetParent($resolved).FullName -ne $test2 -or [IO.Path]::GetFileName($resolved) -notin $oldNames){throw 'Final deletion guard failed'}
 Remove-Item -LiteralPath $resolved -Recurse -Force
}
$remaining=@(Get-ChildItem -LiteralPath $test2)
if($remaining.Count -ne 2 -or @($remaining | Where-Object {$_.Name -notin $products.Name}).Count){throw 'Output root contains unexpected entries'}
Write-Output 'DELIVERY_PASS: Test2 contains exactly two verified game folders'
