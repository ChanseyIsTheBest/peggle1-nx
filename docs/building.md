# Building peggle1-nx

Everything below runs on Windows. You need your own copy of the Xbox Live Arcade game, its Title Update #3 and, if you
want it, Peggle Nights; nothing made from them is committed (`assets/`, `app/generated/` and `out/` are git-ignored).
Players use the installer page instead (https://chanseyisthebest.github.io/peggle1-nx-installer/): it makes the same
files in the browser.

## Tools

- VS 2022 Build Tools (MSVC libraries + Windows SDK), LLVM clang 20 (portable is fine; by default `..\_tools\llvm`),
  Python 3 with `xxhash`, Git. CMake and Ninja come from the VS Build Tools. Paths can be overridden, see
  `tools/win/env.ps1`. Configure and build through `tools\win\run.ps1` (it loads that environment): from a plain
  Git Bash a re-configure finds the wrong compiler and Ninja.
- For the Switch: devkitPro (devkitA64, libnx, deko3d), and uam and SPIRV-Cross built for the PC
  (`bash tools/deko3d_build_tools.sh`, into `out/deko3d_tools`). No Vulkan driver is needed: the NRO draws with deko3d.

## 1. The game files

```powershell
# The game (verifies the STFS hash tables, writes nothing to the package)
python tools\stfs_extract.py "<...>\58410889\000D0000\87D0A5D366F24C8FF8BEE120E5F72D78F100E189" assets\game_root
# Title Update #3 into the same folder: default.xexp (the runtime applies it at start) and nightstu.pak
python tools\stfs_extract.py "<...>\TU_1C42249_0000108000000.0000000000102" assets\game_root
# Peggle Nights stays a package: the NRO installs it from dlc\ beside itself at its first start
```

## 2. The code generator and the PC build

```powershell
# Third-party sources of the SDK (only the files this fork changed are kept in sdk/thirdparty)
python tools\fetch_thirdparty.py

# The code generator (-march=x86-64-v2 is required: the SDK uses SSSE3 intrinsics)
powershell -File tools\win\run.ps1 out\host_configure.log "cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2"
powershell -File tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue"

# Translate the game to C++ with the optimised code generation and its post steps (tools/post_codegen.py; it writes
# the decrypted, patched executable to out\default_image.bin, which step 3 reads)
powershell -File tools\codegen.ps1 -PostPasses

# The PC build (Renut's renderer on Vulkan or the emulated GPU; for tests)
powershell -File tools\win\run.ps1 out\pc_configure.log "cmake --preset win-pc -S app -DPEGGLE1_RENUT_RENDERER=ON -DPEGGLE1_RENUT_PORT_SHADERS=OFF -DPEGGLE1_PERF_POST=ON"
powershell -File tools\win\run.ps1 out\pc_build.log "cmake --build out/pc"

# A run of the PC build (muted and minimized by default), its log and shots in out\renut_tests\<name>
powershell -File tools\win\p1_run.ps1 -Name boot1 -Seconds 60
```

Test runs drive a scripted pad (`-InputScript "20s:START,24s:A"`, see `app/src/peggle1_input_script.h`; a second
player with `REX_PEGGLE1_INPUT_SCRIPT_P2`), and `tools\win\p1_run.ps1 -Env "REX_PEGGLE1_CPU_VERIFY=true"` checks every
call of the native functions against the recompiled code.

## 3. The shaders

The 82 shader containers the patched executable stores, translated as the installer page does it (without the early
alpha copies, which the page does not make):

```powershell
python tools\renut_shader_dump.py --image out\default_image.bin --out out\renut_shader_dump
$env:BANJO_DEKO3D_SPIRV_DIR = "$PWD\out\deko3d_release\spirv"
out\pc\xenos_recomp\build\XenosRecomp\XenosRecomp.exe out\renut_shader_dump out\deko3d_release\scratch_cache.cpp app\thirdparty\reblue-XenosRecomp\XenosRecomp\shader_common.h
Remove-Item env:BANJO_DEKO3D_SPIRV_DIR
python tools\deko3d_shaders.py --spirv out\deko3d_release\spirv --out out\deko3d_release
python tools\deko3d_glsl_opt.py --in out\deko3d_release\glsl --out out\deko3d_release\glsl_opt
python tools\renut_switch_shader_cache.py --pc out\pc
python tools\renut_release_shaders.py
```

`out\renut_switch_release\` then holds the shader table for a release NRO (no translated shaders in it) and, in `sd\`,
`deko3d_glsl.bin`: the package that goes beside the NRO (the installer page makes the same shaders from your copy).
Development NROs carry every translated shader inside (`tools\deko3d_shader_package.py --out out\renut_switch`, with
the early alpha copies of the PC build's `out\pc\renut_gpu\early_alpha_hlsl` as `BANJO_HLSL_OVERRIDE`).

## 4. The NRO

```powershell
# The release: no game shader inside (deko3d_glsl.bin beside it), no LTO, no PGO
powershell -File tools\win\build_switch.ps1 -Jobs 12 -BuildDir out\nx_release -Log out\nx_release_build.log -Pgo none -Define "PEGGLE1_RENUT_RELEASE_SHADERS=ON"
# A development build: every translated shader inside
powershell -File tools\win\build_switch.ps1 -Jobs 12 -BuildDir out\nx_dev -Log out\nx_dev_build.log
```

The Switch build's settings are compiled in: `app/peggle1_switch_defaults.toml` (Renut on deko3d, no log, guest
sign-in for local multiplayer, 720p) is applied before the SD card's `peggle1.toml`, and `app/peggle1_switch.toml`
(language, controllers) is written as `peggle1.toml` at the first start when there is none. A test run turns the log
on in its `peggle1.toml` (`log_level = "info"`, `log_to_file = true`).

## 5. The SD folder

```powershell
python tools\build_asset_pack.py assets\game_root --out-dir out\pack --verify
```

`sdmc:/switch/peggle1-nx/` takes `peggle1.nro`, `game_root.nxpack` + `game_root.nxidx` from `out\pack`,
`deko3d_glsl.bin` from `out\renut_switch_release\sd` (release NRO only) and `dlc\` with the Peggle Nights package.

In Ryujinx, `tools/win/p1_emu_run.sh <name> <seconds> [key = value ...]` runs the NRO from `out/emu_sd/peggle1-nx`
(see the script for the data folder it expects) and collects its log and shots.
