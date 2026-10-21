#!/usr/bin/env python3
"""peggle2-nx (2026-10-20): writes Renut's port-written shaders where the translator takes a shader's HLSL from.

Peggle 2 compiles three of its six shaders at boot (D3DXCompileShader: the Bink movie vertex shader and the two movie
pixel shaders), so no game file holds them and no tool can translate them from the player's files (the web installer
included). shaders/port/renut/ has port-written versions of them (no game shader code). This tool puts in front of each
body what Renut's translator (thirdparty/reblue-XenosRecomp) puts in front of every shader it writes: its
shader_common.h and the prologue of constant and descriptor-index macros (ShaderRecompiler's, by the same pattern),
and writes <vs|ps>_<container XXH3>.hlsl into the directory of BANJO_HLSL_OVERRIDE (the early alpha pass's), so the
translator's second run compiles them in place of its own translation (app/src/renut_engine/gpu/gpu.cmake).

    python tools/renut_port_shaders.py --out <override dir> [--common <shader_common.h>]
"""
import argparse
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = os.path.join(ROOT, 'shaders', 'port', 'renut')
COMMON = os.path.join(ROOT, 'app', 'thirdparty', 'reblue-XenosRecomp', 'XenosRecomp', 'shader_common.h')

# The containers' XXH3 (the names the translator gives their HLSL), the body and its defines.
SHADERS = [
    ('vs_3caebedd6edf56e6', 'peggle1_renut_movie_vs.hlsl', []),
    ('ps_23fb686cb21303f8', 'peggle1_renut_movie_ps.hlsl', []),
    ('ps_b79e9877de15d6db', 'peggle1_renut_movie_ps.hlsl', ['RENUT_MOVIE_ALPHA']),
]

SAMPLER_COUNT = 32  # shader_recompiler.cpp
TEXTURE_DIMENSIONS = ['2D', '3D', 'Cube']
SWIZZLES = 'xyzw'


def prologue(pixel):
    """ShaderRecompiler's text between shader_common.h and the shader's constant names, for a shader whose samplers
    have no names (s0 to s31)."""
    name = 'Pixel' if pixel else 'Vertex'
    out = ['#ifdef __spirv__', '']
    out.append(f'#define g_C(INDEX) select((uint)(INDEX) < 256u, g_Ubo{name}.v[min((uint)(INDEX), 255u)], 0.0)')
    for r in range(SAMPLER_COUNT):
        for j, dim in enumerate(TEXTURE_DIMENSIONS):
            out.append(f'#define s{r}_Texture{dim}DescriptorIndex RENUT_SHARED_UINT({j * SAMPLER_COUNT * 4 + r * 4})')
        out.append(f'#define s{r}_SamplerDescriptorIndex '
                   f'RENUT_SHARED_UINT({len(TEXTURE_DIMENSIONS) * SAMPLER_COUNT * 4 + r * 4})')
    out += ['', '#else', '']
    out.append(f'cbuffer {name}ShaderConstants : register(b{1 if pixel else 0}, space4)')
    out += ['{', '\tfloat4 g_CArr[256] : packoffset(c0);', '};', '',
            '#define g_C(INDEX) select((uint)(INDEX) < 256u, g_CArr[min((uint)(INDEX), 255u)], 0.0)', '',
            'cbuffer SharedConstants : register(b2, space4)', '{']
    for r in range(SAMPLER_COUNT):
        for j, dim in enumerate(TEXTURE_DIMENSIONS):
            out.append(f'\tuint s{r}_Texture{dim}DescriptorIndex : '
                       f'packoffset(c{j * (SAMPLER_COUNT // 4) + r // 4}.{SWIZZLES[r % 4]});')
        out.append(f'\tuint s{r}_SamplerDescriptorIndex : '
                   f'packoffset(c{len(TEXTURE_DIMENSIONS) * (SAMPLER_COUNT // 4) + r // 4}.{SWIZZLES[r % 4]});')
    out += ['\tDEFINE_SHARED_CONSTANTS();', '};', '', '#endif', '', '']
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--common', default=COMMON)
    a = ap.parse_args()
    common = open(a.common, encoding='utf-8').read()
    os.makedirs(a.out, exist_ok=True)
    for stem, body_name, defines in SHADERS:
        pixel = stem.startswith('ps_')
        body = open(os.path.join(PORT, body_name), encoding='utf-8').read()
        text = (f'// peggle2-nx port shader {stem} (tools/renut_port_shaders.py, shaders/port/renut/{body_name})\n'
                + common + '\n' + prologue(pixel)
                + ''.join(f'#define {d} 1\n' for d in defines) + body)
        path = os.path.join(a.out, stem + '.hlsl')
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    print(f'renut_port_shaders: {len(SHADERS)} port shaders -> {a.out}')


if __name__ == '__main__':
    main()
