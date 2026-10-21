#!/usr/bin/env python3
"""Whole-program ARM counts of the recompiled functions (sub_* / __imp__sub_*) of Switch ELFs, one objdump pass each:
instructions, loads, stores, calls, and what the CPU rounds change (msr fpcr, dmb, fcvtas, frame pointers, tbl).
  python tools/arm_recomp_totals.py A.elf [B.elf ...]"""
import bisect
import re
import subprocess
import sys

NM = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-nm.exe'
OBJDUMP = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-objdump.exe'
REC = re.compile(r'(__imp__)?sub_[0-9A-F]{8}(\.\w+)*$')
KEYS = ['functions', 'bytes', 'insns', 'loads', 'stores', 'rev', 'bl', 'blr', 'msr_fpcr', 'mrs_fpcr', 'dmb', 'fcvtas',
        'fcvtzs', 'fcvtns', 'sdiv_udiv', 'tbl', 'frame_ptr', 'fmov_gp_fp']


def ranges(elf):
    out = subprocess.run([NM, '-S', '--defined-only', elf], capture_output=True, text=True).stdout
    r = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2].lower() in ('t', 'w') and REC.match(p[3]):
            a, n = int(p[0], 16), int(p[1], 16)
            if n:
                r.append((a, a + n))
    r.sort()
    merged = []
    for a, b in r:
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
        else:
            merged.append((a, b))
    return merged, len(r)


def count(elf):
    rs, nfunc = ranges(elf)
    starts = [a for a, _ in rs]
    c = dict.fromkeys(KEYS, 0)
    c['functions'] = nfunc
    c['bytes'] = sum(b - a for a, b in rs)
    proc = subprocess.Popen([OBJDUMP, '-d', '--no-show-raw-insn', '-j', '.text', elf], stdout=subprocess.PIPE,
                            text=True, bufsize=1 << 20)
    line_re = re.compile(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$')
    for line in proc.stdout:
        m = line_re.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        i = bisect.bisect_right(starts, addr) - 1
        if i < 0 or addr >= rs[i][1]:
            continue
        op, args = m.group(2), m.group(3)
        c['insns'] += 1
        if op.startswith('ld'):
            c['loads'] += 1
        elif op.startswith('st'):
            c['stores'] += 1
        elif op.startswith('rev'):
            c['rev'] += 1
        elif op == 'bl':
            c['bl'] += 1
        elif op == 'blr':
            c['blr'] += 1
        elif op == 'msr' and 'fpcr' in args:
            c['msr_fpcr'] += 1
        elif op == 'mrs' and 'fpcr' in args:
            c['mrs_fpcr'] += 1
        elif op == 'dmb':
            c['dmb'] += 1
        elif op == 'fcvtas':
            c['fcvtas'] += 1
        elif op == 'fcvtzs':
            c['fcvtzs'] += 1
        elif op == 'fcvtns':
            c['fcvtns'] += 1
        elif op in ('sdiv', 'udiv'):
            c['sdiv_udiv'] += 1
        elif op == 'tbl':
            c['tbl'] += 1
        elif op in ('mov', 'add') and args.startswith('x29, sp'):
            c['frame_ptr'] += 1
        elif op == 'fmov' and re.match(r'[sdv]\d+(\.\w+\[\d\])?, [wx]\d+', args):
            c['fmov_gp_fp'] += 1
    proc.wait()
    return c


def main():
    rows = [(e, count(e)) for e in sys.argv[1:]]
    for e, c in rows:
        print(e)
        print('   ' + ', '.join(f'{k} {c[k]}' for k in KEYS))
    if len(rows) >= 2:
        a = rows[0][1]
        for e, b in rows[1:]:
            print(f'{rows[0][0]} against {e}:')
            print('   ' + ', '.join(f'{k} {100.0 * (a[k] - b[k]) / b[k]:+.1f} %' if b[k] else f'{k} {a[k]} (was 0)'
                                    for k in KEYS))


if __name__ == '__main__':
    main()
