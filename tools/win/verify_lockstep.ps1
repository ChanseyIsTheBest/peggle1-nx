# Verify-mode run (150 s) then a lockstep capture (game frames 1000-1399) compared with the CPU-off baselines.
#   powershell -NoProfile -ExecutionPolicy Bypass -File verify_lockstep.ps1 -Name nonarg [-SkipVerify] [-SkipLockstep]
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [switch]$SkipVerify,
    [switch]$SkipLockstep,
    [string]$Seed = 'seed_noach',
    [string]$Base1 = 'ls_off1',
    [string]$Base2 = 'ls_off2',
    [switch]$CpuOff,
    [switch]$NoCompare
)
Set-Location (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script = "900:A,1020:A,1200:A,1320:A,1500:A,1700:A,1900:A,2100:A,2400:A,2700:A,3000:A,3300:A"
$env:REX_MASSEFFECT_RENDERER_NATIVE = 'true'
if ($CpuOff) { $env:REX_PEGGLE1_CPU_ENHANCEMENTS = 'false' }
# Test runs keep their saves inside the project (user_data_root), each run starting from the same seed: the user's
# Documents\peggle1 save is never touched, and progress one run saves cannot change the next run's menus.
$userdata = Join-Path (Get-Location) 'out\pc\test_userdata'
$seedDir = Join-Path $userdata $Seed
if (-not (Test-Path $seedDir)) { throw "seed $seedDir not found" }
function Use-Seed([string]$tag) {
    $run = Join-Path $userdata ('run_' + $tag)
    if (Test-Path $run) { Remove-Item -Recurse -Force $run }
    Copy-Item -Recurse $seedDir $run
    $env:REX_USER_DATA_ROOT = $run
}
if (-not $SkipVerify) {
    Use-Seed 'verify'
    $env:REX_PEGGLE1_CPU_VERIFY = 'true'
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -Seconds 150 -InputScript $script -Tail 1 | Select-Object -First 1
    $env:REX_PEGGLE1_CPU_VERIFY = $null
    $f = (Get-ChildItem out\pc\logs\peggle1_*.log | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
    "VERIFY LOG $f"
    "DIFF/MISMATCH lines: " + (Select-String -CaseSensitive -Path $f -Pattern "DIFFERENCE|MISMATCH").Count
    Select-String -CaseSensitive -Path $f -Pattern "\[hot\] 10 s" | Select-Object -Last 1 | ForEach-Object { $l = $_.Line; $l.Substring($l.IndexOf('| checks')) }
    Select-String -CaseSensitive -Path $f -Pattern "\[verify\]" | Select-Object -Last 1 | ForEach-Object { $_.Line }
    Select-String -CaseSensitive -Path $f -Pattern "\[enhancements\] masters" | ForEach-Object { $_.Line }
}
if (-not $SkipLockstep) {
    Use-Seed 'lockstep'
    # 2026-10-20: peggle1_fps is 60 by default now; the baselines were recorded at 30.
    $env:REX_PEGGLE1_FPS = '30'
    $env:REX_VBLANK_PER_SWAP = 'true'; $env:REX_VBLANK_PER_SWAP_COUNT = '2'; $env:REX_CLOCK_PER_SWAP_HZ = '30'
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -Seconds 150 -InputScript $script -VideoFromFrame 1000 -VideoFrames 400 -Tail 1 | Select-Object -First 1
    # Disk space (the user's request, relayed 2026-10-13): only the run's frame hashes are kept under its name; the raw
    # recording stays video_f1000.rgba and the next run overwrites it.
    python tools\compare_hashes.py --hash out\pc\captures\video_f1000.rgba
    foreach ($e in 'hash', 'frames', 'txt') { Copy-Item -Force "out\pc\captures\video_f1000.$e" "out\pc\captures\video_f1000_ls_$Name.$e" }
    $env:REX_VBLANK_PER_SWAP = $null; $env:REX_VBLANK_PER_SWAP_COUNT = $null; $env:REX_CLOCK_PER_SWAP_HZ = $null
    $env:REX_PEGGLE1_FPS = $null
    $f = (Get-ChildItem out\pc\logs\peggle1_*.log | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
    "LOCKSTEP LOG $f"
    $c = 'out\pc\captures\video_f1000_'
    if (-not $NoCompare) { python tools\compare_hashes.py "${c}ls_$Name.hash" "${c}$Base1.hash" "${c}$Base2.hash" }
}
$env:REX_MASSEFFECT_RENDERER_NATIVE = $null
$env:REX_USER_DATA_ROOT = $null
$env:REX_PEGGLE1_CPU_ENHANCEMENTS = $null
if (Get-Process peggle1 -ErrorAction SilentlyContinue) { "WARNING: a peggle1 process is still running" }
