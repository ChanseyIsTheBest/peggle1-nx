# Runs the code generator on app\peggle1_manifest.toml (no post-codegen passes): the quick loop of stage 2.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\codegen_p1.ps1 [-Rexglue <exe>]
param([string]$Rexglue = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Rexglue) {
    $Rexglue = Get-ChildItem (Join-Path $root 'sdk\out'), 'D:\peggle 2\peggle2-nx\sdk\out' -Recurse -Filter 'rexglue*.exe' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^rexglue(rd|d)?\.exe$' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
New-Item -ItemType Directory -Force (Join-Path $root 'out') | Out-Null
$log = Join-Path $root 'out\codegen.log'
Push-Location (Join-Path $root 'app')
try {
    $env:REX_MAX_JUMP_TABLE_ENTRIES = '1024'
    $env:REXGLUE_DUMP_IMAGE = Join-Path $root 'out\default_image.bin'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $env:ComSpec /c "`"$Rexglue`" codegen peggle1_manifest.toml > `"$log`" 2>&1"
    $code = $LASTEXITCODE
} finally { Pop-Location }
Write-Host "== $Rexglue exit $code in $([int]$sw.Elapsed.TotalSeconds) s"
Get-Content $log -Tail 25
exit $code
