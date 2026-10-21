#!/usr/bin/env python3
"""Makes library containers for the vertex shaders Direct3D rewrote for a vertex declaration.

The native renderer's discovery (MASSEFFECT_SHADER_DISCOVERY) keeps the exact microcode of every shader the ring
loads that the library cannot identify (<stage>_<hash>.ucode + .txt with the address it was loaded from). A vertex
shader loaded from guest memory (address != 0) whose microcode has the same size as a real container (the dump of
D3DDevice_CreateVertexShader) and differs from it only in vertex-fetch words is Direct3D's rewrite of that shader
for the bound declaration. For each one this writes

  <out>/actual/vs_<hash>.bin    the real container with its microcode replaced by the loaded one (nothing else
                                changed), and
  <out>/repaired/vs_<hash>.bin  the same after shaders/tools/repair_vertex_variant_declarations.cpp, which remaps
                                the DECL to the rewritten fetch order and refuses anything but fetch-only changes
                                (binary-mode output).

Immediate shaders (address 0, Direct3D's own) are left to tools/wrap_discovered.py.

  python tools/make_vertex_variants.py --raw out/pc/shaders_discovery --containers out/pc/shaders \
      --out out/pc/shaders_variants_made [--repair out/tools/repair_vertex_variant_declarations.exe]
"""
import argparse
import glob
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def be(b, o):
    return struct.unpack_from('>I', b, o)[0]


def microcode_extent(container):
    """(start, size in bytes) of the microcode inside the file: virtual part + the code offset of the header."""
    virtual = be(container, 4)
    header = be(container, 24)
    return virtual + be(container, header), be(container, header + 4)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--raw', nargs='+', required=True)
    ap.add_argument('--containers', nargs='+', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--repair', default=os.path.join(ROOT, 'out', 'tools', 'repair_vertex_variant_declarations.exe'))
    a = ap.parse_args()

    real = []
    for d in a.containers:
        for p in sorted(glob.glob(os.path.join(d, 'vs_*.bin'))):
            c = open(p, 'rb').read()
            if len(c) < 36 or (be(c, 0) & 0xFFFFFF01) != 0x102A1101:
                continue
            start, size = microcode_extent(c)
            real.append((p, c, start, [be(c, start + 4 * i) for i in range(size // 4)]))
    actual_dir = os.path.join(a.out, 'actual')
    repaired_dir = os.path.join(a.out, 'repaired')
    os.makedirs(actual_dir, exist_ok=True)
    os.makedirs(repaired_dir, exist_ok=True)
    for d in (actual_dir, repaired_dir):
        for old in glob.glob(os.path.join(d, '*.bin')):
            os.remove(old)
    made = repaired = 0
    seen = set()
    for d in a.raw:
        for p in sorted(glob.glob(os.path.join(d, 'vs_*.ucode'))):
            name = os.path.basename(p)[:-len('.ucode')]
            if name in seen:
                continue
            seen.add(name)
            txt = p[:-len('.ucode')] + '.txt'
            address = 0
            if os.path.exists(txt):
                for line in open(txt):
                    if line.startswith('address='):
                        address = int(line.split('=')[1], 16)
            if not address:
                print(f'skip {name}: immediate (Direct3D\'s own shader)')
                continue
            raw = open(p, 'rb').read()
            words = [be(raw, 4 * i) for i in range(len(raw) // 4)]
            best = None
            for path, c, start, code in real:
                if len(code) != len(words):
                    continue
                diff = [i for i in range(len(words)) if words[i] != code[i]]
                # Only vertex-fetch instructions may differ: fetch words have opcode 0 in the low 5 bits of the
                # instruction's first word (the repair tool proves the rest).
                if all((code[(i // 3) * 3] & 31) == 0 for i in diff) and (best is None or len(diff) < best[0]):
                    best = (len(diff), path, c, start)
            if not best:
                print(f'skip {name}: no same-size real container differing only in fetch words')
                continue
            n, path, c, start = best
            actual = bytearray(c)
            actual[start:start + len(raw)] = raw
            out_actual = os.path.join(actual_dir, f'{name}.bin')
            open(out_actual, 'wb').write(actual)
            made += 1
            out_repaired = os.path.join(repaired_dir, f'{name}.bin')
            r = subprocess.run([a.repair, path, out_actual, out_repaired], capture_output=True, text=True)
            ok = r.returncode == 0 and os.path.exists(out_repaired)
            repaired += ok
            print(f'{name}: from {os.path.basename(path)} ({n} fetch words differ), repair '
                  f'{"ok" if ok else "REFUSED"}: {(r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip() else ""}')
    print(f'{made} variants made, {repaired} repaired')


if __name__ == '__main__':
    sys.exit(main())
