#!/usr/bin/env python3
"""stvlx / stvrx without byte loops (from sasrt-nx's tool of 2026-10-14, idea 5 of sa2-nx's list; banjo-nx's copy).
The generator writes the left / right unaligned vector stores (stvlx, stvrx and their 128 / l forms) as a loop of up
to 16 single-byte stores:
    for (size_t i = 0; i < (16 - (ea & 0xF)); i++)          ->   REX_STVLX(ea, V);
        REX_STORE_U8(ea + i, V.u8[15 - i]);
    for (size_t i = 0; i < (ea & 0xF); i++)                  ->   REX_STVRX(ea, V);
        REX_STORE_U8(ea - i - 1, V.u8[i]);
REX_STVLX / REX_STVRX (sdk/include/rex/ppc/vec_unaligned.h) reverse the vector once and store the same bytes with at
most five stores. Exact: the same bytes get the same values and no other byte is written (the range stays in one
16-byte block, which never straddles the 0xE0000000 host offset); sdk/tests/vec_unaligned_test.cpp checks every
alignment against the loops. The lvlx / lvrx loads are not touched.
In Banjo (2026-10-14): 735 stvlx and 277 stvrx loops in 56 generated files.
REX_VEC_UNALIGNED_LOOPS (compile time) gives the loops back.
(peggle2-nx's copy, 2026-10-19: unchanged but for this line; the tree is app/generated/default.)
  python tools/vec_unaligned_stores.py --gen app/generated/default [--check]
"""
import argparse
import glob
import os
import re
import sys

LEFT = re.compile(r'^\tfor \(size_t i = 0; i < \(16 - \(ea & 0xF\)\); i\+\+\)\n'
                  r'\t\tREX_STORE_U8\(ea \+ i, ([\w.]+)\.u8\[15 - i\]\);$', re.M)
RIGHT = re.compile(r'^\tfor \(size_t i = 0; i < \(ea & 0xF\); i\+\+\)\n'
                   r'\t\tREX_STORE_U8\(ea - i - 1, ([\w.]+)\.u8\[i\]\);$', re.M)
MARKER = '// rexglue ports: stvlx / stvrx without byte loops (tools/vec_unaligned_stores.py)'
END = '// (end of the macros of tools/vec_unaligned_stores.py)\n'
MACROS = MARKER + '''
#ifndef REX_STVLX
#if defined(REX_VEC_UNALIGNED_LOOPS)
#define REX_STVLX(ea, v) for (size_t i = 0; i < (16 - ((ea) & 0xF)); i++) REX_STORE_U8((ea) + i, (v).u8[15 - i])
#define REX_STVRX(ea, v) for (size_t i = 0; i < ((ea) & 0xF); i++) REX_STORE_U8((ea) - i - 1, (v).u8[i])
#else
#include <rex/ppc/vec_unaligned.h>
#define REX_STVLX(ea, v) rex::ppc::StoreVectorLeft(REX_RAW_ADDR(ea), (ea), (v).u8)
#define REX_STVRX(ea, v) rex::ppc::StoreVectorRight(REX_RAW_ADDR((ea) & ~0xFu), (ea), (v).u8)
#endif  // REX_VEC_UNALIGNED_LOOPS
#endif  // REX_STVLX
''' + END


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    left = right = changed = already = 0
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        already += text.count('REX_STVLX(') + text.count('REX_STVRX(')
        new, nl = LEFT.subn(r'\tREX_STVLX(ea, \1);', text)
        new, nr = RIGHT.subn(r'\tREX_STVRX(ea, \1);', new)
        left += nl
        right += nr
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): vec_unaligned_stores: no single *_pch.h in %s' % a.gen)
    text = open(pchs[0], encoding='utf-8').read()
    if MACROS not in text and not a.check:
        anchor = '#define REX_LOAD_U8(x)'
        if MARKER in text:
            start = text.index(MARKER)
            text = text[:start] + MACROS + text[text.index(END, start) + len(END):]
        elif anchor in text:
            text = text.replace(anchor, MACROS + anchor, 1)
        else:
            sys.exit('ERROR (post-codegen patch): vec_unaligned_stores: no REX_LOAD_U8 in %s' % pchs[0])
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text)
    print('vec_unaligned_stores%s: %d stvlx and %d stvrx byte loops written as a few stores (%d files; %d already in '
          'that form)' % (' (check only)' if a.check else '', left, right, changed, already))


if __name__ == '__main__':
    main()
