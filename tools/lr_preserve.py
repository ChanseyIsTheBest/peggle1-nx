#!/usr/bin/env python3
"""cpu_aggressive: proof that the epilogue's `mtlr` (REX_LR_WRITE, tools/frame_traffic.py's mark) writes back the value
ctx.lr already holds, in Banjo's executable, so it can compile to nothing WITHOUT changing any guest-visible state.
(peggle2-nx's copy, 2026-10-19: Peggle's tree; no no-return addresses of Banjo's; the functions Peggle's game
patches change neither preserve ctx.lr nor get a rewrite: tools/leaf_dead_stores.py, OPAQUE_MARK.)

Since elide_lr_stores, no call sets ctx.lr; a C++ call returns by itself. So ctx.lr only changes where the code says
so: REX_LR_WRITE in epilogues, the generator's plain `ctx.lr = ...` (functions where the link register is data:
`bl` to a local label, `mtlr rX` of a computed value), host code. A standard function saves ctx.lr (REX_LR_READ +
REX_DEAD_LR_SAVE at entry r1 - 8), and its epilogue reloads that slot and writes it back. If every callee returns
with ctx.lr as it found it, ctx.lr at the epilogue still equals the value saved at entry, and the write-back stores
the same value: removing it changes nothing (the slot is still written at entry; only the dead reload and the
store to ctx go away).

Per function, a forward data flow over parse_function's graph on the statements, with symbolic values:
  lr      ctx.lr is still the entry value (E) or not (T);
  regs    registers holding a copy of E (REX_LR_READ, plain `rX.u64 = ctx.lr`, a reload of an intact slot);
  slot    the word at entry r1 - 8 holds E's low half (written by the entry save; any other store at r1-relative
          entry offset -8 makes it T); r1 followed as in r1_preserve.py (d = r1 - entry r1);
  a call  of a function proven to preserve ctx.lr keeps lr; anything else (indirect, hooked, import, unproven):
          T, unless --assume-indirect (then the PC check REX_LR_WRITE_CHECK verifies it at run time).
A REX_LR_WRITE(reg) site is PROVEN when reg holds E and lr is E. A function PRESERVES ctx.lr when lr is E at every
exit (greatest fixed point over the call graph). Upper half: the save keeps the low 32 bits and the reload
zero-extends, so "the same value" also needs ctx.lr < 2^32 at function entry: every writer of ctx.lr writes a 32-bit
value except a plain `ctx.lr = rX.u64` of a sign-extended register; those functions are listed (the PC check
compares all 64 bits).

  python lr_preserve.py --gen <tree> --out <dir> [--assume-indirect]
Writes <dir>/lr_preserve.json: {"preserving": [...], "proven_sites": {function: [statement line indices]},
"unproven_sites": {...}, "plain_writers": [...]}.
"""
import argparse
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import leaf_dead_stores as lds  # noqa: E402
import dead_ctx_defs as dcd  # noqa: E402

DFORM = re.compile(r'^(-?\d+)\((r\d+)\)$')
ASSIGN = re.compile(r'^(?:REX_DEAD_DEF\()?((?:ctx\.)?[rf]\d+)\.(?:u64|s64|u32|s32|u8|u16|s8|s16|f64)\s*=')
NORETURN = set()  # peggle2-nx: Banjo's addresses do not apply; a callee with no exit is found by the flow itself


def instructions(lines):
    return [(m.group(1), m.group(2).strip()) for m in (lds.COMMENT.match(l) for l in lds.resolve_stubs(lines)) if m]


def flow(nodes, ins, chunks, pres, assume):
    """(preserves, proven sites, unproven sites)"""
    n = len(nodes)
    if n == 0 or len(ins) != n:
        return n == 0, [], []
    E, T = 'E', 'T'
    state = [None] * n
    state[0] = (0, E, T, frozenset(), frozenset())   # d, lr, slot, regs-with-E, frame pointers (reg, d)
    work = [0]
    proven, unproven = set(), set()
    exits_ok = True
    seen = 0
    while work:
        seen += 1
        if seen > 400000:
            return False, [], []
        i = work.pop()
        d, lr, slot, regs, fps = state[i]
        regs = set(regs)
        fps = dict(fps)
        mn, ops = ins[i]
        b = mn.rstrip('.')
        o = [x.strip() for x in ops.split(',')] if ops else []
        node = nodes[i]
        stmts = [t for _, t in chunks[i][1]]
        for k, t in enumerate(stmts):
            m = re.match(r'^REX_LR_READ\(((?:ctx\.)?r\d+)\);', t)
            if m:
                (regs.add if lr == E else regs.discard)(m.group(1))
                continue
            m = re.match(r'^REX_DEAD_LR_SAVE\(REX_STORE_D_U32\(ctx\.r1\.u32, -8, ((?:ctx\.)?r\d+)\.u32\)\);', t)
            if m:
                slot = E if (m.group(1) in regs and d == 0) else T
                continue
            m = re.match(r'^REX_LR_WRITE\(((?:ctx\.)?r\d+)\);', t)
            if m:
                ok = m.group(1) in regs and lr == E
                (proven if ok else unproven).add(chunks[i][1][k][0])
                lr = E if m.group(1) in regs else T
                continue
            m = re.match(r'^((?:ctx\.)?r\d+)\.u64 = REX_LOAD_D_U32\(ctx\.r1\.u32, -8\);', t)
            if m:
                (regs.add if (slot == E and d == 0) else regs.discard)(m.group(1))
                continue
            m = re.match(r'^((?:ctx\.)?r\d+)\.u64 = ctx\.lr;', t)
            if m:
                (regs.add if lr == E else regs.discard)(m.group(1))
                continue
            if re.match(r'^ctx\.lr = ', t):
                m = re.match(r'^ctx\.lr = ((?:ctx\.)?r\d+)\.u64;', t)
                lr = E if (m and m.group(1) in regs) else T
                continue
            if 'ctx.lr' in t:
                lr = T                      # old_lr blocks and the like
            m = ASSIGN.match(t)
            if m:
                regs.discard(m.group(1))
        # r1 and stores to the slot (guest instruction level)
        if b.startswith('st') and len(o) > 1 and b not in ('stwu', 'stdu'):
            mm = DFORM.match(o[1])
            if mm and mm.group(2) == 'r1' and d is not None:
                e = d + int(mm.group(1))
                size = 8 if b.startswith(('std', 'stfd')) else 4
                if e < -4 and e + size > -8 and not any('REX_DEAD_LR_SAVE' in t for t in stmts):
                    slot = T
        newfp = None
        if b in ('stwu', 'stdu') and o and o[0] == 'r1':
            mm = DFORM.match(o[1])
            d = d + int(mm.group(1)) if (mm and mm.group(2) == 'r1' and d is not None) else None
        elif b in ('addi',) and len(o) == 3 and o[0] == 'r1' and o[1] == 'r1':
            d = d + int(o[2]) if d is not None else None
        elif b in ('addi',) and len(o) == 3 and o[0] == 'r1' and o[1] in fps:
            d = fps[o[1]] + int(o[2])
        elif b == 'mr' and len(o) == 2 and o[0] == 'r1' and o[1] in fps:
            d = fps[o[1]]
        elif node.defn & lds.R(1):
            d = None                         # lwz r1,0(r1) / alloca: the slot address is no longer known
        if b == 'addi' and len(o) == 3 and o[0] != 'r1' and o[1] == 'r1' and d is not None:
            newfp = (o[0], d + int(o[2]))
        elif b == 'mr' and len(o) == 2 and o[0] != 'r1' and o[1] == 'r1' and d is not None:
            newfp = (o[0], d)
        for r in list(fps):
            if node.defn & lds.R(int(r[1:])):
                del fps[r]
        if newfp:
            fps[newfp[0]] = newfp[1]
        text = ' '.join(stmts)
        called = node.call or 'REX_CALL_INDIRECT' in text or 'REX_PROMOTED' in text
        if not called and node.use == lds.ALL and b.startswith('b'):
            called = True
        if called:
            if node.call and node.call[1]:
                callee = node.call[0]
                if '%08X' % callee in NORETURN:
                    continue
                if not pres(callee):
                    lr = T
            elif not assume:
                lr = T
        if node.exit and lr != E:
            exits_ok = False
        st = (d, lr, slot, frozenset(regs), frozenset(fps.items()))
        for s in node.succ:
            old = state[s]
            if old is None:
                new = st
            else:
                new = (old[0] if old[0] == st[0] else None, E if (old[1] == E and st[1] == E) else T,
                       E if (old[2] == E and st[2] == E) else T, old[3] & st[3], old[4] & st[4])
            if new != old:
                state[s] = new
                work.append(s)
    # a site is proven only if proven on every visit (the last state is the fixed point: recompute)
    return exits_ok, sorted(proven - unproven), sorted(unproven)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--assume-indirect', action='store_true')
    ap.add_argument('--rewrite', action='store_true', help='write REX_LR_WRITE_SAME at the proven sites of --gen')
    ap.add_argument('--restore', action='store_true', help='REX_LR_WRITE_SAME back to REX_LR_WRITE (then stop)')
    ap.add_argument('--only-file', help='JSON list: rewrite only these functions (the per-function choice)')
    a = ap.parse_args()
    if a.restore:
        n = 0
        for f in os.listdir(a.gen):
            if re.match(r'\w+_recomp\.\d+\.cpp$', f):
                p = os.path.join(a.gen, f)
                t = open(p, encoding='utf-8').read()
                if 'REX_LR_WRITE_SAME(' in t:
                    open(p, 'w', encoding='utf-8', newline='\n').write(t.replace('REX_LR_WRITE_SAME(', 'REX_LR_WRITE('))
                    n += 1
        print('lr_preserve: restored %d files' % n)
        return
    os.makedirs(a.out, exist_ok=True)
    lds.load_mid_asm_hooks(a.gen)
    named = dcd.named_functions(a.gen)
    hooked = dcd.hooked_names(set(named))
    bodies = dcd.all_functions(a.gen, named)
    names = {v: k for k, v in named.items()}
    parsed = {}
    plain = []
    for addr, (path, lines) in bodies.items():
        nodes = lds.parse_function(lines)
        chunks = dcd.chunks_of(lines)
        dcd.fix_named_calls(nodes, chunks, named, hooked)
        parsed[addr] = (nodes, instructions(lines), chunks)
        if any(re.match(r'^\tctx\.lr = (?:ctx\.)?r\d+\.u64;', l) for l in lines):
            plain.append(names.get(addr, 'sub_%08X' % addr))
    # peggle2-nx: a function a game patch changed is opaque: it does not preserve ctx.lr, and none of its sites is
    # proven (its graph is not the parser's: tools/leaf_dead_stores.py, OPAQUE_MARK).
    opaque = {addr for addr, (path, lines) in bodies.items() if lds.is_opaque(lines)}
    pres = set(parsed) - opaque
    callers = collections.defaultdict(set)
    for addr, (nodes, _, _) in parsed.items():
        for nd in nodes:
            if nd.call:
                callers[nd.call[0]].add(addr)
    work = set(parsed)
    while work:
        addr = work.pop()
        if addr not in pres:
            continue
        ok, _, _ = flow(*parsed[addr], lambda x: x in pres, a.assume_indirect)
        if not ok:
            pres.discard(addr)
            work |= callers[addr]
    proven, unproven = {}, {}
    np = nu = 0
    for addr in parsed:
        if addr in opaque:
            continue
        _, p, u = flow(*parsed[addr], lambda x: x in pres, a.assume_indirect)
        nm = names.get(addr, 'sub_%08X' % addr)
        if p:
            proven[nm] = p
            np += len(p)
        if u:
            unproven[nm] = u
            nu += len(u)
    json.dump({'preserving': sorted(names.get(x, 'sub_%08X' % x) for x in pres), 'proven_sites': proven,
               'unproven_sites': unproven, 'plain_writers': plain}, open(os.path.join(a.out, 'lr_preserve.json'), 'w'))
    print('lr_preserve: %d functions, %d preserve ctx.lr; REX_LR_WRITE sites proven %d, not %d; %d functions with a '
          'plain ctx.lr = register write' % (len(parsed), len(pres), np, nu, len(plain)))
    if a.rewrite:
        if a.only_file:
            keep = set(json.load(open(a.only_file)))
            proven = {k: v for k, v in proven.items() if k in keep}
        rewrite(a.gen, bodies, names, proven)


MARK = '// cpu_aggressive: link-register write-backs proven to store the value ctx.lr holds (lr_preserve.py)'
MACROS = MARK + '''
#ifndef REX_LR_WRITE_SAME
#if defined(REX_LR_WRITE_SAME_OFF)
#define REX_LR_WRITE_SAME(reg) REX_LR_WRITE(reg)
#elif defined(REX_LR_WRITE_SAME_CHECK)
extern "C" void RexLrWriteMismatch(unsigned long long held, unsigned long long written) noexcept;
extern "C" volatile unsigned long long g_rex_lr_write_checked;
#define REX_LR_WRITE_SAME(reg)                                              \\
  do {                                                                      \\
    g_rex_lr_write_checked = g_rex_lr_write_checked + 1;                    \\
    if (ctx.lr != (reg).u64) [[unlikely]] RexLrWriteMismatch(ctx.lr, (reg).u64); \\
    REX_LR_WRITE(reg);                                                      \\
  } while (0)
#else
#define REX_LR_WRITE_SAME(reg) ((void)0)
#endif
#endif
'''


def rewrite(gen, bodies, names, proven):
    """REX_LR_WRITE(reg) -> REX_LR_WRITE_SAME(reg) at the proven sites; the macros into the pch."""
    by_file = collections.defaultdict(list)
    for addr, (path, lines) in bodies.items():
        nm = names.get(addr, 'sub_%08X' % addr)
        if nm in proven:
            by_file[path].append((nm, set(proven[nm])))
    done = 0
    for path, items in by_file.items():
        text = open(path, encoding='utf-8').read().split('\n')
        want = dict(items)
        i = 0
        changed = False
        while i < len(text):
            m = re.match(r'^DEFINE_REX_FUNC\((?:__imp__)?(\w+)\) \{$', text[i])
            if not m or m.group(1) not in want:
                i += 1
                continue
            sites = want[m.group(1)]
            for k in sites:
                j = i + 1 + k
                if re.match(r'^\tREX_LR_WRITE\(((?:ctx\.)?r\d+)\);$', text[j]):
                    text[j] = text[j].replace('REX_LR_WRITE(', 'REX_LR_WRITE_SAME(')
                    done += 1
                    changed = True
            i += 1
        if changed:
            open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(text))
    pch = [f for f in os.listdir(gen) if f.endswith('_pch.h')][0]
    p = os.path.join(gen, pch)
    t = open(p, encoding='utf-8').read()
    if MARK not in t:
        anchor = '// rexglue ports: registers as locals for the length of a call-free loop (tools/loop_locals.py)'
        k = t.index(anchor)
        open(p, 'w', encoding='utf-8', newline='\n').write(t[:k] + MACROS + t[k:])
    print('lr_preserve: %d REX_LR_WRITE sites rewritten as REX_LR_WRITE_SAME' % done)


if __name__ == '__main__':
    main()
