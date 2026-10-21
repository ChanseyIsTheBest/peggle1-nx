#!/usr/bin/env python3
"""(peggle2-nx's copy of banjo-nx's tool, 2026-10-19: compile switch PEGGLE1_GEN_RO_CFG; the functions Peggle's game
patches change are not analysed: tools/leaf_dead_stores.py, OPAQUE_MARK. banjo-nx's text:)
banjo-nx (2026-10-16): read-only constants found over a function's control-flow graph (post-codegen step, after
tools/vec_unaligned_stores.py; docs/codegen-enhancements.md). From the estimate of the CPU codegen agent
(out/cpu_proposals/ro_cfg_estimate.py). Works on any ReXGlue port.

tools/ro_constants.py follows the constants `lis` / `addi` / `ori` put in registers along each straight line of a
function and forgets them at every label. Most of the game's float literals are loaded inside loops through a base
register set before the loop, so the label of the loop's head made them unknown: 3,253 more loads from read-only data
(3,114 of them `lfs`) are foldable when the constants are followed over the control-flow graph.

Per function, on its guest instructions (tools/leaf_dead_stores.py's parser: the instruction comments, their
registers and the C++ control flow between them), a forward constant propagation:
  - lis / li / addi / addis (r0 as the base is the literal 0) / ori / mr give a known 64-bit value when their inputs
    are known; any other write of a register forgets it (the parser's register table);
  - a direct call forgets r0-r13 and every register the function keeps in ctx (the callee can write those; the
    registers the generator made C++ locals of this function cannot be reached by a callee);
  - an instruction the parser does not know, an indirect call, a trap or a mid-asm hook forgets everything, and no
    load is folded at an instruction with a mid-asm hook;
  - where paths meet, a register stays known only with the same value on every path; the entry knows nothing.
A D-form load (lfs lfd lwz lhz lha lbz ld lwa; not the update forms, not r0 as the base) whose address is then known
and lies wholly inside the image's .rdata, and not on an import record of the XEX (their words are written by the
loader), is written as REX_RO_CFG_<kind>(address, value, <the load as it was>):
  - on the Switch: the value (the compiler folds the conversion behind it, as with tools/ro_constants.py);
  - on the PC: the load as before, compared with the value (RexRoCheck: a difference is reported, and counted with
    the other folded constants): a PC run tests every fold it reaches, the address as well as the bytes;
  - with REX_RO_CFG_OFF (CMake PEGGLE1_GEN_RO_CFG=OFF) or REX_RO_CONSTANTS_OFF: the load as it was.
A load already folded (REX_RO_ in its statements) is left alone, so the step is idempotent.

  python tools/ro_constants_cfg.py --gen app/generated/default --image out/default_image.bin
                                   [--xex assets/game_root/default.xex]
                                   [--check]
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import leaf_dead_stores as ld  # noqa: E402
import ro_constants as ro  # noqa: E402

FUNC = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
LOADS = {'lfs': 4, 'lfd': 8, 'lwz': 4, 'lhz': 2, 'lha': 2, 'lbz': 1, 'ld': 8, 'lwa': 4}
KIND_SIZE = {'U8': 1, 'U16': 2, 'U32': 4, 'U64': 8, 'F32': 4}
LOAD_EXPR = re.compile(r'REX_LOAD_D_(U8|U16|U32|U64|F32)\(([\w.]+)\.u32, (-?\d+)\)')
DFORM = re.compile(r'^(\w+),(-?\d+)\((r\d+)\)$')
CTX_REG = re.compile(r'\bctx\.r(\d+)\b')
M64 = (1 << 64) - 1
MARKER = '// rexglue ports: read-only constants found over the control-flow graph (tools/ro_constants_cfg.py)'
MACROS = MARKER + '''
#ifndef REX_RO_CFG_U32
#if defined(REX_RO_CFG_OFF) || defined(REX_RO_CONSTANTS_OFF)
#define REX_RO_CFG_U8(a, v, load) (load)
#define REX_RO_CFG_U16(a, v, load) (load)
#define REX_RO_CFG_U32(a, v, load) (load)
#define REX_RO_CFG_U64(a, v, load) (load)
#define REX_RO_CFG_F32(a, v, load) (load)
#elif defined(__SWITCH__)
#define REX_RO_CFG_U8(a, v, load) ((u8)(v))
#define REX_RO_CFG_U16(a, v, load) ((u16)(v))
#define REX_RO_CFG_U32(a, v, load) ((u32)(v))
#define REX_RO_CFG_U64(a, v, load) ((u64)(v))
#define REX_RO_CFG_F32(a, v, load) std::bit_cast<float>((u32)(v))
#else
#define REX_RO_CFG_U8(a, v, load) RexRoCheck<u8>((u8)(load), (u8)(v), (a))
#define REX_RO_CFG_U16(a, v, load) RexRoCheck<u16>((u16)(load), (u16)(v), (a))
#define REX_RO_CFG_U32(a, v, load) RexRoCheck<u32>((u32)(load), (u32)(v), (a))
#define REX_RO_CFG_U64(a, v, load) RexRoCheck<u64>((u64)(load), (u64)(v), (a))
#define REX_RO_CFG_F32(a, v, load) std::bit_cast<float>(RexRoCheck<u32>(std::bit_cast<u32>(load), (u32)(v), (a)))
#endif
#endif
'''


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def node_chunks(res):
    """For each guest instruction comment of the resolved lines: (mnemonic, operands) and [(line index, text)]."""
    comments, chunks = [], []
    for li, line in enumerate(res):
        if ld.LABEL.match(line):
            continue
        m = ld.COMMENT.match(line)
        if m:
            comments.append((m.group(1), m.group(2)))
            chunks.append([])
        elif chunks and line.strip():
            chunks[-1].append((li, line))
    return comments, chunks


def has_hook(chunk):
    for _, line in chunk:
        h = ld.HOOK_CALL.match(line.strip())
        if h and h.group(1) in ld.MID_ASM_HOOKS:
            return True
    return False


def propagate(nodes, comments, chunks, ctx_regs):
    """The known general registers at the entry of every instruction (None: not reached)."""
    n = len(nodes)
    preds = [[] for _ in range(n)]
    for k, nd in enumerate(nodes):
        for s in nd.succ:
            preds[s].append(k)
    hooked = [has_hook(chunks[k]) for k in range(n)]
    call_kills = {'r%d' % r for r in range(14)} | {'r%d' % r for r in ctx_regs}

    def transfer(k, st):
        nd = nodes[k]
        if nd.use == ld.ALL or hooked[k]:
            return {}
        st = dict(st)
        mn, ops = comments[k]
        mn = mn.rstrip('.')
        o = [x.strip() for x in ops.split(',')] if ops else []
        dst = val = None
        try:
            if mn == 'lis' and len(o) == 2:
                dst, val = o[0], (sx16(int(o[1], 0) & 0xFFFF) << 16) & M64
            elif mn == 'li' and len(o) == 2:
                dst, val = o[0], sx16(int(o[1], 0) & 0xFFFF) & M64
            elif mn in ('addi', 'addis') and len(o) == 3:
                imm = sx16(int(o[2], 0) & 0xFFFF)
                imm = imm << 16 if mn == 'addis' else imm
                src = 0 if o[1] == 'r0' else st.get(o[1])
                dst, val = o[0], None if src is None else (src + imm) & M64
            elif mn == 'ori' and len(o) == 3:
                src = st.get(o[1])
                dst, val = o[0], None if src is None else src | (int(o[2], 0) & 0xFFFF)
            elif mn == 'mr' and len(o) == 2:
                dst, val = o[0], st.get(o[1])
        except ValueError:
            dst = val = None
        if nd.call is not None:
            for r in call_kills:
                st.pop(r, None)
        for b in range(32):
            if nd.defn & ld.R(b):
                st.pop('r%d' % b, None)
        if dst is not None and val is not None and re.match(r'^r\d+$', dst):
            st[dst] = val
        return st

    state_in = [None] * n
    state_out = [None] * n
    work = list(range(n - 1, -1, -1))
    inwork = set(work)
    while work:
        k = work.pop()
        inwork.discard(k)
        if k == 0:
            sin = {}
        else:
            ps = [state_out[p] for p in preds[k] if state_out[p] is not None]
            if not ps:
                continue
            sin = dict(ps[0])
            for p in ps[1:]:
                sin = {r: v for r, v in sin.items() if p.get(r) == v}
            if not preds[k]:
                sin = {}
        state_in[k] = sin
        out = transfer(k, sin)
        if out != state_out[k]:
            state_out[k] = out
            for s in nodes[k].succ:
                if s not in inwork:
                    work.append(s)
                    inwork.add(s)
    return state_in, hooked


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--xex')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    if not os.path.exists(a.image):
        sys.exit('ERROR (post-codegen patch): ro_constants_cfg: %s not found' % a.image)
    image = open(a.image, 'rb').read()
    ranges, base = ro.read_only_ranges(image)
    records = ro.import_records(a.xex) if a.xex and os.path.isfile(a.xex) else set()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): ro_constants_cfg: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    pch_path = os.path.join(a.gen, pchs[0])
    pch = open(pch_path, encoding='utf-8').read()
    m = re.search(r'#define REX_IMAGE_BASE (0x[0-9A-Fa-f]+)', pch)
    s = re.search(r'#define REX_IMAGE_SIZE (0x[0-9A-Fa-f]+)', pch)
    if not m or not s or int(m.group(1), 16) != base or int(s.group(1), 16) != len(image) or not ranges:
        sys.exit('ERROR (post-codegen patch): ro_constants_cfg: %s is not the image this code was generated from'
                 % a.image)
    if ro.MARKER not in pch:
        sys.exit('ERROR (post-codegen patch): ro_constants_cfg: run tools/ro_constants.py first (RexRoCheck)')
    ld.load_mid_asm_hooks(a.gen)

    def readonly(addr, size):
        if any(((addr + k) & ~3) in records for k in range(size)):
            return False
        return any(st <= addr and addr + size <= en for st, en in ranges)

    stats = dict(folded=0, already=0, skipped_functions=0, changed=0)
    by_kind = {}
    for f in files:
        path = os.path.join(a.gen, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        dirty = False
        i = 0
        while i < len(lines):
            fm = FUNC.match(lines[i])
            if not fm:
                i += 1
                continue
            j = i + 1
            while j < len(lines) and lines[j] != '}':
                j += 1
            body = lines[i + 1:j]
            body_start = i + 1
            i = j + 1
            if ld.is_opaque(body):
                stats['skipped_functions'] += 1   # peggle2-nx: a game patch changed it
                continue
            try:
                nodes = ld.parse_function(body)
            except Exception:
                stats['skipped_functions'] += 1
                continue
            res = ld.resolve_stubs(body)
            if len(res) != len(body):
                stats['skipped_functions'] += 1
                continue
            comments, chunks = node_chunks(res)
            if len(comments) != len(nodes) or not nodes:
                stats['skipped_functions'] += 1
                continue
            ctx_regs = {int(r) for r in CTX_REG.findall('\n'.join(body)) if 14 <= int(r) <= 31}
            state_in, hooked = propagate(nodes, comments, chunks, ctx_regs)
            for k, (mn, ops) in enumerate(comments):
                if mn not in LOADS or state_in[k] is None or hooked[k]:
                    continue
                dm = DFORM.match(ops)
                if not dm or dm.group(3) == 'r0':
                    continue
                known = state_in[k].get(dm.group(3))
                if known is None:
                    continue
                size = LOADS[mn]
                addr = (known + int(dm.group(2))) & 0xFFFFFFFF
                if not readonly(addr, size):
                    continue
                text = ' '.join(t for _, t in chunks[k])
                if 'REX_RO_' in text:
                    stats['already'] += 1
                    continue
                reg_name = re.compile(r'^(?:ctx\.)?(?:ll_|l_)?%s$' % dm.group(3))
                hits = [(li, lm) for li, line in chunks[k] for lm in LOAD_EXPR.finditer(line)
                        if int(lm.group(3)) == int(dm.group(2)) and reg_name.match(lm.group(2))]
                if len(hits) != 1 or KIND_SIZE[hits[0][1].group(1)] != size:
                    continue
                li, lm = hits[0]
                kind = lm.group(1)
                value = int.from_bytes(image[addr - base:addr - base + size], 'big')
                suffix = 'ull' if kind == 'U64' else 'u'
                line = lines[body_start + li]
                new = '%sREX_RO_CFG_%s(0x%08X, 0x%X%s, %s)%s' % (line[:lm.start()], kind, addr, value, suffix,
                                                                lm.group(0), line[lm.end():])
                lines[body_start + li] = new
                dirty = True
                stats['folded'] += 1
                by_kind[mn] = by_kind.get(mn, 0) + 1
        if dirty:
            stats['changed'] += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    if MARKER not in pch:
        anchor = '#define REX_MEMORY_SIZE'
        if anchor not in pch:
            sys.exit('ERROR (post-codegen patch): ro_constants_cfg: no REX_MEMORY_SIZE in %s' % pch_path)
        if not a.check:
            open(pch_path, 'w', encoding='utf-8', newline='\n').write(pch.replace(anchor, MACROS + anchor, 1))
    print('ro_constants_cfg%s: %d more loads from read-only data folded over the control-flow graph (%s); %d already '
          'folded; %d functions not analysed; %d files changed' % (
              ' (check only)' if a.check else '', stats['folded'],
              ', '.join('%s %d' % kv for kv in sorted(by_kind.items(), key=lambda kv: -kv[1])) or 'none',
              stats['already'], stats['skipped_functions'], stats['changed']))


if __name__ == '__main__':
    main()
