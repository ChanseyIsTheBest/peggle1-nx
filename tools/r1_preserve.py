#!/usr/bin/env python3
"""cpu_aggressive: which guest functions of Banjo's executable provably return with r1 (the guest stack pointer) equal
to its value at entry, from the instruction comments of a generated tree.
(peggle2-nx's copy, 2026-10-19: no no-return addresses of Banjo's; the functions Peggle's game patches change do
not count as preserving: tools/leaf_dead_stores.py, OPAQUE_MARK.)

Per function, a forward data flow over parse_function's control-flow graph (tools/leaf_dead_stores.py) of
  d       r1 minus r1-at-entry (None: unknown),
  chain   the d of the value the last stwu / stwux stored at r1 + 0 (the back chain), None if unknown or overwritten,
  regs    registers holding r1-at-entry + c (mr rX,r1 / addi rX,r1,c), for frame pointers.
Instructions: stwu/stdu r1,-N(r1); stwux/stdux r1,r1,rX (d unknown, chain = old d); addi r1,r1,N; lwz/ld r1,0(r1)
(d = chain); mr r1,rX / addi r1,rX,c (d from regs); any other write of r1: unknown. A store with base r1 at offset
0 overwrites the chain word (chain unknown). A call: to a function not (yet) proven, or an unknown callee, makes d
unknown after it ONLY for the proof of that function (the transform reloads after such calls).

A function PRESERVES r1 when d == 0 at every exit (a return, falling off the end, or a tail call: then the callee
must preserve too). Greatest fixed point over the call graph (sound for this partial-correctness property: by
induction on the call depth of any finished call).

  python r1_preserve.py --gen <tree> --out <dir> [--indirect-preserve]
--indirect-preserve: an indirect call, an import or a hooked call counts as preserving (the assumption the PC check
REX_R1_CHECK verifies); without it they make d unknown.
Writes <dir>/r1_preserve.json: {"preserving": [names], "not": {name: reason}}.
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
# no-return functions (tools/non_argument_audit.py's list in docs/codegen-enhancements.md)
# peggle2-nx: Banjo's addresses do not apply (a function with no exit is found by the flow itself); the names stay.
NORETURN = ['rex_longjmp', 'rex_exit', 'rex_j_std___Throw', 'rex_std___Throw', 'rex_abort', 'rex_amsg_exit',
            'rex_exit_1', 'rex_ExitThread']


def instructions(lines):
    out = []
    for line in lds.resolve_stubs(lines):
        m = lds.COMMENT.match(line)
        if m:
            out.append((m.group(1), m.group(2).strip()))
    return out


def flow(nodes, ins, chunks, delta_of, indirect_ok):
    """(delta, reason): the d of every exit when they agree (None: unknown / disagree; 'noreturn': no exit)."""
    n = len(nodes)
    if n == 0:
        return 0, ''
    if len(ins) != n:
        return None, 'misaligned'
    exits = set()
    state = [None] * n
    state[0] = (0, None, (('r1', 0),))
    work = [0]
    while work:
        i = work.pop()
        d, chain, regs = state[i]
        regs = dict(regs)
        mn, ops = ins[i]
        b = mn.rstrip('.')
        o = [x.strip() for x in ops.split(',')] if ops else []
        node = nodes[i]
        text = ' '.join(t for _, t in chunks[i][1])
        # stores at 0(r1) overwrite the chain word
        if b.startswith('st') and len(o) > 1:
            m = DFORM.match(o[1])
            if m and m.group(2) == 'r1' and int(m.group(1)) == 0 and b not in ('stwu', 'stdu'):
                chain = None
        if b in ('stwu', 'stdu') and o and o[0] == 'r1':
            m = DFORM.match(o[1])
            if m and m.group(2) == 'r1' and d is not None:
                chain, d = d, d + int(m.group(1))
            else:
                chain, d = None, None
        elif b in ('stwux', 'stdux') and o and o[0] == 'r1':
            chain, d = d, None
        elif b in ('addi', 'la') and len(o) == 3 and o[0] == 'r1':
            if o[1] == 'r1':
                d = d + int(o[2]) if d is not None else None
            else:
                v = regs.get(o[1])
                d = v + int(o[2]) if v is not None else None
        elif b in ('lwz', 'ld') and o and o[0] == 'r1':
            m = DFORM.match(o[1])
            d = chain if (m and m.group(2) == 'r1' and int(m.group(1)) == 0) else None
            chain = None
        elif b == 'mr' and len(o) == 2 and o[0] == 'r1':
            d = regs.get(o[1])
        elif node.defn & lds.R(1):
            d = None
        # registers holding r1-at-entry + c
        newv = None
        if b in ('addi', 'la') and len(o) == 3 and o[0] != 'r1' and o[1] == 'r1' and d is not None:
            newv = (o[0], d + int(o[2]))
        elif b == 'mr' and len(o) == 2 and o[0] != 'r1' and o[1] == 'r1' and d is not None:
            newv = (o[0], d)
        for r in list(regs):
            if r != 'r1' and node.defn & lds.R(int(r[1:])):
                del regs[r]
        if newv:
            regs[newv[0]] = newv[1]
        regs['r1'] = d if d is not None else None
        if regs['r1'] is None:
            del regs['r1']
        # calls
        called = node.call or 'REX_CALL_INDIRECT' in text or 'REX_PROMOTED' in text
        if not called and node.use == lds.ALL and b.startswith('b'):
            called = True
        dead_end = False
        if called:
            delta = None
            if node.call and node.call[1]:
                delta = delta_of(node.call[0])
            elif indirect_ok:
                delta = 0
            if delta == 'noreturn':
                dead_end = True
            elif delta is None or d is None:
                d = None
            else:
                d = d + delta
            for r in list(regs):
                if r not in ('r1',) and (r == 'r0' or 3 <= int(r[1:]) <= 12):
                    del regs[r]
        if dead_end:
            continue
        if node.exit:
            exits.add(d)
            if d is None:
                return None, 'd unknown at #%d (%s %s)' % (i, mn, ops)
            if len(exits) > 1:
                return None, 'exits disagree %s at #%d (%s %s)' % (sorted(exits), i, mn, ops)
        st = (d, chain, tuple(sorted(regs.items())))
        for s in node.succ:
            old = state[s]
            if old is None:
                new = st
            else:
                nd = old[0] if old[0] == st[0] else None
                nc = old[1] if old[1] == st[1] else None
                nr = tuple(sorted(set(old[2]) & set(st[2])))
                new = (nd, nc, nr)
            if new != old:
                state[s] = new
                work.append(s)
    if not exits:
        return 'noreturn', ''
    return exits.pop(), ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--indirect-preserve', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    lds.load_mid_asm_hooks(a.gen)
    named = dcd.named_functions(a.gen)
    hooked_named = dcd.hooked_names(set(named))
    bodies = dcd.all_functions(a.gen, named)
    names = {v: k for k, v in named.items()}
    parsed = {}
    for addr, (path, lines) in bodies.items():
        nodes = lds.parse_function(lines)
        chunks = dcd.chunks_of(lines)
        dcd.fix_named_calls(nodes, chunks, named, hooked_named)
        # peggle2-nx: a function a game patch changed counts as one with setjmp: not preserving (OPAQUE_MARK)
        parsed[addr] = (nodes, instructions(lines), chunks, any('ppc_setjmp' in l or 'longjmp' in l for l in lines) or
                        lds.is_opaque(lines))
    noret = set(int(x, 16) for x in NORETURN if re.fullmatch(r'[0-9A-F]{8}', x)) |         set(named[x] for x in NORETURN if x in named)
    delta = {addr: ('noreturn' if addr in noret else 0) for addr in parsed}
    reason = {}
    callers = collections.defaultdict(set)
    for addr, (nodes, _, _, _) in parsed.items():
        for nd in nodes:
            if nd.call:
                callers[nd.call[0]].add(addr)
    work = set(parsed)
    flips = collections.Counter()
    rounds = 0
    while work:
        rounds += 1
        addr = work.pop()
        if addr in noret:
            continue
        nodes, ins, chunks, sj = parsed[addr]
        dl, why = flow(nodes, ins, chunks, lambda x: delta.get(x), a.indirect_preserve)
        if sj:
            dl, why = None, 'setjmp / longjmp'
        if flips[addr] > 6:
            dl, why = None, 'unstable'
        if dl != delta[addr]:
            flips[addr] += 1
            delta[addr] = dl
            if why:
                reason[addr] = why
            else:
                reason.pop(addr, None)
            work |= callers[addr]
    preserving = {x for x, v in delta.items() if v == 0 or v == 'noreturn'}
    out = {'preserving': sorted(names.get(x, 'sub_%08X' % x) for x in preserving),
           'delta': {names.get(x, 'sub_%08X' % x): v for x, v in sorted(delta.items()) if v not in (0,)},
           'not': {names.get(x, 'sub_%08X' % x): r for x, r in sorted(reason.items()) if delta[x] is None}}
    reason = {x: r for x, r in reason.items() if delta[x] is None}
    reason.update({x: 'delta %s' % delta[x] for x in delta if isinstance(delta[x], int) and delta[x] != 0})
    json.dump(out, open(os.path.join(a.out, 'r1_preserve.json'), 'w'), indent=0)
    kinds = collections.Counter(r.split(' at ')[0] for r in reason.values())
    print('r1_preserve: %d functions, %d preserve r1 (%d rounds); not: %d %s' % (
        len(parsed), len(preserving), rounds, len(reason), dict(kinds)))


if __name__ == '__main__':
    main()
