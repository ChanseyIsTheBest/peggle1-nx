#!/usr/bin/env python3
"""Prints a rough listing of a range of a guest image dump, for reading code by hand (the portable LLVM has no
PowerPC target, so llvm-mc cannot do it). Decoding is tools/ppc_mini.py's: the integer forms the analysis tools need;
anything else shows as its family and raw word.

  python tools/ppc_dis.py 0x823320D8 [count=32] [--image out/default_image.bin] [--base 0x82000000]
  python tools/ppc_dis.py 0x823320C0-0x82332140
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppc_mini import Image, decode, text  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('where')
    ap.add_argument('count', nargs='?', type=int, default=32)
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'default_image.bin'))
    ap.add_argument('--base', default='0x82000000')
    args = ap.parse_args()
    img = Image(args.image, int(args.base, 16))
    if '-' in args.where:
        lo, hi = (int(x, 16) for x in args.where.split('-'))
        count = (hi - lo) // 4
    else:
        lo, count = int(args.where, 16), args.count
    for i in range(count):
        a = lo + 4 * i
        if not img.contains(a):
            break
        w = img.word(a)
        print(f'{a:08X}  {w:08X}  {text(decode(w, a))}')


if __name__ == '__main__':
    sys.exit(main())
