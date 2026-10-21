# Runs the PC build on a modded game folder (tools/ps4_dlc/build_mod.py) and saves a few frames of it as PNGs.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\mod_test.ps1 [-Root out\ps4_dlc\game_root_mod]
#       [-Seconds 70] [-VideoAt 40] [-VideoFrames 300] [-Frames "0,100,200,299"] [-InputScript "..."] [-Keys "k=v,..."]
# The folder needs main.rsb; the other game files are copied from assets\game_root when missing. The run uses its own
# copy of a fresh save (out\pc\test_userdata\run_mod) and reads the folder directly (no asset pack).
param(
    [string]$Root = 'out\ps4_dlc\game_root_mod',
    [int]$Seconds = 70,
    [int]$VideoAt = 40,
    [int]$VideoFrames = 300,
    [string]$Frames = '0,100,200,299',
    [string]$InputScript = '900:A,1020:A,1200:A,1320:A,1500:A,1700:A,1900:A,2100:A,2400:A,2700:A,3000:A,3300:A',
    [string]$Keys = '',
    [string]$Tag = 'mod',
    # The save the run starts from (a folder of out\pc\test_userdata): seed_empty = a new profile, which goes straight
    # into the first level; seed_noach = a profile that has passed the introduction.
    [string]$Seed = 'seed_empty'
)
Set-Location (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$rootPath = (Resolve-Path $Root).Path
if (-not (Test-Path (Join-Path $rootPath 'main.rsb'))) { throw "no main.rsb in $rootPath" }
Get-ChildItem assets\game_root -File | Where-Object { $_.Name -ne 'main.rsb' } | ForEach-Object {
    $dst = Join-Path $rootPath $_.Name
    if (-not (Test-Path $dst)) { Copy-Item $_.FullName $dst }
}
$set = @()
foreach ($kv in ($Keys -split ',' | Where-Object { $_ })) {
    $k, $v = $kv -split '=', 2
    $name = 'REX_' + $k.ToUpper()
    Set-Item -Path "env:$name" -Value $v
    $set += $name
}
$env:REX_MASSEFFECT_RENDERER_NATIVE = 'true'
$env:REX_GAME_DATA_ROOT = $rootPath
$env:REX_ASSET_PACK = 'false'
$run = Join-Path (Get-Location) "out\pc\test_userdata\run_mod"
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
Copy-Item -Recurse (Join-Path (Get-Location) "out\pc\test_userdata\$Seed") $run
$env:REX_USER_DATA_ROOT = $run
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -Seconds $Seconds -InputScript $InputScript -VideoAt $VideoAt -VideoFrames $VideoFrames -Tail 1 | Select-Object -First 1
foreach ($n in $set + @('REX_MASSEFFECT_RENDERER_NATIVE', 'REX_USER_DATA_ROOT', 'REX_GAME_DATA_ROOT', 'REX_ASSET_PACK')) { Remove-Item "env:$n" -ErrorAction SilentlyContinue }
$f = (Get-ChildItem out\pc\logs\peggle1_*.log | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
"LOG $f"
"errors: " + (Select-String -CaseSensitive -Path $f -Pattern "\[error\]|Unhandled|DIFFERENCE|MISMATCH").Count + "; ring reports: " + (Select-String -CaseSensitive -Path $f -Pattern "\[native\] ring thread:").Count
Select-String -CaseSensitive -Path $f -Pattern "\[error\]|Unhandled|Failed to|not found|missing" | Select-Object -First 12 | ForEach-Object { $l = $_.Line; $l = $l.Substring([Math]::Min(27, $l.Length)); if ($l.Length -gt 230) { $l.Substring(0, 230) } else { $l } }
$raw = "out\pc\captures\video_${VideoAt}s.rgba"
if (Test-Path $raw) {
    python -P -E tools\ps4_dlc\frames.py $raw "out\ps4_dlc\shots" $Tag $Frames
} else { "no recording at $raw" }
if (Get-Process peggle1 -ErrorAction SilentlyContinue) { "WARNING: a peggle1 process is still running" }
