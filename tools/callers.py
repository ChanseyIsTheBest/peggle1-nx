"""Lists who calls a function in Peggle's image, through its jump stubs too.

Most calls in default.xex reach a function through one of the linker's stubs (tools/jump_stubs.py): `bl stub`,
and the stub jumps on. This finds the stubs that lead to each given address and every `bl`/`b` to the function
itself or to one of them, and names the function each call site is in (from the generated code).

  python -P tools/callers.py 0x8246C3F8 [0x...] [--image out/default_image.bin] [--gen app/generated/default] [--max 20]
"""
import argparse
import bisect
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jump_stubs import stub_at  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000
EXEC = [(0x82140000, 0x8219626C), (0x82196400, 0x82BC6F8C), (0x82BC7000, 0x82F778A2)]


def function_starts(gen):
    starts = set()
    for path in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        for m in re.finditer(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)', open(path, errors='replace').read(), re.M):
            starts.add(int(m.group(1), 16))
    return sorted(starts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('targets', nargs='+')
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'default_image.bin'))
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated', 'default'))
    ap.add_argument('--max', type=int, default=20)
    args = ap.parse_args()
    img = open(args.image, 'rb').read()
    starts = function_starts(args.gen)
    targets = [int(t, 16) for t in args.targets]
    stubs_to = {}
    for s, e in EXEC:
        for a in range(s, e, 4):
            t = stub_at(img, a - BASE)
            if t in targets:
                stubs_to.setdefault(t, []).append(a)
    for t in targets:
        via = set(stubs_to.get(t, [])) | {t}
        sites = []
        for s, e in EXEC:
            for a in range(s, e, 4):
                w = struct.unpack_from('>I', img, a - BASE)[0]
                if (w >> 26) != 18 or (w & 2):
                    continue
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                dest = (a + li) & 0xFFFFFFFF
                if dest in via and a not in stubs_to.get(t, []):
                    i = bisect.bisect_right(starts, a) - 1
                    owner = starts[i] if i >= 0 else 0
                    sites.append((a, 'bl' if w & 1 else 'b', owner))
        owners = sorted({o for _, _, o in sites})
        print(f'{t:08X}: {len(stubs_to.get(t, []))} stubs, {len(sites)} call sites in {len(owners)} functions')
        for a, kind, owner in sites[:args.max]:
            print(f'  {kind} at {a:08X} in sub_{owner:08X}')


if __name__ == '__main__':
    main()
