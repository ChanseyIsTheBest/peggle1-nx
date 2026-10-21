#!/usr/bin/env python3
"""peggle2-nx (2026-10-19): banjo-nx's codegen round taken back out of a processed tree, so a second run of
tools/post_codegen.py gives every step the text it had in the first.

Banjo's post_codegen.py takes its LATE rewrites back out before the other steps (r1l, REX_LR_WRITE_SAME, hoisted
checks, inline copies, REX_VSEL: tools/inline_leaves.py, vsel_neon.py, r1_local.py, lr_preserve.py, flush_hoist.py
--restore). Peggle's run showed that the EARLIER steps of the round change what the base steps decide on a second
run: tools/loop_barriers.py did not read a return written as `{ REX_LEAF_STORE(..); REX_LEAF_DEAD_R(..); return; }`
(10 files got other barriers), and tools/flush_mode_flow.py and tools/ro_constants.py proved and folded more on the
round's forms. A tree whose text depends on how often the script ran is not what a PGO profile was made from. So this
step undoes the rest of the round too, in the reverse order of the steps (each the inverse of that tool's rewrite):
  tools/dead_ctx_defs.py        REX_DEAD_DEF / REX_DEAD_REC        -> the statements (its own restore())
  tools/ro_constants_cfg.py     REX_RO_CFG_<kind>(a, v, load)      -> load
  tools/vec_unaligned_stores.py REX_STVLX / REX_STVRX              -> the generator's byte loops
  tools/inline_memcalls.py      REX_MEMCPY_CONST / _MEMSET_CONST   -> the call
  tools/single_floats.py        REX_FADDS / ... / REX_FRSP_SINGLE  -> double(float(...)) (its own UNDO patterns)
  tools/leaf_dead_stores.py     REX_LEAF_DEAD_R / _F               -> REX_LEAF_STORE
  tools/fp_sign_ops.py          REX_FNEG / REX_FABS / REX_FNABS    -> the sign-mask statements
  tools/vperm_one_source.py     REX_VPERM1(L, C)                   -> rex::ppc::simde_mm_perm_epi8_(L, L, C)
The pch keeps the macro blocks (each step finds its block there and leaves it). Run after the late steps' --restore
and before every other step; on a fresh codegen it finds nothing. A file whose text does not change is not written.

  python tools/round_restore.py --gen app/generated/default
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dead_ctx_defs  # noqa: E402
import single_floats  # noqa: E402
import vperm_one_source  # noqa: E402

RO_CFG = re.compile(r'REX_RO_CFG_(?:U8|U16|U32|U64|F32)\(0x[0-9A-F]{8}, 0x[0-9A-F]+(?:u|ull), '
                    r'(REX_LOAD_D_(?:U8|U16|U32|U64|F32)\([\w.]+\.u32, -?\d+\))\)')
VEC_LEFT = re.compile(r'^\tREX_STVLX\(ea, ([\w.]+)\);$', re.M)
VEC_RIGHT = re.compile(r'^\tREX_STVRX\(ea, ([\w.]+)\);$', re.M)
MEMCALL = re.compile(r'\bREX_MEM(?:CPY|SET)_CONST\(\d+, ([^;]*\(ctx, base\))\);')
LEAF_DEAD = re.compile(r'\bREX_LEAF_DEAD_[RF]\((\w+), ([rf]\d+)\)')
FP_SIGN = re.compile(r'^\t(REX_FNEG|REX_FABS|REX_FNABS)\(([A-Za-z_][\w.]*), ([A-Za-z_][\w.]*)\);$', re.M)
FP_SIGN_FORM = {'REX_FNEG': '^ 0x8000000000000000', 'REX_FABS': '& ~0x8000000000000000',
                'REX_FNABS': '| 0x8000000000000000'}
VPERM1 = re.compile(r'\bREX_VPERM1\((' + vperm_one_source.LOAD + r'), ')
UNMACRO = single_floats.UNMACRO


def restore_text(text):
    counts = {}

    def sub(name, pattern, repl, t):
        new, n = pattern.subn(repl, t)
        if n:
            counts[name] = counts.get(name, 0) + n
        return new
    text = sub('ro_constants_cfg', RO_CFG, r'\1', text)
    text = sub('vec_unaligned_stores', VEC_LEFT,
               lambda m: '\tfor (size_t i = 0; i < (16 - (ea & 0xF)); i++)\n\t\tREX_STORE_U8(ea + i, %s.u8[15 - i]);'
               % m.group(1), text)
    text = sub('vec_unaligned_stores', VEC_RIGHT,
               lambda m: '\tfor (size_t i = 0; i < (ea & 0xF); i++)\n\t\tREX_STORE_U8(ea - i - 1, %s.u8[i]);'
               % m.group(1), text)
    text = sub('inline_memcalls', MEMCALL, r'\1;', text)
    text = sub('single_floats', single_floats.UNDO_BINARY, lambda m: '\t%s.f64 = double(float(%s.f64 %s %s.f64));' % (
        m.group(1), m.group(3), UNMACRO[m.group(2)], m.group(4)), text)
    text = sub('single_floats', single_floats.UNDO_SQRT, r'\t\1.f64 = double(float(sqrt(\2.f64)));', text)
    text = sub('single_floats', single_floats.UNDO_FRSP, r'\t\1.f64 = double(float(\2.f64));', text)
    text = sub('leaf_dead_stores', LEAF_DEAD, r'REX_LEAF_STORE(\1, \2)', text)
    text = sub('fp_sign_ops', FP_SIGN, lambda m: '\t%s.u64 = %s.u64 %s;' % (m.group(2), m.group(3),
                                                                           FP_SIGN_FORM[m.group(1)]), text)
    text = sub('vperm_one_source', VPERM1, r'rex::ppc::simde_mm_perm_epi8_(\1, \1, ', text)
    return text, counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    a = ap.parse_args()
    restored_defs = dead_ctx_defs.restore(a.gen)
    total = {}
    changed = 0
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        new, counts = restore_text(text)
        for k, v in counts.items():
            total[k] = total.get(k, 0) + v
        if new != text:
            changed += 1
            open(path, 'w', encoding='utf-8', newline='\n').write(new)
    print('round_restore: dead_ctx_defs restored in %d files; %s; %d files written' % (
        restored_defs, ', '.join('%s %d' % kv for kv in sorted(total.items())) or 'nothing else to take out', changed))


if __name__ == '__main__':
    main()
