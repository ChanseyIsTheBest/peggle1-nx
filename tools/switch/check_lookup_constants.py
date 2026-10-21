#!/usr/bin/env python3
"""Data-flow check of a recompiled function compiled alone (the .s file of tools/switch/compile_one_function.py): does
every table lookup get the constant it needs? The console fault of 2026-10-13 was exactly that: in the game's
streaming copies (sub_8260E9C0, sub_8260E164) the register holding the byte-reversal constant, and the 0x1F / 0x0F
constants of the permute index, were given to data loads inside the loop by GCC's early register allocation pass
(docs/peggle1-notes.md, "Console: large atlases").

A forward data-flow over the function's control-flow graph tags every vector register and every stack / context slot:
  REV   the byte-reversal constant (loaded from the literal with bytes 15..0)
  C1F   movi 0x1f        C0F  movi 0xf        ZERO  movi 0
  LVSL  a row of rex::ppc::VectorShiftTableL (the lvsl result)
  AND   LVSL & C1F       IDX  AND ^ C0F  (the index of the two-register lookup)
  X     anything else (data, or different things on different paths)
and then checks every lookup:
  tbl vD, {vT}, vI            (lvx128's byte reversal)     needs I = REV
  tbl vD, {vA - vB}, vI       (vperm128 after an lvsl)      needs I = IDX
It is written for functions whose one-register lookups are all byte reversals and whose permutes all take an lvsl
result (the streaming copies); elsewhere a report is not a fault by itself. Exit code 1 if any lookup is reported.

  python tools/switch/check_lookup_constants.py out/nx/one_function/sub_8260E9C0_build.s [...]
"""
import re
import sys

TOP = None   # not reached yet
X = 'X'
BRANCH_COND = {'beq', 'bne', 'bcs', 'bhs', 'bcc', 'blo', 'bmi', 'bpl', 'bvs', 'bvc', 'bhi', 'bls', 'bge', 'blt',
               'bgt', 'ble', 'cbz', 'cbnz', 'tbz', 'tbnz'}
NO_DEST = {'str', 'stp', 'strb', 'strh', 'stur', 'st1', 'st2', 'cmp', 'cmn', 'tst', 'ccmp', 'ccmn', 'fcmp', 'fcmpe',
           'prfm', 'nop', 'ret', 'b', 'br', 'bl', 'blr'} | BRANCH_COND


def vreg(tok):
    """'v30.16b' / 'q30' / 'd30' / 's30' -> 30, else None."""
    m = re.fullmatch(r'[vqdshb](\d+)(?:\.\w+)?(?:\[\d+\])?', tok.strip())
    return int(m.group(1)) if m else None


def gpr(tok):
    m = re.fullmatch(r'[xw](\d+)', tok.strip())
    return int(m.group(1)) if m else None


def split_ops(s):
    """Operands split at top-level commas."""
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '[{':
            depth += 1
        elif ch in ']}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def mem(op):
    """'[x0, 592]' -> ('x0', 592); '[sp, 16]' -> ('sp', 16); '[x0]' -> ('x0', 0); other forms -> (base, None)."""
    m = re.match(r'\[(\w+)(?:,\s*#?(-?\d+))?\]$', op)
    if m:
        return m.group(1), int(m.group(2) or 0)
    m = re.match(r'\[(\w+)', op)
    return (m.group(1), None) if m else (None, None)


def meet(a, b):
    if a is TOP:
        return b
    if b is TOP:
        return a
    return a if a == b else X


def analyse(path):
    lines = open(path).read().splitlines()
    text = '\n'.join(lines)
    # The reversal literal: a label followed by the bytes 15..0.
    rev_labels = set()
    for m in re.finditer(r'^(\.LC\d+):\n((?:\t\.byte\t\d+\n){16})', text, re.M):
        if [int(x) for x in re.findall(r'\.byte\t(\d+)', m.group(2))] == list(range(15, -1, -1)):
            rev_labels.add(m.group(1))
    anchors = dict(re.findall(r'^\t\.set\t(\.LANCHOR\d+),\. \+ (\d+)$', text, re.M))
    for a, off in anchors.items():
        # .set .LANCHORn,. + 0 directly before a reversal literal
        i = text.index('\t.set\t%s,. + %s' % (a, off))
        rest = text[i:].split('\n', 2)
        if off == '0' and len(rest) > 1 and rest[1].rstrip(':') in rev_labels:
            rev_labels.add(a)
    # Instructions and labels of the function.
    start = next(i for i, l in enumerate(lines) if re.match(r'^(_Z\w*sub_|__imp__sub_|sub_)\w*:$', l))
    ins, label_at = [], {}
    for l in lines[start + 1:]:
        if l.startswith('\t.size') or l.startswith('\t.cfi_endproc'):
            break
        m = re.match(r'^(\.L\w+):$', l)
        if m:
            label_at[m.group(1)] = len(ins)
        elif l.startswith('\t') and not l.startswith('\t.'):
            parts = l.strip().split('\t', 1)
            ins.append((parts[0], split_ops(parts[1]) if len(parts) > 1 else []))
    n = len(ins)
    succ = [[] for _ in range(n)]
    for i, (op, ops) in enumerate(ins):
        if op == 'ret' or op == 'br':
            continue
        if op == 'b':
            succ[i].append(label_at[ops[0]])
            continue
        if op in BRANCH_COND or (op.startswith('b.') and len(ops) == 1):
            succ[i].append(label_at[ops[-1]])
        if i + 1 < n:
            succ[i].append(i + 1)

    def transfer(i, state):
        v, cells, g = dict(state[0]), dict(state[1]), dict(state[2])
        op, ops = ins[i]
        problems = []
        if op in ('str', 'stp', 'stur') and ops and vreg(ops[0]) is not None:
            srcs = [vreg(o) for o in ops[:-1]]
            base, off = mem(ops[-1])
            if base in ('sp', 'x0') and off is not None and ops[-1].endswith(']'):
                for k, s in enumerate(srcs):
                    cells[(base, off + 16 * k)] = v.get(s, X)
            elif base in ('sp', 'x0') or g.get(base) in ('SPPTR', 'CTXPTR'):
                cells = {}   # a store through a slot pointer this script does not follow
        elif op in ('str', 'stp', 'stur', 'strb', 'strh'):
            base, off = mem(ops[-1])
            if g.get(base) in ('SPPTR', 'CTXPTR'):
                cells = {}
        elif op in ('ldr', 'ldp', 'ldur') and ops and vreg(ops[0]) is not None:
            dsts = [vreg(o) for o in ops[:-1]] if op == 'ldp' else [vreg(ops[0])]
            addr = ops[-1] if op == 'ldp' else ops[1]
            base, off = mem(addr)
            lit = re.search(r'lo12:(\.\w+)', addr)
            for k, d in enumerate(dsts):
                if lit and lit.group(1) in rev_labels:
                    v[d] = 'REV'
                elif base in ('sp', 'x0') and off is not None:
                    v[d] = cells.get((base, off + 16 * k), X)
                elif g.get(base) == 'VST':
                    v[d] = 'LVSL'
                else:
                    v[d] = X
        elif op == 'movi' and vreg(ops[0]) is not None:
            d = vreg(ops[0])
            val = ops[1].lstrip('#')
            lanes = ops[0].split('.')[-1]
            v[d] = ('C1F' if val == '0x1f' and lanes == '16b' else 'C0F' if val == '0xf' and lanes == '16b'
                    else 'ZERO' if val in ('0', '0x0') else X)
        elif op == 'mov' and len(ops) == 2 and vreg(ops[0]) is not None and vreg(ops[1]) is not None \
                and '[' not in ops[0]:
            v[vreg(ops[0])] = v.get(vreg(ops[1]), X)
        elif op in ('and', 'eor') and vreg(ops[0]) is not None:
            a, b = v.get(vreg(ops[1]), X), v.get(vreg(ops[2]), X)
            want = ('LVSL', 'C1F', 'AND') if op == 'and' else ('AND', 'C0F', 'IDX')
            v[vreg(ops[0])] = want[2] if {a, b} == {want[0], want[1]} else X
        elif op == 'tbl':
            index = v.get(vreg(ops[-1]), X)
            two = ' - ' in ops[1] or ',' in ops[1]
            need = 'IDX' if two else 'REV'
            if index != need:
                problems.append((i, '%s %s: the index register holds %s, not %s' % (op, ', '.join(ops), index, need)))
            v[vreg(ops[0])] = X
        elif op == 'bl' or op == 'blr':
            v = {}
            g = {}
        elif op not in NO_DEST and ops:
            d = vreg(ops[0])
            if d is not None:
                v[d] = X
                if op == 'ldp' and len(ops) > 2 and vreg(ops[1]) is not None:
                    v[vreg(ops[1])] = X
            r = gpr(ops[0])
            if r is not None:
                name = 'x%d' % r
                if op == 'adrp' and 'VectorShiftTableL' in ops[1]:
                    g[name] = 'VST'
                elif op == 'add' and len(ops) == 3 and 'VectorShiftTableL' in ops[2] and g.get(ops[1]) == 'VST':
                    g[name] = 'VST'
                elif op in ('add', 'sub', 'mov') and len(ops) >= 2 and ops[1] in ('sp', 'x0'):
                    g[name] = 'SPPTR' if ops[1] == 'sp' else 'CTXPTR'
                else:
                    g.pop(name, None)
                if op == 'ldp' and len(ops) > 2 and gpr(ops[1]) is not None:
                    g.pop('x%d' % gpr(ops[1]), None)
        # Writeback addressing changes its base register.
        for o in ops:
            m = re.match(r'\[(x\d+)[^\]]*\]!', o)
            if m:
                g.pop(m.group(1), None)
        if len(ops) >= 2 and re.fullmatch(r'#?-?\d+', ops[-1]) and re.fullmatch(r'\[(x\d+)\]', ops[-2] or ''):
            g.pop(ops[-2][1:-1], None)
        return (v, cells, g), problems

    def meet_state(a, b):
        if a is TOP:
            return b
        out = []
        for da, db in zip(a, b):
            keys = set(da) | set(db)
            out.append({k: meet(da.get(k, X), db.get(k, X)) for k in keys if meet(da.get(k, X), db.get(k, X)) != X})
        return tuple(out)

    state_in = [TOP] * n
    state_in[0] = ({}, {}, {})
    work = [0]
    while work:
        i = work.pop()
        out, _ = transfer(i, state_in[i])
        for s in succ[i]:
            merged = meet_state(state_in[s], out)
            if merged != state_in[s]:
                state_in[s] = merged
                work.append(s)
    problems, one, two = [], 0, 0
    for i in range(n):
        if state_in[i] is TOP:
            continue
        if ins[i][0] == 'tbl':
            if ' - ' in ins[i][1][1] or ',' in ins[i][1][1]:
                two += 1
            else:
                one += 1
        problems += transfer(i, state_in[i])[1]
    name = path.replace('\\', '/').split('/')[-1]
    print('%-46s %5d instructions | lookups checked: %3d byte reversals, %3d two-register | wrong index: %d' % (
        name, n, one, two, len(problems)))
    for i, text_ in problems[:4]:
        print('      instruction %5d: %s' % (i, text_))
    if not rev_labels:
        print('      (no reversal literal found in this file)')
    return len(problems)


if __name__ == '__main__':
    sys.exit(1 if sum(analyse(p) for p in sys.argv[1:]) else 0)
