#!/usr/bin/env python3
"""Writes the shader containers stored in the decompressed default.xex image as <stage>_<XXH3 of the container>.bin,
the names peggle1_dump_shaders gives the same containers when the game passes them to D3DDevice_Create*Shader.

The scan is the installer's (peggle2-nx-installer lib/shaders.js imageContainers, futari-nx's tools/extract_shaders.py):
a word-aligned 0x102A1101 (vertex) / 0x102A1100 (pixel) magic, +4 virtual size 12..64 KiB, +8 microcode size 4..1 MiB
(whole words) inside the image. Peggle 2's image holds exactly three: vs_6b6d1eda0e6e465b (0x820528D0),
ps_a17c79e99907d662 (0x82052AD8), ps_558cf34ef8661cae (0x82052C38). The game's other three shaders are compiled at
run time (D3DXCompileShader) and are not in any file.

  python tools/extract_stored_shaders.py out/default_image.bin --out <folder>

Needs the xxhash module (as tools/hd/ does).
"""
import argparse
import glob
import os
import struct
import sys

import xxhash

BASE = 0x82000000


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', help='decompressed default.xex image (out/default_image.bin), loaded at 0x82000000')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    image = open(a.image, 'rb').read()
    os.makedirs(a.out, exist_ok=True)
    for old in glob.glob(os.path.join(a.out, '*.bin')):
        os.remove(old)  # regenerated from scratch
    found = 0
    for off in range(0, len(image) - 12, 4):
        magic = struct.unpack_from('>I', image, off)[0]
        if magic not in (0x102A1101, 0x102A1100):
            continue
        header, ucode = struct.unpack_from('>II', image, off + 4)
        if header < 12 or header > 1 << 16 or ucode == 0 or ucode > 1 << 20 or ucode % 4:
            continue
        if off + header + ucode > len(image):
            continue
        container = image[off:off + header + ucode]
        name = f'{"vs" if magic == 0x102A1101 else "ps"}_{xxhash.xxh3_64_intdigest(container):016x}.bin'
        open(os.path.join(a.out, name), 'wb').write(container)
        print(f'{name}  {BASE + off:08X}  {len(container)} bytes')
        found += 1
    print(f'{found} containers')
    return 0 if found else 1


if __name__ == '__main__':
    sys.exit(main())
