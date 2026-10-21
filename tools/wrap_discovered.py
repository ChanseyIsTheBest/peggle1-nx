#!/usr/bin/env python3
"""Wraps the raw ring microcode the native renderer discovered (MASSEFFECT_SHADER_DISCOVERY: <stage>_<hash>.ucode)
into register-linked containers for the shader library, but ONLY the shaders no real container covers.

Why the filter matters: a raw discovery is often a vertex shader Direct3D rewrote for a vertex declaration, whose
real container also exists (the dump from CreateVertexShader, a variant repaired by repair_vertex_variant_declarations,
or the rewritten container MASSEFFECT_SHADER_MISSING keeps). Packed together, the library's code index prefers the
register-linked wrapper when two entries share their microcode, and a register-linked vertex shader paired with the
game's semantic-linked pixel shader passes its interpolators by the wrong location: SA2's health warning, SEGA and Sonic
Team screens came out black (the texture coordinates arrived as zero). Only Direct3D's own shaders, which have no
container anywhere, are wrapped.

  python tools/wrap_discovered.py --raw out/pc/shaders_discovery out/pc/shaders_discovery2 \
      --containers out/pc/shaders out/pc/shaders_missing out/pc/shaders_repaired --out out/pc/shaders_wrapped

The wrapper's template of each stage is the first vs_/ps_*.bin by name in the first --containers folder, unless
--template-vs / --template-ps name one. The template keeps only bytes that neither the translator nor the renderer reads
(wrap_raw_shader.py rewrites every header word they use), so the translated shader does not depend on it; it only
changes the container, so its key in the library.

--immediate-only wraps only what the ring loaded with IM_LOAD_IMMEDIATE (address 0 in the .txt: Direct3D's own
shaders) and leaves loads from guest memory (vertex shaders Direct3D rewrote for a declaration) to
tools/make_vertex_variants.py, without needing their containers in --containers to be skipped.

The release library (2026-10-14, the installer's recipe; tools/build_shader_library_static.sh) wraps with the
containers stored in default.xex, because the first dumps by name (ps_23fb686cb21303f8, vs_3caebedd6edf56e6) are
shaders the game compiles at run time, which the installer cannot make:

  python tools/wrap_discovered.py --raw out/pc/shaders_discovery --containers <stored> <variants>/repaired \\
      --out <wrapped> --immediate-only --template-vs <stored>/vs_6b6d1eda0e6e465b.bin \\
      --template-ps <stored>/ps_558cf34ef8661cae.bin
"""
import argparse
import glob
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def physical_part(container):
    if len(container) < 12:
        return b''
    virtual = struct.unpack_from('>I', container, 4)[0]
    physical = struct.unpack_from('>I', container, 8)[0]
    return container[virtual:virtual + physical]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--raw', nargs='+', required=True, help='folders with <stage>_<hash>.ucode files')
    ap.add_argument('--containers', nargs='+', required=True, help='folders with real containers (*.bin)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--template-vs', help='template container of the vertex shaders (default: the first vs_*.bin by '
                                          'name in the first --containers folder)')
    ap.add_argument('--template-ps', help='template container of the pixel shaders (default: likewise)')
    ap.add_argument('--immediate-only', action='store_true',
                    help='wrap only the IM_LOAD_IMMEDIATE loads (address 0 in the .txt: Direct3D\'s own shaders)')
    a = ap.parse_args()

    covered = []
    for d in a.containers:
        for p in glob.glob(os.path.join(d, '*.bin')):
            covered.append((os.path.basename(p), physical_part(open(p, 'rb').read())))
    templates = {s: sorted(glob.glob(os.path.join(a.containers[0], f'{s}_*.bin'))) for s in ('vs', 'ps')}
    for s, chosen in (('vs', a.template_vs), ('ps', a.template_ps)):
        if chosen:
            if not os.path.isfile(chosen):
                sys.exit(f'no template {chosen}')
            templates[s] = [chosen]
        if templates[s]:
            print(f'{s} template: {templates[s][0]}')
    os.makedirs(a.out, exist_ok=True)
    for old in glob.glob(os.path.join(a.out, '*.bin')):
        os.remove(old)  # the folder is regenerated from scratch, so a skipped shader cannot linger
    wrapped = skipped = 0
    seen = set()
    for d in a.raw:
        for p in sorted(glob.glob(os.path.join(d, '*.ucode'))):
            name = os.path.basename(p)[:-len('.ucode')]
            if name in seen:
                continue
            seen.add(name)
            if a.immediate_only:
                txt = p[:-len('.ucode')] + '.txt'
                address = None
                if os.path.exists(txt):
                    for line in open(txt):
                        if line.startswith('address='):
                            address = int(line.split('=')[1], 16)
                if address != 0:
                    print(f'skip {name}: loaded from guest memory (address '
                          f'{"unknown" if address is None else f"{address:08X}"}), not an immediate load')
                    skipped += 1
                    continue
            raw = open(p, 'rb').read()
            owner = next((n for n, phys in covered if raw and raw in phys), None)
            if owner:
                print(f'skip {name}: its microcode is in the real container {owner}')
                skipped += 1
                continue
            stage = name.split('_', 1)[0]
            if not templates.get(stage):
                sys.exit(f'no {stage} template container in {a.containers[0]}')
            subprocess.run([sys.executable, os.path.join(ROOT, 'shaders', 'tools', 'wrap_raw_shader.py'), p,
                            templates[stage][0], os.path.join(a.out, name + '.bin'), '--stage', stage], check=True)
            print(f'wrap {name}')
            wrapped += 1
    print(f'{wrapped} wrapped (Direct3D\'s own shaders), {skipped} skipped (a real container has them' +
          (', or not an immediate load)' if a.immediate_only else ')'))


if __name__ == '__main__':
    main()
