"""Finds the code that builds given guest addresses (string or data references) in Peggle's image.

The compiler forms an address as `lis rX,hi` followed (within a few instructions, often interleaved) by a D-form
instruction on rX: addi/ori (the address itself) or a load/store at rX+lo (the data there). Every such pair whose sum
lands in one of the given ranges is printed with the function it is in (from the generated code).
  python -P tools/xrefs.py 0x82009EC0 [0x82009F5C ...] [--span 0x20] [--window 48]
  (--span: also matches addresses up to that many bytes past each given one, e.g. a member of a struct)
"""
import argparse
import bisect
import glob
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000
# D-form opcodes whose rA + simm16 is an address: addi, lwz, lwzu, lbz, lbzu, stw, stwu, stb, stbu, lhz, lhzu, lha,
# lhau, sth, sthu, lfs, lfsu, lfd, lfdu, stfs, stfsu, stfd, stfdu; ori (24) uses the unsigned low half.
D_FORM = {14, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55}
NAMES = {14: 'addi', 24: 'ori', 32: 'lwz', 34: 'lbz', 36: 'stw', 38: 'stb', 40: 'lhz', 42: 'lha', 44: 'sth',
         48: 'lfs', 50: 'lfd', 52: 'stfs', 54: 'stfd'}


def function_starts(gen):
    s = set()
    for p in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        for m in re.finditer(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)', open(p, errors='replace').read(), re.M):
            s.add(int(m.group(1), 16))
    return sorted(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addrs', nargs='+')
    ap.add_argument('--span', default='0')
    ap.add_argument('--window', type=int, default=48)
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'default_image.bin'))
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated', 'default'))
    a = ap.parse_args()
    span = int(a.span, 0)
    targets = [int(x, 16) for x in a.addrs]
    img = open(a.image, 'rb').read()
    n = len(img) // 4
    words = struct.unpack('>%dI' % n, img[:n * 4])
    starts = function_starts(a.gen)
    lo_code, hi_code = starts[0], starts[-1] + 0x10000
    for t in targets:
        hits = []
        for i in range((lo_code - BASE) // 4, min(n, (hi_code - BASE) // 4)):
            w = words[i]
            if w >> 26 != 15 or (w >> 16) & 31 != 0:  # lis rX,hi (addis rX,0,hi)
                continue
            rx = (w >> 21) & 31
            hi = (w & 0xFFFF) << 16
            for j in range(i + 1, min(n, i + 1 + a.window)):
                v = words[j]
                op = v >> 26
                ra = (v >> 16) & 31
                rd = (v >> 21) & 31
                if op in D_FORM or op == 24:
                    base_reg = rd if op == 24 else ra  # ori rA,rS,uimm: the source is the rS field
                    if base_reg == rx:
                        lo = v & 0xFFFF
                        if op != 24 and lo & 0x8000:
                            lo -= 0x10000
                        addr = (hi + lo) & 0xFFFFFFFF
                        if t <= addr <= t + span:
                            pc = BASE + j * 4
                            k = bisect.bisect_right(starts, pc) - 1
                            fn = 'sub_%08X' % starts[k] if k >= 0 else '?'
                            hits.append((pc, fn, NAMES.get(op, 'op%d' % op), addr))
                # rX redefined (a GPR load or add into it, or ori into it): the lis no longer applies
                written = ra if op == 24 else rd
                if op in (14, 15, 24, 32, 33, 34, 35, 40, 41, 42, 43) and written == rx:
                    break
                if op == 18 and v & 1 == 0:  # unconditional branch without link: the block ends
                    break
                if v == 0x4E800020:  # blr
                    break
        print('%08X: %d reference(s)' % (t, len(hits)))
        for pc, fn, name, addr in hits:
            print('  %08X in %s: %s -> %08X' % (pc, fn, name, addr))


if __name__ == '__main__':
    main()
