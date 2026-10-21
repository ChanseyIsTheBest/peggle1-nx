#!/bin/bash
# Compiles the port's own shaders (shaders/port/*.hlsl) to SPIR-V and writes them into
# app/src/me/peggle2_port_shaders_spirv.inc, which app/src/me/peggle2_port_shaders.cpp includes. The .inc is checked in
# (the app build has no DXC); run this after changing the HLSL or shaders/XenosRecomp/shader_common.h.
#   peggle2_movie_vs.hlsl              -> kPeggle2MovieVs        (for the game's vs_3caebedd6edf56e6)
#   peggle2_movie_ps.hlsl              -> kPeggle2MoviePs        (ps_23fb686cb21303f8)
#   peggle2_movie_ps.hlsl, with alpha  -> kPeggle2MoviePsAlpha   (ps_b79e9877de15d6db)
# Same DXC options as the shader library (tools/build_shader_library.sh). Environment: DXC (default: the one
# build_shader_library.sh uses), SPIRV_VAL (validates when set or found in MSYS2 clang64), INC (output), XENOS (the
# folder with shader_common.h), PYTHON.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DXC=${DXC:-/c/devkitPro/UnleashedRecomp-NX/tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe}
XENOS=${XENOS:-$ROOT/shaders/XenosRecomp}
INC=${INC:-$ROOT/app/src/me/peggle2_port_shaders_spirv.inc}
SPIRV_VAL=${SPIRV_VAL:-$(command -v spirv-val || ls /c/devkitPro/msys2/clang64/bin/spirv-val.exe 2>/dev/null || true)}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FLAGS=(-spirv -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -Werror=parameter-usage -I "$XENOS")

"$DXC" "${FLAGS[@]}" -T vs_6_6 -fvk-invert-y -Fo "$TMP/vs.spv" "$HERE/peggle2_movie_vs.hlsl"
"$DXC" "${FLAGS[@]}" -T ps_6_6 -Fo "$TMP/ps.spv" "$HERE/peggle2_movie_ps.hlsl"
"$DXC" "${FLAGS[@]}" -T ps_6_6 -D PEGGLE2_MOVIE_ALPHA -Fo "$TMP/ps_alpha.spv" "$HERE/peggle2_movie_ps.hlsl"
if [ -n "$SPIRV_VAL" ]; then
  for m in vs ps ps_alpha; do "$SPIRV_VAL" --target-env vulkan1.2 --scalar-block-layout "$TMP/$m.spv"; done
  echo "spirv-val: valid"
fi
"${PYTHON:-python}" -P "$HERE/spv_to_inc.py" "$INC" kPeggle2MovieVs="$TMP/vs.spv" kPeggle2MoviePs="$TMP/ps.spv" \
  kPeggle2MoviePsAlpha="$TMP/ps_alpha.spv"
echo "written $INC"
