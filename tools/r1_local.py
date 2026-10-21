#!/usr/bin/env python3
"""cpu_aggressive (proposed post-codegen step): the guest stack pointer r1 as a C++ local mirror in every calling
function, so the compiler does not reload ctx.r1 after each call.

Today every function reads r1 from ctx (`REX_STORE_D_U32(ctx.r1.u32, 116, ...)`); a call may write ctx, so after
each call the compiler loads ctx.r1 again (about 65,000 of the Switch ELF's 104,000 ctx.r1 loads). Whole-program
fact of Banjo's executable (peggle2-nx: the copy of 2026-10-19 skips the functions Peggle's game patches change)
(tools r1_preserve.py): 38,414 of its 39,674 functions return with r1 equal to its value at
entry (prologue `stwu r1,-N(r1)` and epilogue `addi r1,r1,N` / `lwz r1,0(r1)` balanced on every path, through their
callees, to a greatest fixed point); 14,104 of them without assuming anything about indirect calls, hooks and imports.

The rewrite, per function that calls anything (no setjmp / longjmp):
  REX_FUNC_PROLOGUE();  REX_R1_LOCAL();          // PPCRegister r1l = ctx.r1  (OFF: PPCRegister& r1l = ctx.r1)
  every `ctx.r1.` -> `r1l.`; every statement that writes r1l is followed by REX_R1_SYNC() (ctx.r1 = r1l: ctx stays
  exact at all times, so callees, hooks and host code see the same r1 as before);
  after each statement with a call:
    REX_R1_KEEP()     the callee is proven to preserve r1 without assumptions: nothing;
    REX_R1_ASSUMED()  proven only if indirect calls, hooks, imports and host helpers preserve r1 (every guest function
                      reachable through them is in the proven set or reloads; the PC build checks this assumption at
                      every such call with REX_R1_CHECK, and REX_R1_STRICT reloads instead);
    REX_R1_RELOAD()   anything else (a callee that moves r1: epilogue fragments, unknown frames): r1l = ctx.r1.
Switch back: REX_R1_LOCAL_OFF makes r1l a reference to ctx.r1 and every macro nothing: the original code.

  python r1_local.py --gen <tree> --proof-strict <r1_preserve.json> --proof-assumed <r1_preserve.json> [--restore]
Idempotent (a function that already has REX_R1_LOCAL is left alone). --restore takes the rewrite out.
"""
import argparse
import json
import os
import re
import sys

MARKER = '// cpu_aggressive: r1 as a local mirror (r1_local.py)'
MACROS = MARKER + r'''
#ifndef REX_R1_LOCAL
#if defined(REX_R1_LOCAL_OFF)
#define REX_R1_LOCAL() PPCRegister& r1l = ctx.r1
#define REX_R1_SYNC() ((void)0)
#define REX_R1_RELOAD() ((void)0)
#define REX_R1_KEEP() ((void)0)
#define REX_R1_ASSUMED() ((void)0)
#else
#define REX_R1_LOCAL() PPCRegister r1l = ctx.r1
#define REX_R1_SYNC() (ctx.r1.u64 = r1l.u64)
#define REX_R1_RELOAD() (r1l.u64 = ctx.r1.u64)
#if defined(REX_R1_CHECK)
extern "C" void RexR1Mismatch(unsigned long long expected, unsigned long long got) noexcept;
extern "C" volatile unsigned long long g_rex_r1_checked;
#define REX_R1_KEEP()                                                         \
  do {                                                                        \
    g_rex_r1_checked = g_rex_r1_checked + 1;                                  \
    if (ctx.r1.u64 != r1l.u64) [[unlikely]] {                                 \
      RexR1Mismatch(r1l.u64, ctx.r1.u64);                                     \
      r1l.u64 = ctx.r1.u64;                                                   \
    }                                                                         \
  } while (0)
#define REX_R1_ASSUMED() REX_R1_KEEP()
#elif defined(REX_R1_STRICT)
#define REX_R1_KEEP() ((void)0)
#define REX_R1_ASSUMED() REX_R1_RELOAD()
#else
#define REX_R1_KEEP() ((void)0)
#define REX_R1_ASSUMED() ((void)0)
#endif
#endif
#endif
'''
HEAD = re.compile(r'^DEFINE_REX_FUNC\((?:__imp__)?(\w+)\) \{$')
CALL = re.compile(r'\b(__imp__)?([A-Za-z_]\w*)\(ctx, base(?:, [^)]*)?\)')
SKIP_CALLS = {'REX_PROMOTED_IS'}
ASSIGN = re.compile(r'^\tr1l\.\w+ = ')
TAIL = re.compile(r'(\(ctx, base\);\s*return;\s*\}?\s*$)|(^\treturn;)')


GUEST = set()


def classify(line, strict, assumed):
    """'keep' / 'assumed' / 'reload' for a statement line with calls, or None (no call)."""
    worst = None
    order = {'keep': 0, 'assumed': 1, 'reload': 2}
    found = False
    for imp, name in CALL.findall(line):
        if name in SKIP_CALLS or name.startswith('REX_LOAD') or name.startswith('REX_STORE'):
            continue
        found = True
        if name in GUEST:
            if name in strict:
                c = 'keep' if (imp or not name.startswith('sub_')) else 'assumed'   # sub_X: through its hook
            elif name in assumed:
                c = 'assumed'
            else:
                c = 'reload'                       # a guest function not proven to preserve r1
        else:
            c = 'assumed'                          # host helpers: ppc_trap, Rex*, imports, hooks
        if worst is None or order[c] > order[worst]:
            worst = c
    if 'REX_CALL_INDIRECT' in line or 'REX_PROMOTED_CALL' in line:
        found = True
        if worst is None or order['assumed'] > order[worst]:
            worst = 'assumed'
    return worst if found else None


def rewrite(body, strict, assumed, stats):
    text = '\n'.join(body)
    if 'REX_R1_LOCAL()' in text:
        stats['already'] += 1
        return None
    if 'ppc_setjmp' in text or 'longjmp' in text:
        stats['setjmp'] += 1
        return None
    if '\n\t// peggle2-nx:' in '\n' + text:
        stats['opaque'] = stats.get('opaque', 0) + 1   # peggle2-nx: a game patch changed it (host calls with &ctx)
        return None
    if not re.search(r'ctx\.r1\b', text):
        return None
    if not any(classify(l, strict, assumed) for l in body if not l.lstrip().startswith('//')):
        stats['no_call'] += 1
        return None
    out = []
    added = False
    for line in body:
        if line.lstrip().startswith('//'):
            out.append(line)
            continue
        new = line.replace('ctx.r1.', 'r1l.').replace('ctx.r1)', 'r1l)').replace('ctx.r1,', 'r1l,')
        if re.search(r'ctx\.r1\b', new):
            stats['odd'] += 1
            return None
        if not added and new.strip() == 'REX_FUNC_PROLOGUE();':
            out.append(new)
            out.append('\tREX_R1_LOCAL();')
            added = True
            continue
        suffix = ''
        if ASSIGN.match(new) or re.match(r'^\tREX_LOOP_OUT\(\w+, r1l\);', new):
            suffix += ' REX_R1_SYNC();'
            stats['sync'] += 1
        c = classify(new, strict, assumed)
        if c and not re.search(r'return;\s*\}?\s*$', new):
            suffix += {'keep': ' REX_R1_KEEP();', 'assumed': ' REX_R1_ASSUMED();', 'reload': ' REX_R1_RELOAD();'}[c]
            stats[c] += 1
        out.append(new + suffix)
    if not added:
        stats['no_prologue'] += 1
        return None
    stats['functions'] += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--proof-strict')
    ap.add_argument('--proof-assumed')
    ap.add_argument('--restore', action='store_true')
    ap.add_argument('--only', help='comma-separated function names')
    ap.add_argument('--only-file', help='JSON list of function names (the per-function choice, tools/choice.py)')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pch = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')][0]
    stats = {k: 0 for k in ('functions', 'keep', 'assumed', 'reload', 'sync', 'already', 'setjmp', 'no_call', 'odd',
                            'no_prologue', 'files')}
    if a.restore:
        for f in files:
            p = os.path.join(a.gen, f)
            t = open(p, encoding='utf-8').read()
            if 'REX_R1_LOCAL()' not in t:
                continue
            t = t.replace('\tREX_R1_LOCAL();\n', '')
            t = re.sub(r' REX_R1_(?:SYNC|KEEP|ASSUMED|RELOAD)\(\);', '', t)
            t = t.replace('r1l.', 'ctx.r1.').replace('r1l)', 'ctx.r1)').replace('r1l,', 'ctx.r1,')
            open(p, 'w', encoding='utf-8', newline='\n').write(t)
        return
    js = json.load(open(a.proof_strict))
    ja = json.load(open(a.proof_assumed))
    GUEST.update(js['preserving'], js['delta'], js['not'], ja['preserving'], ja['delta'], ja['not'])
    strict = set(js['preserving'])
    assumed = set(json.load(open(a.proof_assumed))['preserving']) - strict
    only = set(a.only.split(',')) if a.only else None
    if a.only_file:
        only = (only or set()) | set(json.load(open(a.only_file)))
    p = os.path.join(a.gen, pch)
    t = open(p, encoding='utf-8').read()
    if MARKER not in t:
        anchor = '// rexglue ports: dead stack-frame traffic (tools/frame_traffic.py)'
        i = t.index(anchor)
        t = t[:i] + MACROS + t[i:]
        open(p, 'w', encoding='utf-8', newline='\n').write(t)
    for f in files:
        path = os.path.join(a.gen, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        new = []
        i = 0
        changed = False
        while i < len(lines):
            m = HEAD.match(lines[i])
            if not m:
                new.append(lines[i])
                i += 1
                continue
            j = i + 1
            while j < len(lines) and lines[j] != '}':
                j += 1
            body = lines[i + 1:j]
            r = None if (only and m.group(1) not in only) else rewrite(body, strict, assumed, stats)
            new.append(lines[i])
            new.extend(r if r is not None else body)
            changed |= r is not None
            i = j
        if changed:
            stats['files'] += 1
            open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(new))
    print('r1_local: %s' % stats)


if __name__ == '__main__':
    main()
