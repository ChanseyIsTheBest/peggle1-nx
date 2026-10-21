"""One pass over the generated tree: how many sites of each form the rounds wrote."""
import collections
import glob
import os
import re

GEN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'app', 'generated', 'default').replace(chr(92), '/')
PAT = re.compile(r'\b(rex::ppc::(?:fctidz|fctid|fctiwz|fctiw|div\w+|fp_copy\w*|copy_\w+)|REX_PROMOTED_CALL|REX_CALL_INDIRECT_FUNC_AT|'
                 r'REX_CALL_INDIRECT_FUNC|REX_GUEST_CAS\w*|REX_FLUSH_KNOWN_ON|REX_FLUSH_KNOWN_OFF|REX_RO_\w+|REX_LOAD_F32|'
                 r'REX_LOAD_D_F32|REX_BSWAP128|REX_SHUFFLE_TABLE|REX_LOOP_LOCAL|REX_LEAF_LOCAL|REX_LOOP_BARRIER|'
                 r'ctx\.fpscr\.\w+|REX_FP_\w+|rex::ppc::\w+)\b')
PPC = re.compile(r'^\t// (\w+\.?) ', re.M)
c = collections.Counter()
ops = collections.Counter()
funcs = 0
for p in glob.glob(GEN + '/*_recomp.*.cpp'):
    t = open(p, encoding='utf-8', errors='replace').read()
    funcs += t.count('\nDEFINE_REX_FUNC(')
    for m in PAT.finditer(t):
        c[m.group(1)] += 1
    for m in PPC.finditer(t):
        ops[m.group(1)] += 1
print('functions', funcs)
for k, v in sorted(c.items(), key=lambda kv: -kv[1]):
    print(f'{v:8d} {k}')
print('PPC:', {k: ops[k] for k in ('fctid', 'fctidz', 'fctiw', 'fctiwz', 'divw', 'divwu', 'divd', 'divdu', 'bctrl', 'stwcx.', 'stdcx.',
                                   'lfs', 'lfsx', 'lfd', 'fcmpu', 'fcmpo') if k in ops})
print('PPC instructions:', sum(ops.values()))
