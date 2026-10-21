#!/bin/bash
# The release shader library's recipe on the PC (2026-10-14): what peggle2-nx-installer's lib/shaders.js
# (PEGGLE1_SHADER_RECIPE) makes from default.xex alone, with this repository's tools:
#   1. the containers stored in the image          tools/extract_stored_shaders.py  (3: vs_6b6d, ps_a17c, ps_558c)
#   2. vs_6b6d as Direct3D rewrites it for its      tools/make_vertex_variants.py + the DECL repair (1: from the ring's
#      vertex declaration                           microcode in $DISCOVERY; the installer runs Direct3D's patcher)
#   3. Direct3D's own shaders, wrapped              tools/wrap_discovered.py --immediate-only, with the stored vs_6b6d
#                                                   and ps_558c as templates (4)
#   4. tools/build_shader_library.sh on those 8 into $OUT.
# Not in it: the three shaders the game compiles at run time (D3DXCompileShader: vs_3caebedd6edf56e6,
# ps_23fb686cb21303f8, ps_b79e9877de15d6db) and Direct3D's rewrite of one of them (vs_3df8254d4b8024c4); the NRO
# draws those with its own shaders.
#
# The containers and the HLSL are the installer's byte for byte. The SPIR-V is not always: build_shader_library.sh
# uses the native DXC 1.8.2407, the installer the WebAssembly DXC v2025.1, and the release library is the installer's
# (peggle2-nx-installer: node test/library_node.mjs <Xbox 360 game folder> --out <folder>). Test the NRO with that one.
#
#   tools/build_shader_library_static.sh
# Environment (defaults): IMAGE (out/default_image.bin), DISCOVERY (out/pc/shaders_discovery), WORK
# (out/shaders_static_work), OUT (out/shaders_static), PYTHON (python), REPAIR (the DECL repair tool; built into
# $WORK/bin when missing), CXX and DXC as for build_shader_library.sh.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${IMAGE:-$ROOT/out/default_image.bin}
DISCOVERY=${DISCOVERY:-$ROOT/out/pc/shaders_discovery}
WORK=${WORK:-$ROOT/out/shaders_static_work}
OUT=${OUT:-$ROOT/out/shaders_static}
CXX=${CXX:-/c/devkitPro/msys2/clang64/bin/clang++.exe}
REPAIR=${REPAIR:-$WORK/bin/repair_vertex_variant_declarations.exe}
PY=("${PYTHON:-python}" -P)

if [ ! -x "$REPAIR" ]; then
  echo "== building the DECL repair tool"
  mkdir -p "$(dirname "$REPAIR")"
  "$CXX" -std=c++20 -O2 -static "$ROOT/shaders/tools/repair_vertex_variant_declarations.cpp" -o "$REPAIR"
fi
echo "== 1. stored containers"
"${PY[@]}" "$ROOT/tools/extract_stored_shaders.py" "$IMAGE" --out "$WORK/stored"
echo "== 2. vertex variants"
"${PY[@]}" "$ROOT/tools/make_vertex_variants.py" --raw "$DISCOVERY" --containers "$WORK/stored" --out "$WORK/variants" \
  --repair "$REPAIR"
echo "== 3. Direct3D's own shaders"
"${PY[@]}" "$ROOT/tools/wrap_discovered.py" --raw "$DISCOVERY" --containers "$WORK/stored" "$WORK/variants/repaired" \
  --out "$WORK/wrapped" --immediate-only --template-vs "$WORK/stored/vs_6b6d1eda0e6e465b.bin" \
  --template-ps "$WORK/stored/ps_558cf34ef8661cae.bin"
n=$(ls "$WORK/stored" "$WORK/variants/repaired" "$WORK/wrapped" | grep -c '\.bin$')
[ "$n" -eq 8 ] || { echo "expected 8 containers, have $n" >&2; exit 1; }
echo "== 4. library"
OUT="$OUT" "$ROOT/tools/build_shader_library.sh" "$WORK/stored" "$WORK/variants/repaired" "$WORK/wrapped"
