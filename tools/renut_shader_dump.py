#!/usr/bin/env python3
"""peggle2-nx (2026-10-19): the shader containers Renut's translator (thirdparty/reblue-XenosRecomp) builds its shader
cache from, for Peggle 2.

Two sources, written as <out>/{vs,ps}_<xxh3_64 of the container>.bin (the name Renut's renderer looks shaders up by:
XXH3 of header + physical block, as D3DDevice_Create*Shader receives them):
  - the decrypted executable image (out/default_image.bin, written by the code generator): containers compiled into
    the executable are stored whole there (magic 0x102A11xx, low bit 1 = vertex shader). The scan is Renut's
    dump_shaders.py find_shader_headers;
  - containers a run dumped (peggle1_dump_shaders = true writes <exe folder>/shaders/*.bin at Create*Shader; Renut's
    renderer's own capture folder, shader_capture/, holds the ones its cache lacked): copied as they are.

  python tools/renut_shader_dump.py --image out/default_image.bin --dumps out/pc/shaders out/pc/shader_capture
                                    --out out/renut_shader_dump
"""
import argparse
import os
import re
import struct
import sys

import xxhash

SHADER_MAGIC = re.compile(rb"\x10\x2a\x11", re.S)


def find_shader_headers(data):
    hdrs = []
    for m in SHADER_MAGIC.finditer(data):
        o = m.start()
        if o % 4 or o + 0x24 > len(data):
            continue
        virt, phys = struct.unpack_from(">II", data, o + 4)
        ct, dt, so = struct.unpack_from(">III", data, o + 0x10)
        if not (0x24 <= virt < 0x10000 and 0 < phys < 0x100000 and o + virt <= len(data)):
            continue
        if ct >= virt or dt >= virt or not (0x24 <= so < virt - 8):
            continue
        poff, psize = struct.unpack_from(">II", data, o + so)
        if poff + psize != phys:
            continue
        hdrs.append(o)
    return hdrs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', required=True)
    ap.add_argument('--dumps', nargs='*', default=[])
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    image = open(a.image, 'rb').read()
    found = {'vs': 0, 'ps': 0}
    for h in find_shader_headers(image):
        virt, phys = struct.unpack_from(">II", image, h + 4)
        blob = image[h:h + virt + phys]
        if len(blob) != virt + phys:
            continue
        kind = 'vs' if struct.unpack_from(">I", image, h)[0] & 1 else 'ps'
        name = '%s_%s.bin' % (kind, xxhash.xxh3_64_hexdigest(blob))
        path = os.path.join(a.out, name)
        if not os.path.exists(path):
            open(path, 'wb').write(blob)
        found[kind] += 1
    copied = 0
    for folder in a.dumps:
        if not os.path.isdir(folder):
            continue
        for name in os.listdir(folder):
            if re.fullmatch(r'(vs|ps)_[0-9a-f]{16}\.bin', name):
                dst = os.path.join(a.out, name)
                if not os.path.exists(dst):
                    open(dst, 'wb').write(open(os.path.join(folder, name), 'rb').read())
                    copied += 1
    total = len([n for n in os.listdir(a.out) if n.endswith('.bin')])
    print('renut_shader_dump: %d vertex and %d pixel shader containers in the image; %d copied from run dumps; %d in %s'
          % (found['vs'], found['ps'], copied, total, a.out))


if __name__ == '__main__':
    sys.exit(main())
