# Assembles the SD card folder for a Switch test: out\dist\switch\peggle2-nx\ with
#   peggle1.nro                    from out\nx (tools\win\build_switch.ps1)
#   peggle1.toml                   app\switch\peggle1.toml (only when the folder has none yet: it is yours to edit)
#   peggle1_shaders.mesp (+ .idx)  the shader library made from your own game files (out\shaders)
#   README.txt                     how to install and launch
#   configs\*.toml                 the premade configs (tools\make_enhancement_configs.py): copy one over peggle1.toml
# The game files are yours (assets\game_root: default.xex, main.rsb, ...) and stay out of the repository:
#   -WithGameData              adds them as the asset cache, game_root.nxpack + game_root.nxidx (one file the console
#                              keeps open instead of the folder; built here with tools\build_asset_pack.py when
#                              missing or older than the folder);
#   -WithGameData -LooseGame   copies the game_root folder instead: the console builds the pack on its first boot.
# Copy the folder to sdmc:/switch/. out\dist is git-ignored: the shader library and the pack are made from the game.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\package_switch.ps1 [-WithGameData [-LooseGame]]
#
# The one game data pack with the PS4 content (out\ps4_dlc\...; docs\peggle1-notes.md) and the HD pack go in with
#   -Pack <file.nxpack>        a pack built with tools\build_asset_pack.py (its .nxidx beside it): it becomes the
#                              folder's game_root.nxpack + game_root.nxidx (moved with -MovePack, else copied);
#   -HdPack <peggle1_hd.pack>  copied beside the NRO, with its list (peggle1_hd.txt) when there is one.
# Without them the folder keeps the pack and the HD pack it has.
param([switch]$WithGameData, [switch]$LooseGame, [string]$Pack = '', [switch]$MovePack, [string]$HdPack = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dist = Join-Path $root 'out\dist\switch\peggle2-nx'
New-Item -ItemType Directory -Force $dist | Out-Null
$nro = Join-Path $root 'out\nx\peggle1.nro'
if (-not (Test-Path $nro)) { throw "$nro not found: build it with tools\win\build_switch.ps1" }
Copy-Item $nro $dist -Force
# Since 1.0.0 the NRO carries its settings (app/peggle1_switch_defaults.toml) and writes the player's short
# peggle1.toml at its first start: the folder ships none. One from an older package is set aside, not used.
$oldToml = Join-Path $dist 'peggle1.toml'
if (Test-Path $oldToml) { Move-Item -Force $oldToml (Join-Path $dist 'peggle1.toml.before_1.0.0') }
# The premade configs (tools\make_enhancement_configs.py): copy one over peggle1.toml to use it.
New-Item -ItemType Directory -Force (Join-Path $dist 'configs') | Out-Null
# Old configs go first (sa2-nx's lesson: a stale test config with outdated keys stayed in the SD folder).
Get-ChildItem (Join-Path $dist 'configs') -Filter *.toml -ErrorAction SilentlyContinue | Remove-Item -Force
Copy-Item -Force (Join-Path $root 'app\configs\*.toml') (Join-Path $dist 'configs')
# The console's CPU sampler follows the toml key switch_profile_stacks (on in every config but the verify one). The
# flag file logs/rex/stacks_profile.flag would force it on even in the verify config, so the package carries none and
# removes one left by an earlier package.
Remove-Item -Force (Join-Path $dist 'logs\rex\stacks_profile.flag') -ErrorAction SilentlyContinue
foreach ($f in 'peggle1_shaders.mesp', 'peggle1_shaders.mesp.idx') {
    $src = Join-Path $root "out\shaders\$f"
    if (Test-Path $src) { Copy-Item $src $dist -Force } else { "warning: $src missing (tools/build_shader_library.sh)" }
}
if ($WithGameData) {
    $game = Join-Path $root 'assets\game_root'
    $dst = Join-Path $dist 'game_root'
    if ($LooseGame) {
        New-Item -ItemType Directory -Force $dst | Out-Null
        Get-ChildItem $game -File | Copy-Item -Destination $dst -Force
        foreach ($f in 'game_root.nxpack', 'game_root.nxidx') {
            Remove-Item -Force (Join-Path $dist $f) -ErrorAction SilentlyContinue
        }
    } else {
        & python (Join-Path $root 'tools\build_asset_pack.py') $game --if-stale
        if ($LASTEXITCODE) { throw 'asset pack build failed' }
        foreach ($f in 'game_root.nxpack', 'game_root.nxidx') {
            Copy-Item -Force (Join-Path $root "assets\$f") (Join-Path $dist $f)
        }
        # A loose folder from an earlier package is no longer read (the pack wins): drop the staging copy.
        if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
    }
}
if ($Pack) {
    $packPath = (Resolve-Path $Pack).Path
    $idxPath = [IO.Path]::ChangeExtension($packPath, '.nxidx')
    if (-not (Test-Path $idxPath)) { throw "no index beside the pack: $idxPath" }
    foreach ($pair in @(@($packPath, 'game_root.nxpack'), @($idxPath, 'game_root.nxidx'))) {
        $to = Join-Path $dist $pair[1]
        if ($MovePack) { Move-Item -Force $pair[0] $to } else { Copy-Item -Force $pair[0] $to }
    }
}
if ($HdPack) {
    $hdPath = (Resolve-Path $HdPack).Path
    Copy-Item -Force $hdPath (Join-Path $dist 'peggle1_hd.pack')
    $hdList = Join-Path (Split-Path $hdPath) 'peggle1_hd.txt'
    if (Test-Path $hdList) { Copy-Item -Force $hdList (Join-Path $dist 'peggle1_hd.txt') }
}
function Describe($name) {
    $p = Join-Path $dist $name
    if (Test-Path $p) { $i = Get-Item $p; '{0:N0} bytes, {1:yyyy-MM-dd HH:mm}' -f $i.Length, $i.LastWriteTime } else { 'not in this folder' }
}
@"
peggle2-nx for the Nintendo Switch
==================================

Everything is in this one folder. Copy its files over the ones in sdmc:/switch/peggle2-nx/ (do not delete the folder
on the card first: your logs and settings are in it):

  peggle1.nro                the program                         $(Describe 'peggle1.nro')
  game_root.nxpack           THE game data pack (one, complete)  $(Describe 'game_root.nxpack')
  game_root.nxidx            its index (they belong together)    $(Describe 'game_root.nxidx')
  peggle1_hd.pack            optional: the 1080p art             $(Describe 'peggle1_hd.pack')
  peggle1_shaders.mesp       the shader library (+ .idx)
  (peggle1.toml)             not in this folder: the game writes your short settings file at its first start
  configs\                   test settings (logs, profiler): copy one over peggle1.toml for a fault report

There is one game data pack. Older test folders beside this one (peggle2-nx-dlc, -dlc-music, -dlc-full, -dlc-full-mp,
-hd) are superseded by it: use nothing from them. The program says which pack it mounted in the log, in the line
"[pack] mounted ... files, ... directories, ... MB": with this pack it reads about $(if (Test-Path (Join-Path $dist 'game_root.nxpack')) { '{0:N0}' -f ((Get-Item (Join-Path $dist 'game_root.nxpack')).Length / 1MB) } else { '?' }) MB.

The pack is made from your own game files: default.xex and main.rsb of your Xbox 360 game (python
tools\build_asset_pack.py), main.rsb rebuilt with the content of your PS4 game where the notes say so
(tools\ps4_dlc\build_mod.py; docs\peggle1-notes.md).

Launch it with a title takeover (hold R while starting a game, then open the Homebrew Menu) or a forwarder: from the
Album it does not get enough memory.

Settings: the NRO carries all of them (every optimisation on, all PS4 content, the 1080p art always, no log). At its
first start it writes sdmc:/switch/peggle2-nx/peggle1.toml with only your options: frame rate (30 by default),
language and the controllers. If you still have a peggle1.toml from an older test build there, delete it (or keep it
as a copy elsewhere) so the game writes the new one.

For a fault report: copy configs\peggle1_test_dlc_log.toml (or another test config) over peggle1.toml; it turns the
log and the profiler on, and logs go to sdmc:/switch/peggle2-nx/logs/.

A PGO build (the instrumented NRO for profile collection) writes sdmc:/switch/peggle2-nx/pgo/peggle1_pgo_<N>.stream
every 2 minutes and when you press HOME: play normally, then copy that pgo folder back to the PC.
"@ | Set-Content -Encoding ascii (Join-Path $dist 'README.txt')
Get-ChildItem $dist | Format-Table Name, Length
