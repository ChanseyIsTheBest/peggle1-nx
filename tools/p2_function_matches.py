"""Finds Peggle 2 functions whose code is in Peggle 1 too (byte for byte, or the same with other branch targets).

For each address of a list of Peggle 2 functions (default: Peggle 2's localized copies and hand-written natives), its
code (to the next function start of Peggle 2's generated tree) is searched in Peggle 1's image dump:
  exact    the same bytes (a hand-written native of Peggle 2 then applies as it is; its FNV check passes)
  masked   the same instructions with the displacement fields of relative branches (b, bl, bc) ignored: the same
           function placed elsewhere, calling the same-shaped functions
Matches are only taken at Peggle 1 function starts (its generated tree).

  python -P tools/p2_function_matches.py [--list <file of P2 addresses>]
"""
import argparse
import glob
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000
P2 = r'D:\peggle 2\peggle2-nx'


def starts(gen, pat):
    s = set()
    for p in glob.glob(os.path.join(gen, pat)):
        for m in re.finditer(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)', open(p, errors='replace').read(), re.M):
            s.add(int(m.group(1), 16))
    return sorted(s)


def masked(words):
    out = []
    for w in words:
        op = w >> 26
        if op == 18:
            w &= 0xFC000003
        elif op == 16:
            w &= 0xFFFF0003
        out.append(w)
    return tuple(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', default=None)
    a = ap.parse_args()
    if a.list:
        addrs = [int(l.split()[0], 16) for l in open(a.list) if l.strip() and not l.startswith('#')]
    else:
        addrs = [int(l.split()[0], 16) for l in open(os.path.join(ROOT, 'out', 'to_delete', 'p2_configs',
                                                                 'localized_peggle2.txt'))
                 if l.strip() and not l.startswith('#')]
        addrs += [0x825F0AE0, 0x825F10B0]
    img2 = open(os.path.join(P2, 'out', 'default_image.bin'), 'rb').read()
    img1 = open(os.path.join(ROOT, 'out', 'default_image.bin'), 'rb').read()
    s2 = starts(os.path.join(P2, 'app', 'generated', 'default'), 'peggle2_recomp.*.cpp')
    s1 = starts(os.path.join(ROOT, 'app', 'generated', 'default'), 'peggle1_recomp.*.cpp')
    import bisect
    n1 = len(img1) // 4
    w1 = struct.unpack('>%dI' % n1, img1[:n1 * 4])
    s1set = set(s1)
    # index Peggle 1 function starts by their first two masked words
    index = {}
    for f in s1:
        i = (f - BASE) // 4
        if i + 2 <= n1:
            index.setdefault(masked(w1[i:i + 2]), []).append(f)
    for a2 in addrs:
        i = bisect.bisect_right(s2, a2)
        end = s2[i] if i < len(s2) else a2 + 0x400
        size = min(end - a2, 0x4000)
        words2 = struct.unpack('>%dI' % (size // 4), img2[a2 - BASE:a2 - BASE + size])
        key = masked(words2[:2])
        found = []
        for f in index.get(key, []):
            j = (f - BASE) // 4
            words1 = w1[j:j + len(words2)]
            if words1 == words2:
                found.append((f, 'exact'))
            elif masked(words1) == masked(words2):
                found.append((f, 'masked'))
        desc = ', '.join(f'{f:08X} {k}' for f, k in found) or '-'
        print(f'{a2:08X} ({size} bytes): {desc}')


if __name__ == '__main__':
    main()
