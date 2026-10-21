# A timed run (fresh save seed, the scripted route) with some keys, then what the game's network start-up did: the
# [net] lines, the two RakNet threads (routine 825FBDD8), the debug probes of the network manager and any crash.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\net_test.ps1 -Keys "net_offline_sockets=true" [-Seconds 60]
param([string]$Keys = '', [int]$Seconds = 60)
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
$run = Join-Path (Get-Location) "out\pc\test_userdata\run_net"
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
Copy-Item -Recurse (Join-Path (Get-Location) "out\pc\test_userdata\seed_empty") $run
$env:REX_USER_DATA_ROOT = $run
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -Seconds $Seconds -InputScript $script -Tail 1 | Select-Object -First 1
foreach ($n in $set + @('REX_MASSEFFECT_RENDERER_NATIVE', 'REX_USER_DATA_ROOT')) { Remove-Item "env:$n" -ErrorAction SilentlyContinue }
$f = (Get-ChildItem out\pc\logs\peggle1_*.log | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
"LOG $f keys [$Keys]"
"DIFFERENCE/MISMATCH: " + (Select-String -CaseSensitive -Path $f -Pattern "DIFFERENCE|MISMATCH").Count + "; ring reports: " + (Select-String -CaseSensitive -Path $f -Pattern "\[native\] ring thread:").Count
"RakNet threads (routine 825FBDD8): " + (Select-String -CaseSensitive -Path $f -Pattern "\[thread\] created .* routine 825FBDD8").Count
Select-String -CaseSensitive -Path $f -Pattern "\[net\]|NetDll_getsockname STUB|\[dbg\]|Unhandled guest access violation|free 414B9AF0" | Select-Object -First 30 | ForEach-Object { $l = $_.Line; $l = $l.Substring([Math]::Min(27, $l.Length)); if ($l.Length -gt 250) { $l.Substring(0, 250) } else { $l } }
if (Get-Process peggle1 -ErrorAction SilentlyContinue) { "WARNING: a peggle1 process is still running" }
