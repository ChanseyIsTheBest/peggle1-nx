# One lockstep recording of Peggle 1 on the PC: a fresh save, an input script timed in game frames, the frames from
# game frame -From on recorded at half size (peggle1_video_from_frame), muted and minimized (boot_test.ps1). Two runs
# with different renderers record the same game frames; tools/compare_raw.py compares them.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\p1_lockstep.ps1 -Tag xenos [-Renderer xenos|renut]
#       [-From 1900] [-Frames 1200]
#
# Writes out\pc\captures\lock_<Tag>.rgba (+ .txt, .frames). The raw recording is large (0.9 MB a frame): delete it
# once compared.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [ValidateSet('xenos', 'renut')][string]$Renderer = 'xenos',
    [int]$From = 1900,
    [int]$Frames = 1200,
    # The route: START at frame 1200 (the title), then A every 180 frames (3 s at 60 frames a second).
    [string]$InputScript = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
if (-not $InputScript) {
    $InputScript = (@('1200:START') + (8..60 | ForEach-Object { "$($_ * 180):A" })) -join ','
}
$userdata = Join-Path $root "out\pc\test_userdata\lock_$Tag"
if (Test-Path $userdata) { Remove-Item -Recurse -Force $userdata }
New-Item -ItemType Directory -Force $userdata | Out-Null
$env:REX_USER_DATA_ROOT = $userdata
if ($Renderer -eq 'renut') {
    $env:REX_PEGGLE1_RENDERER = 'renut'
    $env:REX_RENUT_GPU_BACKEND = 'vulkan'
} else {
    $env:REX_PEGGLE1_RENDERER = 'native'
}
try {
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -VideoFromFrame $From -VideoFrames $Frames -GameFps 60 -InputScript $InputScript -Tail 3 | Select-Object -First 3
} finally {
    foreach ($n in 'REX_USER_DATA_ROOT', 'REX_PEGGLE1_RENDERER', 'REX_RENUT_GPU_BACKEND') { Remove-Item "env:$n" -ErrorAction SilentlyContinue }
}
$cap = Join-Path $root 'out\pc\captures'
foreach ($ext in 'rgba', 'txt', 'frames') {
    $src = Join-Path $cap "video_f$From.$ext"
    if (Test-Path $src) { Move-Item -Force $src (Join-Path $cap "lock_$Tag.$ext") }
}
Get-ChildItem $cap -Filter "lock_$Tag.*" | ForEach-Object { '{0} {1:n0} bytes' -f $_.Name, $_.Length }
