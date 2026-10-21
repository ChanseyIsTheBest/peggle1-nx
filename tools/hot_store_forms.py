"""Every way a localized copy's logged variant (NativeLogged) can write guest memory: the forms the store log knows
against any other mention of guest memory that is not a known load."""
import collections
import glob
import os
import re

HOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'app', 'generated', 'hot').replace(chr(92), '/')
KNOWN_LOAD = re.compile(
    r'\bREX_LOAD_(?:D_)?(?:U8|U16|U32|U64|F32|F64)\(|\bREX_RO_\w+\(|simde_mm_load_si128\(\(simde__m128i\*\)REX_RAW_ADDR\('
    r'|simde_mm_loadu_si128\(\(simde__m128i\*\)REX_RAW_ADDR\(')
LOGGED = re.compile(r'\bPEGGLE1_LOG_(?:STORE_(?:D_)?U(?:8|16|32|64)|STORE_V128|ZERO)\(')
forms = collections.Counter()
unknown = collections.Counter()
examples = {}
leaves = 0
for p in sorted(glob.glob(HOT + '/n_*.h')):
    t = open(p, encoding='utf-8').read()
    i = t.find('inline void NativeLogged(')
    if i < 0:
        continue
    leaves += 1
    body = t[i:]
    for l in body.split('\n'):
        for m in LOGGED.finditer(l):
            forms[m.group(0)] += 1
        rest = LOGGED.sub('X(', l)
        rest = KNOWN_LOAD.sub('L(', rest)
        # Anything left that names guest memory or a store.
        for m in re.finditer(r'\b(REX_STORE\w*|REX_RAW_ADDR|simde_mm_store\w*|simde_mm_maskmove\w*|memcpy|memset|memmove|'
                             r'REX_\w*STORE\w*|REX_\w*ZERO\w*|REX_DCBZ\w*|base\s*\+|REX_MEM\w*|__builtin_mem\w*)', rest):
            unknown[m.group(1)] += 1
            examples.setdefault(m.group(1), (p.split('/')[-1].split(chr(92))[-1], l.strip()[:260]))
print(leaves, 'leaf copies with a logged variant')
print('logged forms:', dict(forms))
print('other mentions of guest memory in logged variants:', dict(unknown))
for k, (f, l) in examples.items():
    print(f'  {k}: {f}: {l}')
