# Translates the game (default.xex) to C++ with the host code generator. It builds nothing: build the generator first
# (tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue").
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\codegen.ps1 [-Classic] [-NoPerf] [-PostPasses]
#
# Input:   assets\game_root\default.xex   (tools\stfs_extract.py puts it there)
#          app\peggle1_manifest.toml          (project, game paths, includes)
# Output:  app\generated\                 (C++ of the game and generated\rexglue.cmake; not committed)
#          app\generated\codegen_profile.txt  ("optimised" or "classic": the build logs it in its [enhancements] line)
#          out\codegen.log                (log of the generator)
#
# -Classic leaves out app\perf_overrides.toml (registers as C++ locals): a local diagnostic only; delivered NROs are
# always the optimised build (docs/peggle1-notes.md, "Enhancements"). Run the script again without it to go back.
# The post-codegen performance passes (tools/post_codegen.py: sa2-nx's pipeline) belong to the optimised build and run by default; -NoPerf (and -Classic) leave them out: local diagnostics.
param(
    [string]$Rexglue = '',
    [switch]$NoPerf,
    [switch]$PostPasses,
    [switch]$Classic
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$app = Join-Path $root 'app'
if (-not $Rexglue) {
    # The newest generator, whatever its configuration's suffix: the PC build links rexgluerd.exe (RelWithDebInfo) and
    # re-runs it by itself; a rexglue.exe left from an older Release build does not know the newer options
    # (2026-10-13: this script picked one of 2026-10-05 and wrote a tree without fp_mode_lookahead).
    $Rexglue = Get-ChildItem (Join-Path $root 'sdk\out') -Recurse -Filter 'rexglue*.exe' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^rexglue(rd|d)?\.exe$' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Rexglue -or -not (Test-Path $Rexglue)) { throw 'rexglue.exe not found; build it first (see the header)' }
$xex = Join-Path $root 'assets\game_root\default.xex'
if (-not (Test-Path $xex)) { throw "$xex not found; extract your package with tools\stfs_extract.py first" }

New-Item -ItemType Directory -Force (Join-Path $root 'out') | Out-Null
$log = Join-Path $root 'out\codegen.log'
Write-Host "== codegen with $Rexglue"
$manifest = 'peggle1_manifest.toml'
if ($Classic) {
    # The same manifest without perf_overrides.toml, next to it (relative paths stay valid), removed afterwards.
    $manifest = 'peggle1_manifest.classic.toml'
    $text = Get-Content (Join-Path $app 'peggle1_manifest.toml') -Raw
    $text = $text -replace '(?m)^includes = \[.*\]$', 'includes = ["overrides.toml"]'
    Set-Content -Encoding utf8 (Join-Path $app $manifest) $text
}
Push-Location $app
try {
    $env:REX_MAX_JUMP_TABLE_ENTRIES = '1024'
    # The loaded image (decrypted, decompressed), for the post-codegen steps that read the game's read-only data and
    # look for function addresses in it (tools/ro_constants.py, the entry modes of tools/flush_mode_flow.py).
    $env:REXGLUE_DUMP_IMAGE = Join-Path $root 'out\default_image.bin'
    & $env:ComSpec /c "`"$Rexglue`" codegen $manifest > `"$log`" 2>&1"
    $code = $LASTEXITCODE
} finally {
    Pop-Location
    if ($Classic) { Remove-Item (Join-Path $app $manifest) -ErrorAction SilentlyContinue }
}
Get-Content $log -Tail 30
if ($code -ne 0) { throw "codegen failed (exit $code), see out\codegen.log" }

# peggle1-nx (2026-10-21): the passes carry Peggle 2's lists (app/indirect_promotions.txt, src/cpu/hot/localized.txt)
# until Peggle 1 runs: off unless -PostPasses (tools/pointer_fixpoint.py runs this script after every pass).
$perf = $PostPasses -and -not ($NoPerf -or $Classic)
if ($perf) {
    # sa2-nx's pipeline (2026-10-13): the base passes, its three CPU rounds' steps, the localized copies, leaf locals.
    python (Join-Path $root 'tools\post_codegen.py')
    if ($LASTEXITCODE) { throw 'tools\post_codegen.py failed' }
}
if ($Classic) {
    # codegen.d names the temporary manifest, which is gone now: the build would take that as a changed input and
    # regenerate the optimised profile at once. Point it at the real manifest; the build still regenerates (and the
    # marker then says optimised) when one of the real inputs changes.
    $dep = Join-Path $app 'generated\default\codegen.d'
    if (Test-Path $dep) {
        (Get-Content $dep -Raw) -replace 'peggle1_manifest\.classic\.toml', 'peggle1_manifest.toml' |
            Set-Content -Encoding ascii -NoNewline $dep
    }
}
# The localized copies of the native replacements are made from the generated code (app/src/cpu/hot/localized.txt);
# tools\post_codegen.py makes them at the right place of its sequence, so only a build without it makes them here.
if (-not $perf -and $PostPasses) {
    python (Join-Path $root 'tools\localize_native.py') --list (Join-Path $app 'src\cpu\hot\localized.txt')
    if ($LASTEXITCODE) { throw 'tools\localize_native.py failed' }
}
$profile = if ($Classic) { 'classic' } else { 'optimised' }
if ($perf) { $profile += '+perf' }
Set-Content -Encoding ascii -NoNewline (Join-Path $app 'generated\codegen_profile.txt') $profile
Write-Host "done: $app\generated ($profile)"
