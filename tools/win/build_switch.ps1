# Builds the Nintendo Switch NRO: out\nx\peggle1.nro (devkitA64 GCC through tools/switch/cmake/switch-devkitA64.cmake,
# libnx, the static NVK driver from the mesa-switch Horizon SDK, the native renderer).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\build_switch.ps1 [-Jobs 8] [-Reconfigure]
#       [-Define "PEGGLE1_SWITCH_LTO=OFF;OTHER=VALUE"]
#
# -Define sets CMake cache options (it configures again). They stay in the build folder's cache until set back:
# a development build without the long LTO link is -Define "PEGGLE1_SWITCH_LTO=OFF", a release build
# -Define "PEGGLE1_SWITCH_LTO=ON".
#
# The NVK SDK is the portlibs/switch folder of the Mesa Horizon install (it must contain lib\libvulkan.a); override it
# with -NvkSdk or $env:PEGGLE1_NVK_SDK. CMake and Ninja come from the VS 2022 Build Tools (no MSVC environment: the
# compilers are devkitA64's). The generated code must exist (tools\codegen.ps1) and the host glslangValidator must be
# built (tools\win\build_glslang.ps1). The whole output goes to out\nx_build.log.
param(
    [string]$NvkSdk = '',
    [int]$Jobs = 8,
    [string]$BuildType = 'Release',
    [switch]$Reconfigure,
    [string]$BuildDir = 'out\nx',
    [string]$Log = 'out\nx_build.log',
    [string]$Define = '',
    # Profile-guided optimisation (2026-10-14, sa2-nx's setup): generate = the instrumented NRO that collects counts on
    # the console (app/src/peggle1_pgo.cpp); use = the optimised NRO from the counts tools\win\pgo_merge.ps1 merged;
    # none = neither. Without -Pgo the build keeps the setting it had.
    [ValidateSet('', 'none', 'generate', 'use')][string]$Pgo = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
$vs = if ($env:PEGGLE1_VSDIR) { $env:PEGGLE1_VSDIR } else { 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools' }
$cm = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:PATH = "$cm\CMake\bin;$cm\Ninja;$env:PATH"
# devkitPro's installer sets DEVKITPRO to the MSYS path /opt/devkitpro, which CMake on Windows cannot use: the Windows
# path is forced (override with $env:PEGGLE1_DEVKITPRO).
$env:DEVKITPRO = if ($env:PEGGLE1_DEVKITPRO) { $env:PEGGLE1_DEVKITPRO } else { 'C:/devkitPro' }
if (-not $NvkSdk) {
    $NvkSdk = if ($env:PEGGLE1_NVK_SDK) { $env:PEGGLE1_NVK_SDK } else {
        'C:/devkitPro/mesa-switch-main-nfsmw-source/mesa-switch-main/mesa-unified-install/opt/devkitpro/portlibs/switch' }
}
if (-not (Test-Path (Join-Path $NvkSdk 'lib\libvulkan.a'))) { throw "no lib\libvulkan.a under $NvkSdk" }
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
New-Item -ItemType Directory -Force (Split-Path $Log) | Out-Null
$ErrorActionPreference = 'Continue'
# A fresh log for every run (the build step appends to what the configure step wrote).
Set-Content -Path $Log -Value "== build_switch $(Get-Date -Format s)" -Encoding ascii
$defines = ($Define -split ';' | Where-Object { $_ } | ForEach-Object { "-D$_" }) -join ' '
if ($Pgo) { $defines = ("$defines -DPEGGLE1_PGO=" + $(if ($Pgo -eq 'none') { '' } else { $Pgo })).Trim() }
# GCC's lto-wrapper compiles the LTRANS partitions in parallel only through GNU make: without a make on PATH it runs
# all of them one after another (sa2-nx: about 17 minutes). env.ps1 keeps MSYS2's usr\bin off PATH (its own cmd, link
# and find), so make.exe and the three DLLs it needs get a folder of their own. (sa2-nx's build script.)
$make = Join-Path $root 'out\tools\make'
if (-not (Test-Path (Join-Path $make 'make.exe'))) {
    New-Item -ItemType Directory -Force $make | Out-Null
    foreach ($f in 'make.exe', 'msys-2.0.dll', 'msys-iconv-2.dll', 'msys-intl-8.dll') {
        Copy-Item -Force (Join-Path $env:DEVKITPRO "msys2\usr\bin\$f") $make
    }
}
$env:PATH = "$make;$env:PATH"
if ($Reconfigure -or $defines -or -not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    $configure = "cmake -S app -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$BuildType " +
        "`"-DCMAKE_TOOLCHAIN_FILE=$root\tools\switch\cmake\switch-devkitA64.cmake`" " +
        "`"-DREXSDK_DIR=$root\sdk`" `"-DREXGLUE_SWITCH_NVK_SDK=$NvkSdk`" -DPEGGLE1_NATIVE_RENDERER=OFF -DPEGGLE1_RENUT_RENDERER=ON -DPEGGLE1_RENUT_PORT_SHADERS=OFF `"-DCMAKE_MAKE_PROGRAM=$cm\Ninja\ninja.exe`" " +
        "`"-DPython3_EXECUTABLE=$python`" `"-DPYTHON_EXECUTABLE=$python`" $defines"
    & $env:ComSpec /c "$configure >> `"$Log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content $Log -Tail 30; "exit=$LASTEXITCODE"; exit $LASTEXITCODE }
}
& $env:ComSpec /c "cmake --build `"$BuildDir`" -j $Jobs >> `"$Log`" 2>&1"
$code = $LASTEXITCODE
Get-Content $Log -Tail 25
"exit=$code"
exit $code
