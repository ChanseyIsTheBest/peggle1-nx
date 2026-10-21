# A Peggle 1 PC test run: muted and minimized (boot_test.ps1), its own user data folder on D: (never the user's
# Documents), a shot every N seconds and a contact sheet of them.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\p1_run.ps1 -Name boot1 [-Seconds 60] [-ShotsEvery 3]
#       [-InputScript "20s:A,25s:A"] [-Keep] [-Env "REX_KEY=value;REX_OTHER=value"]
#
# User data: out\pc\test_userdata\<Name> (made fresh unless -Keep). Shots: out\renut_tests\<Name>\shots, the sheet
# out\renut_tests\<Name>\sheet.jpg; the run's log is copied there too.
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [int]$Seconds = 60,
    [int]$ShotsEvery = 3,
    [string]$InputScript = '',
    [switch]$Keep,
    # Extra environment for the run, "NAME=value;NAME=value" (a hashtable does not survive powershell -File).
    [string]$Env = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
$userdata = Join-Path $root "out\pc\test_userdata\$Name"
if (-not $Keep -and (Test-Path $userdata)) { Remove-Item -Recurse -Force $userdata }
New-Item -ItemType Directory -Force $userdata | Out-Null
$shots = Join-Path $root 'out\pc\shots'
if (Test-Path $shots) { Get-ChildItem $shots -Filter *.rgba | Remove-Item -Force }
$env:REX_USER_DATA_ROOT = $userdata
$env:REX_PEGGLE1_SHOTS_EVERY_S = "$ShotsEvery"
$set = @('REX_USER_DATA_ROOT', 'REX_PEGGLE1_SHOTS_EVERY_S')
foreach ($pair in ($Env -split ';' | Where-Object { $_ })) {
    $k, $v = $pair -split '=', 2
    Set-Item -Path "env:$k" -Value $v
    $set += $k
}
try {
    # An empty -InputScript would reach boot_test.ps1 as a missing argument: pass it only when set.
    $extra = @()
    if ($InputScript) { $extra = @('-InputScript', $InputScript) }
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -Seconds $Seconds -Tail 25 @extra
} finally {
    foreach ($n in $set) { Remove-Item "env:$n" -ErrorAction SilentlyContinue }
}
$dest = Join-Path $root "out\renut_tests\$Name"
New-Item -ItemType Directory -Force $dest | Out-Null
$log = Get-ChildItem out\pc\logs -Filter *.log -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
if ($log) { Copy-Item $log.FullName (Join-Path $dest 'run.log') -Force }
if ((Test-Path $shots) -and (Get-ChildItem $shots -Filter *.rgba)) {
    python -P tools\shots_to_png.py $shots --sheet (Join-Path $dest 'sheet.jpg') --scale 0.25
    # The PNGs go next to the sheet; the raw shots are removed (about 3.6 MB each at 720p).
    Get-ChildItem $shots -Filter *.png | Move-Item -Destination $dest -Force
    Get-ChildItem $shots -Filter *.rgba | Remove-Item -Force
    "sheet: $(Join-Path $dest 'sheet.jpg')"
} else {
    'no shots written'
}
