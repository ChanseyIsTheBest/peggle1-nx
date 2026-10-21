# A timed run (fresh seed, the scripted route) with some keys, then the flips per 10 s (the waits of the flip wait).
#   powershell -NoProfile -ExecutionPolicy Bypass -File vsync_test.ps1 -Keys "peggle1_vsync_relaxed=false,diag_vdswap_delay_us=20000" [-Seconds 100]
param([string]$Keys = '', [int]$Seconds = 100)
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
$run = Join-Path (Get-Location) "out\pc\test_userdata\run_vsync"
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
Copy-Item -Recurse (Join-Path (Get-Location) "out\pc\test_userdata\seed_empty") $run
$env:REX_USER_DATA_ROOT = $run
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -Seconds $Seconds -InputScript $script -Tail 1 | Select-Object -First 1
foreach ($n in $set + @('REX_MASSEFFECT_RENDERER_NATIVE', 'REX_USER_DATA_ROOT')) { Remove-Item "env:$n" -ErrorAction SilentlyContinue }
$f = (Get-ChildItem out\pc\logs\peggle1_*.log | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
"LOG $f keys [$Keys]"
"DIFFERENCE/MISMATCH: " + (Select-String -CaseSensitive -Path $f -Pattern "DIFFERENCE|MISMATCH").Count
Select-String -CaseSensitive -Path $f -Pattern "vsync_relaxed:" | ForEach-Object { $l = $_.Line; $l.Substring([Math]::Min(48, $l.Length)) }
$waits = Select-String -CaseSensitive -Path $f -Pattern "WAIT_REG_MEM last 10 s" | ForEach-Object {
    $pk = if ($_.Line -match 'last 10 s: (\d+) packets') { [int]$Matches[1] } else { 0 }
    $w = if ($_.Line -match 'mem [0-9A-F]{8} 3 00000000 mask FFFFFFFF: (\d+) waits, ([\d.]+) ms') { "$($Matches[1])w/$([int][double]$Matches[2])ms" } else { '0w' }
    "$([int]($pk / 8))f:$w"
}
"per 10 s: frames (WAIT packets / 8) : flip waits / ms waited: " + ($waits -join ' ')
Select-String -CaseSensitive -Path $f -Pattern "\[native\] ring thread:" | Select-Object -Last 1 | ForEach-Object { $l = $_.Line; $l.Substring([Math]::Min(48, $l.Length)) }
Select-String -CaseSensitive -Path $f -Pattern "peggle1_vsync_relaxed[ :=]+\d+" -AllMatches | Select-Object -Last 1 | ForEach-Object { $_.Matches | ForEach-Object { $_.Value } }
if (Get-Process peggle1 -ErrorAction SilentlyContinue) { "WARNING: a peggle1 process is still running" }
