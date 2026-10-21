#!/usr/bin/env python3
"""Static AArch64 instruction counts of functions in two Switch ELFs (the user's rule: judge a change by what it removes
on the Cortex-A57, not by PC milliseconds). For each function: total instructions, loads, stores, byte reverses
(guest big-endian accesses), branches and calls, in ELF A and ELF B.

  python tools/arm_insn_count.py A.elf B.elf sub_82153480 __imp__sub_82396650 ... [--all-recompiled]

--all-recompiled sums every __imp__sub_* / sub_* function instead (whole-program view; inlining moves code between
functions, so per-function numbers of a direct-calls build can grow while the program shrinks).
"""
import collections
import re
import subprocess
import sys

NM = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-nm.exe'
OBJDUMP = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-objdump.exe'


def symbols(elf):
    out = subprocess.run([NM, '-S', '--defined-only', elf], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2].lower() in ('t', 'w'):
            syms[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
    return syms


def classify(mnemonic):
    m = mnemonic
    if m.startswith(('ldr', 'ldp', 'ldur', 'ld1', 'ld2', 'ld3', 'ld4', 'ldx', 'ldax', 'ldar')):
        return 'load'
    if m.startswith(('str', 'stp', 'stur', 'st1', 'st2', 'st3', 'st4', 'stx', 'stlx', 'stlr')):
        return 'store'
    if m.startswith('rev'):
        return 'rev'
    if m in ('bl', 'blr'):
        return 'call'
    if m.startswith(('b', 'cb', 'tb', 'ret', 'br')):
        return 'branch'
    return 'other'


def count(elf, syms, names):
    total = collections.Counter()
    ranges = sorted((syms[n][0], syms[n][0] + syms[n][1]) for n in names if n in syms and syms[n][1])
    # objdump once per contiguous chunk of ranges (fast enough for a few thousand functions).
    for start, stop in ranges:
        out = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', f'--start-address=0x{start:x}',
                              f'--stop-address=0x{stop:x}', elf], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r'^\s+[0-9a-f]+:\s+([a-z][a-z0-9.]*)', line)
            if m:
                total['insn'] += 1
                total[classify(m.group(1))] += 1
    return total


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 2:
        raise SystemExit(__doc__)
    a, b, *names = args
    sa, sb = symbols(a), symbols(b)
    if '--all-recompiled' in sys.argv:
        pick = lambda s: [n for n in s if re.fullmatch(r'(__imp__)?sub_[0-9A-F]{8}', n)]
        groups = [('all recompiled', pick(sa), pick(sb))]
    else:
        groups = [(n, [n], [n]) for n in names]
    keys = ('insn', 'load', 'store', 'rev', 'branch', 'call')
    print(f'{"function":28s} ' + ' '.join(f'{k:>15s}' for k in keys))
    for label, na, nb in groups:
        ca, cb = count(a, sa, na), count(b, sb, nb)
        cells = []
        for k in keys:
            cells.append(f'{ca[k]:>6d}->{cb[k]:<7d}' if ca[k] or cb[k] else f'{"-":>15s}')
        print(f'{label[:28]:28s} ' + ' '.join(cells))


if __name__ == '__main__':
    main()
