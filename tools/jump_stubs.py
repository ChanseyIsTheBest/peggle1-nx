"""Finds the linker's jump stubs in the image dump and checks a list of addresses against them.

Peggle's default.xex (and its title update) holds runs of 16-byte stubs that jump to a function through ctr:

    lis   r11, hi(target)
    addi  r11, r11, lo(target)
    mtctr r11
    bctr

Calls reach them by `bl` (the generator makes a function of those) and by `b` (tail calls: those it reports as
"target not in any function"). Each stub is a function of its own.

Usage: python -P tools/jump_stubs.py [--targets out/unresolved_targets.txt] [--image out/default_image.bin]
Prints the stub runs, then for each listed target whether it is a stub start and where the stub jumps.
"""
import argparse
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000


def stub_at(img, off):
    if off + 16 > len(img):
        return None
    w0, w1, w2, w3 = struct.unpack_from('>4I', img, off)
    if (w0 & 0xFFFF0000) != 0x3D600000 or (w1 & 0xFFFF0000) != 0x396B0000:
        return None
    if w2 != 0x7D6903A6 or w3 != 0x4E800420:
        return None
    hi = (w0 & 0xFFFF) << 16
    lo = w1 & 0xFFFF
    if lo & 0x8000:
        lo -= 0x10000
    return (hi + lo) & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'default_image.bin'))
    ap.add_argument('--targets', default=None)
    ap.add_argument('--toml', action='store_true', help='print [functions] lines for the stub targets')
    args = ap.parse_args()
    img = open(args.image, 'rb').read()
    stubs = {}
    off = 0
    while off + 16 <= len(img):
        t = stub_at(img, off)
        if t is not None:
            stubs[BASE + off] = t
            off += 16
        else:
            off += 4
    runs = []
    for a in sorted(stubs):
        if runs and runs[-1][1] == a:
            runs[-1][1] = a + 16
            runs[-1][2] += 1
        else:
            runs.append([a, a + 16, 1])
    print(f'{len(stubs)} stubs in {len(runs)} runs')
    for s, e, n in runs:
        if n >= 4:
            print(f'  {s:08X}-{e:08X}: {n} stubs')
    if args.targets:
        targets = [int(x, 16) for x in open(args.targets).read().split()]
        bad = [t for t in targets if t not in stubs]
        print(f'{len(targets)} targets, {len(targets) - len(bad)} are stub starts, {len(bad)} are not')
        for t in bad:
            print(f'  not a stub: {t:08X}')
        if args.toml:
            for t in sorted(targets):
                if t in stubs:
                    print(f'"0x{t:08X}" = {{ size = 0x10 }}  # stub -> 0x{stubs[t]:08X}')


if __name__ == '__main__':
    main()
