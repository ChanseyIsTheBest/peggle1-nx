# A run of the scripted route (fresh seed) with the indirect-call census and the runtime census.
#   powershell -NoProfile -ExecutionPolicy Bypass -File census.ps1 [-Seconds 150] [-Out out\rounds\indirect_census.txt] [-Keys "k=v,..."]
param([int]$Seconds = 150, [string]$Out = 'out\rounds\indirect_census.txt', [string]$Keys = '', [switch]$NoCensus)
Set-Location (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script = "900:A,1020:A,1200:A,1320:A,1500:A,1700:A,1900:A,2100:A,2400:A,2700:A,3000:A,3300:A"
$set = @()
foreach ($kv in ($Keys -split ',' | Where-Object { $_ })) {
    $k, $v = $kv -split '=', 2
    $name = 'REX_' + $k.ToUpper()
    Set-Item -Path "env:$name" -Value $v
    $set += $name
}
$env:REX_MASSEFFECT_RENDERER_NATIVE = 'true'
if (-not $NoCensus) {
    $env:REX_PEGGLE1_DIAG_INDIRECT_CENSUS = (Join-Path (Get-Location) $Out)
    $env:REX_PEGGLE1_DIAG_RUNTIME_CENSUS = 'true'
}
$run = Join-Path (Get-Location) "out\pc\test_userdata\run_census"
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
Copy-Item -Recurse (Join-Path (Get-Location) "out\pc\test_userdata\seed_empty") $run
$env:REX_USER_DATA_ROOT = $run
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -Seconds $Seconds -InputScript $script -Tail 1 | Select-Object -First 1
foreach ($n in $set + @('REX_MASSEFFECT_RENDERER_NATIVE', 'REX_USER_DATA_ROOT', 'REX_PEGGLE1_DIAG_INDIRECT_CENSUS', 'REX_PEGGLE1_DIAG_RUNTIME_CENSUS')) { Remove-Item "env:$n" -ErrorAction SilentlyContinue }
$f = (Get-ChildItem out\pc\logs\peggle1_*.log | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
"LOG $f keys [$Keys]"
"ring reports: " + (Select-String -CaseSensitive -Path $f -Pattern "\[native\] ring thread:").Count + "; DIFFERENCE/MISMATCH: " + (Select-String -CaseSensitive -Path $f -Pattern "DIFFERENCE|MISMATCH").Count
Select-String -CaseSensitive -Path $f -Pattern "self-check|flush-mode flow|\[census\]|indirect census|\[enhancements\] masters" | Select-Object -Last 8 | ForEach-Object { $l = $_.Line; $l.Substring([Math]::Min(48, $l.Length)) }
if (Get-Process peggle1 -ErrorAction SilentlyContinue) { "WARNING: a peggle1 process is still running" }
