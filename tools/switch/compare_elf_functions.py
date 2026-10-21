#!/usr/bin/env python3
"""Per-function summary of the recompiled functions of a Switch ELF, and the comparison of two summaries: what a
compiler flag or a codegen step changed, function by function (2026-10-13: the cost of -mearly-ra=none).

  python tools/switch/compare_elf_functions.py summary <elf> <out.json>
  python tools/switch/compare_elf_functions.py compare <a.json> <b.json> [--list N]

The summary holds, for every sub_XXXXXXXX / __imp__sub_XXXXXXXX: the number of instructions, a hash of the sequence
of mnemonics (addresses and registers left out: a function whose code only moved has the same hash), and how many
one-register and two-register table lookups it has. A summary takes about a minute per ELF.
"""
import collections
import hashlib
import json
import re
import subprocess
import sys

OBJDUMP = 'C:/devkitPro/devkitA64/bin/aarch64-none-elf-objdump.exe'


def summary(elf, out):
    proc = subprocess.Popen([OBJDUMP, '-d', '--no-show-raw-insn', elf], stdout=subprocess.PIPE, text=True,
                            errors='replace', bufsize=1 << 20)
    head = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
    funcs = {}
    cur = None
    for line in proc.stdout:
        m = head.match(line)
        if m:
            name = m.group(1)
            if re.fullmatch(r'(?:__imp__)?sub_[0-9A-F]{8}', name):
                cur = funcs.setdefault(name, {'n': 0, 'h': hashlib.md5(), 'tbl1': 0, 'tbl2': 0, 'vec': 0})
            else:
                cur = None
            continue
        if cur is None or not line.startswith(' '):
            continue
        parts = line.split('\t')
        if len(parts) < 2:
            continue
        op = parts[1].strip()
        args = parts[2] if len(parts) > 2 else ''
        cur['n'] += 1
        cur['h'].update(op.encode() + b' ')
        if op == 'tbl':
            if '-' in args.split('}')[0]:
                cur['tbl2'] += 1
            else:
                cur['tbl1'] += 1
        if re.search(r'\b[vq]\d+\b|\bv\d+\.', args):
            cur['vec'] += 1
    proc.wait()
    json.dump({k: {'n': v['n'], 'h': v['h'].hexdigest(), 'tbl1': v['tbl1'], 'tbl2': v['tbl2'], 'vec': v['vec']}
               for k, v in funcs.items()}, open(out, 'w'))
    print('%s: %d recompiled functions, %d instructions' % (elf, len(funcs), sum(v['n'] for v in funcs.values())))


def compare(a_path, b_path, show):
    a, b = json.load(open(a_path)), json.load(open(b_path))
    common = sorted(set(a) & set(b))
    changed = [f for f in common if a[f]['h'] != b[f]['h']]
    print('functions: %d in A, %d in B, %d in both; mnemonic sequence differs in %d' % (
        len(a), len(b), len(common), len(changed)))
    na, nb = sum(a[f]['n'] for f in common), sum(b[f]['n'] for f in common)
    print('instructions in the common functions: A %d, B %d (%+d, %+.3f %%)' % (na, nb, nb - na, 100.0 * (nb - na) / na))
    groups = collections.Counter()
    for f in changed:
        two = a[f]['tbl2'] or b[f]['tbl2']
        vec = a[f]['vec'] or b[f]['vec']
        groups['with two-register lookups' if two else 'with vector/FP-register code, no two-register lookup' if vec
               else 'without vector or FP registers'] += 1
    for k, v in groups.most_common():
        print('  changed %s: %d' % (k, v))
    two_all = [f for f in common if a[f]['tbl2'] or b[f]['tbl2']]
    print('functions with two-register lookups: %d, of them changed: %d' % (
        len(two_all), sum(1 for f in two_all if a[f]['h'] != b[f]['h'])))
    lost = [f for f in common if a[f]['tbl1'] + a[f]['tbl2'] != b[f]['tbl1'] + b[f]['tbl2']]
    print('functions whose number of table lookups differs: %d' % len(lost))
    for f in sorted(lost, key=lambda f: -abs((a[f]['tbl1'] + a[f]['tbl2']) - (b[f]['tbl1'] + b[f]['tbl2'])))[:show]:
        print('    %-22s A: %4d one-register + %3d two-register   B: %4d + %3d   instructions %6d -> %6d' % (
            f, a[f]['tbl1'], a[f]['tbl2'], b[f]['tbl1'], b[f]['tbl2'], a[f]['n'], b[f]['n']))
    d = sorted(changed, key=lambda f: -abs(b[f]['n'] - a[f]['n']))[:show]
    print('largest changes in instruction count:')
    for f in d:
        print('    %-22s %6d -> %6d (%+d)  two-register lookups %d' % (f, a[f]['n'], b[f]['n'], b[f]['n'] - a[f]['n'],
                                                                  b[f]['tbl2']))


if __name__ == '__main__':
    if sys.argv[1] == 'summary':
        summary(sys.argv[2], sys.argv[3])
    else:
        compare(sys.argv[2], sys.argv[3], int(sys.argv[sys.argv.index('--list') + 1]) if '--list' in sys.argv else 12)
