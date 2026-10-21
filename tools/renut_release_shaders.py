#!/usr/bin/env python3
"""peggle2-nx (2026-10-20): the Switch build's Renut shader files without translated game shaders, for a build that can
be distributed (out/renut_switch_release/), and the file the web installer makes in their place, for tests.

    python tools/renut_release_shaders.py [--glsl out/deko3d_shaders/glsl_opt] [--cache out/renut_switch/shader_cache_switch.cpp]

Writes into out/renut_switch_release/:
  deko3d_glsl.bin, deko3d_glsl_switch.cpp   the GLSL package built into the NRO: only the port's own shaders
                                            (tools/renut_port_shaders.py: the Bink movie trio the game compiles at boot)
  shader_cache_switch.cpp                   the shader table (numbers: hashes, fetch masks, vertex input formats) without
                                            the SPIR-V (the deko3d back end never reads it; the PC's Vulkan one does)
  sd/deko3d_glsl.bin                        the package of the shaders stored in default.xex, which the web installer
                                            makes from the player's file and puts beside the NRO (this one is the Python
                                            tools' copy: game data, for tests only, never distributed)
Point the Switch build at the first three with -Define "BANJO_DEKO3D_GLSL_CPP=...;BANJO_RENUT_SHADER_CACHE_CPP=..."
(app/cmake/peggle1_renut_gpu.cmake).

peggle1-nx (2026-10-21): Peggle 1 has no port shaders (its PopCap logo is a WMV the game decodes on the CPU;
PEGGLE1_RENUT_PORT_SHADERS is off), so nothing is built in: no deko3d_glsl.bin / deko3d_glsl_switch.cpp at the top,
all 82 stored shaders go into sd/deko3d_glsl.bin, and the release build takes -Define "BANJO_DEKO3D_GLSL_CPP=none;
BANJO_RENUT_SHADER_CACHE_CPP=<out>/shader_cache_switch.cpp" (no built-in package: the NRO reads the installer's file).
The GLSL is the native chain without early alpha (out/deko3d_release/glsl_opt: XenosRecomp without
BANJO_HLSL_OVERRIDE), which the installer can make.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import renut_port_shaders  # noqa: E402


def package(glsl_dir, names, out_dir):
    """tools/deko3d_shader_package.py over the given shaders only."""
    os.makedirs(out_dir, exist_ok=True)
    src = os.path.join(out_dir, 'glsl')
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(src)
    for n in names:
        shutil.copyfile(os.path.join(glsl_dir, n), os.path.join(src, n))
    subprocess.run([sys.executable, '-P', os.path.join(ROOT, 'tools', 'deko3d_shader_package.py'), '--glsl', src,
                    '--out', out_dir], check=True)
    shutil.rmtree(src)


def strip_spirv(text):
    """shader_cache_switch.cpp without the SPIR-V: the assembler's .incbin goes, its sizes are 0, and every entry's
    SPIR-V offset and size (the 4th and 5th numbers of g_shaderCacheEntries' lines) are 0."""
    text, n = re.subn(r'// banjo-nx: the compressed SPIR-V.*?\.previous\\n"\);\n',
                      'const uint8_t g_compressedSpirvCache[] = {0};  // peggle2-nx: no SPIR-V (deko3d reads none)\n',
                      text, flags=re.S)
    if n != 1:
        sys.exit('renut_release_shaders: the SPIR-V block of shader_cache_switch.cpp not found')
    text = re.sub(r'(g_spirvCache(?:Compressed|Decompressed)Size\s*=\s*)\d+', r'\g<1>0', text)

    def entry(m):
        fields = m.group(2).split(', ')
        fields[3] = fields[4] = '0'
        return m.group(1) + ', '.join(fields)
    text = re.sub(r'^(\t\{ )(0x[0-9A-F]+, \d+, \d+, \d+, \d+)', entry, text, flags=re.M)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--glsl', default=os.path.join(ROOT, 'out', 'deko3d_release', 'glsl_opt'))
    ap.add_argument('--cache', default=os.path.join(ROOT, 'out', 'renut_switch', 'shader_cache_switch.cpp'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'renut_switch_release'))
    a = ap.parse_args()
    port = set()  # peggle1-nx: no port shaders (see above)
    all_names = sorted(f for f in os.listdir(a.glsl) if re.fullmatch(r'(vs|ps)_[0-9A-F]{16}\.glsl', f))
    missing = port - set(all_names)
    if missing:
        sys.exit(f'renut_release_shaders: {sorted(missing)} not in {a.glsl}')
    stored = [n for n in all_names if n not in port]
    if port:
        package(a.glsl, sorted(port), a.out)
    package(a.glsl, stored, os.path.join(a.out, 'sd'))
    os.remove(os.path.join(a.out, 'sd', 'deko3d_glsl_switch.cpp'))  # the installer's file is never built in
    text = open(a.cache, encoding='utf-8').read()
    with open(os.path.join(a.out, 'shader_cache_switch.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(strip_spirv(text))
    print(f'renut_release_shaders: built in {len(port)} port shaders, beside the NRO {len(stored)} stored ones '
          f'({", ".join(stored)}) -> {a.out}')


if __name__ == '__main__':
    main()
