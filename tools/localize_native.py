#!/usr/bin/env python3
"""Writes "localized copies" of recompiled guest functions: native replacements made mechanically from the generated
C++ (app/generated/default), for the hooks of app/src/cpu/peggle1_hot_guest.cpp. From sa2-nx's tool of the same name
(UnleashedRecomp-NX's switch-localize.py idea), with additions for Peggle 2:

  - a loop barrier (PEGGLE1_LOOP_BARRIER(), a compiler-only memory clobber) at every loop header, i.e. every label a
    later `goto` jumps back to: the copy reads guest memory without `volatile`, and GCC -O3 hoists such loads out of
    loops (the Mushihimesama port's console hang). Straight-line code keeps the speed;
  - for a leaf function (no call), a second variant NativeLogged whose guest stores go through PEGGLE1_LOG_* macros
    that record the bytes they overwrite: the guard can then roll the native's writes back, run the recompiled
    original and compare every written byte and every register, without a hand-written list of write ranges;
  - the hook lines themselves (app/generated/hot/localized_hooks.inc), with the size and FNV-1a 64 hash of the guest
    code each copy was made from, so adding a function is one line in app/src/cpu/hot/localized.txt.

The copy runs exactly the same operations as the recompiled function, with two differences that make it faster:
  - the guest registers it reaches through the context (ctx.rN, fN, vN, crN, ctr, xer, reserved) live in C++ locals:
    loaded from ctx at entry and written back at every return, so callers see exactly the state the recompiled function
    leaves (the non-volatile registers are C++ locals of the generated code already: app/perf_overrides.toml);
  - guest memory is accessed without `volatile` (peggle1_hot_guest.cpp redefines the REX_LOAD/STORE macros).
ctx.lr, ctx.fpscr and ctx.msr stay in the context. Floating point: the app is compiled with -ffp-contract=off on both
compilers (app/CMakeLists.txt), like the recompiled code, so the copy rounds exactly as the original.

Calls (smart sync, sa2-nx round 3; key peggle1_cpu_natives_smart_sync): a call writes back and reloads only the localized
registers its callee can touch: the registers the callee's generated body mentions, together with everything its
callees mention, transitively (every generated function is indexed: out/localize_index.json; strongly connected
components of the call graph). Registers the callee cannot reach stay in locals and are written back at the next call
that can reach them, or at the return. A callee whose reach is unknown gets the full sync: indirect calls, traps,
imports and any name that is not a generated function, and functions overridden by an app hook other than the natives
(a native or a copy touches no more than its original). Each smart call site also keeps the full sync behind the key,
and in verify mode (peggle1_cpu_verify) it snapshots the context, makes the call and checks that the callee left every
localized register outside its reach untouched (a write there would mean the analysis missed it; reads are covered by
the same textual analysis).

  python tools/localize_native.py --list app/src/cpu/hot/localized.txt      (what codegen.ps1 and the build run)
  python tools/localize_native.py 825BE6A0 [--what "..."] [--sync full]      (one function, for a look)
"""
import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000
REG = re.compile(r'\bctx\.(r\d+|f\d+|v\d+|cr\d|ctr|xer|reserved)\b')
# Any call of the generated code's calling convention: a generated function (sub_X or __imp__sub_X), a helper, an
# import or a host function (group 1), an indirect call or a trap (no group 1).
CALL = re.compile(r'\b([A-Za-z_]\w*)\(ctx, base\);|\bREX_CALL_INDIRECT_FUNC(?:_AT)?\([^;]*\);|\bppc_trap\(ctx, base[^;]*\);')
# sa2-nx's post-codegen steps (2026-10-13). A leaf function rewritten by tools/leaf_locals.py no longer says
# ctx.<register>: its declarations name the registers. A promoted indirect call (tools/indirect_calls.py) names its
# target's hookable symbol in its PC self-check.
LEAF_REG = re.compile(r'\bREX_LEAF_LOCAL\(\w+, l_\w+, (\w+)\)')
# banjo-nx's codegen round (peggle2-nx, 2026-10-19): this tool now runs after it (tools/post_codegen.py), so a copy
# gets its rewrites too. Two of its forms are taken back to the generator's first:
#   - tools/inline_memcalls.py wraps a constant-size memcpy / memset call as REX_MEMCPY_CONST(n, sub_X(ctx, base));
#     (the helper reads its arguments from ctx and runs the call when it must). A copy keeps the plain call (sa2-nx's
#     and banjo-nx's rule: its registers are locals, synced around calls only), and the index sees the call;
#   - tools/vec_unaligned_stores.py writes stvlx / stvrx as REX_STVLX / REX_STVRX (stores through REX_RAW_ADDR that
#     the logged variant would not see): the byte loops again, whose REX_STORE_U8 it logs.
# The round's LATE rewrites run after this tool in tools/post_codegen.py; CMake's configure step can run it again on
# the final tree (the Switch cross build has no codegen step: the first Switch build of 2026-10-19 failed on copies
# that called tools/inline_leaves.py's inl_sub_X, which only the recomp files define). They are taken back out too,
# so a copy is the same text whenever it is made: REX_INL(sub_X)(ctx, base) (tools/inline_leaves.py),
# REX_LR_WRITE_SAME (tools/lr_preserve.py), the r1 mirror (tools/r1_local.py), REX_VSEL (tools/vsel_neon.py).
MEMCALL_WRAP = re.compile(r'\bREX_MEM(?:CPY|SET)_CONST\(\d+, ((?:__imp__)?sub_[0-9A-F]{8}\(ctx, base\))\);')
VEC_LEFT = re.compile(r'^(\t*)REX_STVLX\(ea, ([\w.]+)\);$', re.M)
VEC_RIGHT = re.compile(r'^(\t*)REX_STVRX\(ea, ([\w.]+)\);$', re.M)
INL_CALL = re.compile(r'\bREX_INL\((sub_[0-9A-F]+)\)\(ctx, base\);')
R1_SUFFIX = re.compile(r' REX_R1_(?:SYNC|KEEP|ASSUMED|RELOAD)\(\);')


def unvsel(text):
    """REX_VSEL(c, a, b) as simde's expression again (tools/vsel_neon.py --restore)."""
    if os.path.dirname(os.path.abspath(__file__)) not in sys.path:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import vsel_neon
    out, pos = [], 0
    while True:
        k = text.find('REX_VSEL(', pos)
        if k < 0:
            out.append(text[pos:])
            return ''.join(out)
        a, end = vsel_neon.split_args(text, k + len('REX_VSEL('))
        out.append(text[pos:k])
        out.append(f'{vsel_neon.HEAD}{a[0]}, {a[1]}), simde_mm_and_si128({a[0]}, {a[2]}))')
        pos = end


def plain(text):
    """The generator's form of the rewrites above (the late ones first: they were made last)."""
    if 'REX_INL(' in text:
        text = INL_CALL.sub(r'__imp__\1(ctx, base);', text)
    if 'REX_VSEL(' in text:
        text = unvsel(text)
    if 'REX_R1_' in text or 'r1l' in text:
        text = text.replace('\tREX_R1_LOCAL();\n', '').replace('\tREX_R1_LOCAL();', '')
        text = R1_SUFFIX.sub('', text)
        text = text.replace('r1l.', 'ctx.r1.').replace('r1l)', 'ctx.r1)').replace('r1l,', 'ctx.r1,')
    if 'REX_LR_WRITE_SAME(' in text:
        text = text.replace('REX_LR_WRITE_SAME(', 'REX_LR_WRITE(')
    if 'REX_MEM' in text:
        text = MEMCALL_WRAP.sub(r'\1;', text)
    if 'REX_STV' in text:
        text = VEC_LEFT.sub(lambda m: '%sfor (size_t i = 0; i < (16 - (ea & 0xF)); i++)\n%s\tREX_STORE_U8(ea + i, %s.u8[15 - i]);'
                            % (m.group(1), m.group(1), m.group(2)), text)
        text = VEC_RIGHT.sub(lambda m: '%sfor (size_t i = 0; i < (ea & 0xF); i++)\n%s\tREX_STORE_U8(ea - i - 1, %s.u8[i]);'
                             % (m.group(1), m.group(1), m.group(2)), text)
    return text
PROMOTED = re.compile(r'\bREX_PROMOTED_CALL\(0x[0-9A-F]{8}, (sub_[0-9A-F]{8})\)')
LABEL = re.compile(r'^(loc_[0-9A-F]+):\s*$')
DEF = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
INDEX = os.path.join(ROOT, 'out', 'localize_index.json')
ALL = None  # "reach unknown": full sync
# Kernel imports whose register use is known: the SDK's export shim (rex::ppc::HostToGuestFunction) reads exactly the
# arguments of the host function and writes only its return register, and these host functions never touch the guest
# context otherwise (no wait, no callback into guest code). Any other import gets the full sync.
#   KeTlsGetValue_entry(u32 tls_index) -> u32 (sdk/src/kernel/xboxkrnl/xboxkrnl_threading.cpp): r3 in, r3 out.
#   KeTlsSetValue_entry(u32 tls_index, u32 tls_value) -> u32 (same file): r3, r4 in, r3 out.
#   RtlEnterCriticalSection_entry(cs) / RtlLeaveCriticalSection_entry(cs) -> void (xboxkrnl_rtl.cpp): r3 in; a contended
#   enter waits NON-alertably (xeKeWaitForSingleObject(..., alertable 0)), so no APC runs guest code meanwhile.
#   RtlTryEnterCriticalSection_entry(cs) -> u32: r3 in, r3 out.
IMPORT_REACH = {'__imp__KeTlsGetValue': {'r3'}, '__imp__KeTlsSetValue': {'r3', 'r4'},
                '__imp__RtlEnterCriticalSection': {'r3'}, '__imp__RtlLeaveCriticalSection': {'r3'},
                '__imp__RtlTryEnterCriticalSection': {'r3'},
                # KeBugCheckEx_entry(code, p1, p2, p3, p4) / KeBugCheck_entry(code) -> void (xboxkrnl_debug.cpp): log
                # and break, nothing else (the heap's corruption path).
                '__imp__KeBugCheckEx': {'r3', 'r4', 'r5', 'r6', 'r7'}, '__imp__KeBugCheck': {'r3'}}
# App hooks that only READ these registers (and ctx.lr), write none, and call the original: their reach is the
# original's plus these. The heap tracker of app/src/peggle1_debug_hooks.cpp (a diagnostic: when it goes, these go).
# A hook here that wrote a register would show as a smart-sync DIFFERENCE in verify mode.
TRANSPARENT_HOOKS = {
    '8231BAE0': {'r1', 'r3', 'r5'},        # RtlAllocateHeap
    '8231C3D8': {'r1', 'r3', 'r5'},        # RtlFreeHeap
    '8231C6D0': {'r1', 'r3', 'r5', 'r6'},  # RtlReAllocateHeap
    '82369B48': set(), '825EEA68': set(), '825EE8C0': set(), '823188B0': set(), '823189E8': set(),  # entries
}


def build_index(gen):
    """name -> [mentioned registers, callees, unknown-reach flag] for every generated function (cached by mtime)."""
    files = sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp')))
    stamp = [[os.path.basename(f), os.path.getmtime(f)] for f in files]
    if os.path.exists(INDEX):
        try:
            cached = json.load(open(INDEX, encoding='utf-8'))
            if cached.get('stamp') == stamp and cached.get('gen') == os.path.abspath(gen):
                return cached['funcs']
        except (OSError, ValueError):
            pass
    funcs = {}
    for path in files:
        name = None
        for line in open(path, encoding='utf-8', errors='replace'):
            m = DEF.match(line.rstrip('\r\n'))
            if m:
                name = m.group(1)
                funcs[name] = [set(), set(), False]
                continue
            if name is None:
                continue
            if line.startswith('}'):
                name = None
                continue
            if line.lstrip().startswith('//'):
                continue
            line = plain(line)  # a wrapped memcpy / memset call is a call (banjo-nx's round)
            f = funcs[name]
            for m in REG.finditer(line):
                f[0].add(m.group(1))
            for m in LEAF_REG.finditer(line):  # tools/leaf_locals.py: the function's registers, under another name
                f[0].add(m.group(1))
            for m in CALL.finditer(line):
                if m.group(1) is None:  # indirect call or trap
                    f[2] = True
                else:
                    f[1].add(m.group(1))
    out = {k: [sorted(v[0]), sorted(v[1]), v[2]] for k, v in funcs.items()}
    os.makedirs(os.path.dirname(INDEX), exist_ok=True)
    json.dump({'gen': os.path.abspath(gen), 'stamp': stamp, 'funcs': out}, open(INDEX, 'w', encoding='utf-8'))
    return out


def native_hooks():
    """Addresses of our natives: the hand-written hooks of peggle1_hot_guest.cpp and the localized copies of
    localized.txt. A native touches no more than its original, a copy exactly what its original touches."""
    text = open(os.path.join(ROOT, 'app', 'src', 'cpu', 'peggle1_hot_guest.cpp'), encoding='utf-8').read()
    names = set(re.findall(r'^PEGGLE1_HOT_HOOK(?:_UNGUARDED|_LOGGED)?\(([0-9A-F]{8}),', text, re.M))
    for line in open(os.path.join(ROOT, 'app', 'src', 'cpu', 'hot', 'localized.txt'), encoding='utf-8'):
        line = line.strip()
        if line and not line.startswith('#'):
            names.add(line.split()[0].upper())
    return names


def other_hooks():
    """Every 82xxxxxx address the app or SDK sources mention, minus our natives: an app hook may read any register."""
    names = set()
    for root in (os.path.join(ROOT, 'app', 'src'), os.path.join(ROOT, 'sdk', 'src'), os.path.join(ROOT, 'sdk', 'include')):
        for d, _, fs in os.walk(root):
            for fn in fs:
                if not fn.endswith(('.cpp', '.h', '.inc', '.hpp')):
                    continue
                try:
                    text = open(os.path.join(d, fn), encoding='utf-8', errors='ignore').read()
                except OSError:
                    continue
                names.update(m.upper() for m in re.findall(r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])', text))
    return names - native_hooks()


class Reach:
    """Transitive register reach of generated functions: the union of the registers mentioned by a function and by
    everything it calls (strongly connected components over the call graph, computed on demand). ALL = unknown."""

    def __init__(self, funcs, unsafe):
        self.funcs = funcs
        self.unsafe = unsafe
        self.memo = {}

    def own(self, name):
        if name in IMPORT_REACH:
            return set(IMPORT_REACH[name]), ()
        imp = name.startswith('__imp__')
        base = name[len('__imp__'):] if imp else name
        if not imp and base.startswith('sub_') and base[4:] in TRANSPARENT_HOOKS:
            return set(TRANSPARENT_HOOKS[base[4:]]), ['__imp__' + base]  # the hook's reads, then the original
        if base.startswith('sub_') and base[4:] in self.unsafe and not imp:
            return ALL, ()  # overridden by an app hook that may read anything
        f = self.funcs.get(base)
        if f is None or f[2]:
            return ALL, ()  # not a generated function (import, host function), or it makes indirect calls
        return set(f[0]), list(f[1])

    def of(self, root):
        if root in self.memo:
            return self.memo[root]
        # Iterative Tarjan: frames of (node, list of callees, next position).
        index, low, on_stack, stack, owns = {}, {}, set(), [], {}
        counter = 0
        frames = []

        def push(n):
            nonlocal counter
            index[n] = low[n] = counter
            counter += 1
            stack.append(n)
            on_stack.add(n)
            regs, callees = self.own(n)
            owns[n] = (regs, callees)
            frames.append([n, callees, 0])

        push(root)
        while frames:
            frame = frames[-1]
            n, callees, pos = frame
            if pos < len(callees):
                frame[2] += 1
                c = callees[pos]
                if c in self.memo:
                    continue
                if c not in index:
                    push(c)
                elif c in on_stack:
                    low[n] = min(low[n], index[c])
                continue
            frames.pop()
            if frames:
                parent = frames[-1][0]
                low[parent] = min(low[parent], low[n])
            if low[n] != index[n]:
                continue
            members = []
            while True:
                w = stack.pop()
                on_stack.discard(w)
                members.append(w)
                if w == n:
                    break
            member_set = set(members)
            reach = set()
            for w in members:
                regs, callees = owns[w]
                if regs is ALL:
                    reach = ALL
                    break
                reach |= regs
                for c in callees:
                    if c in member_set:
                        continue
                    rc = self.memo.get(c, ALL)  # finished SCCs are memoized; anything else is unknown
                    if rc is ALL:
                        reach = ALL
                        break
                    reach |= rc
                if reach is ALL:
                    break
            for w in members:
                self.memo[w] = reach
        return self.memo[root]


_REACH = {}


def reach_for(gen):
    if gen not in _REACH:
        _REACH[gen] = Reach(build_index(gen), other_hooks())
    return _REACH[gen]


def find_body(gen, name):
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
        m = re.search(r'^DEFINE_REX_FUNC\(' + name + r'\) \{\n(.*?)^\}\n', text, re.S | re.M)
        if m:
            return unleaf(plain(m.group(1))), path
    return None, None


def unleaf(body):
    """The function's text as it was before tools/leaf_locals.py (which runs after this tool in a fresh tree, but
    before it when post_codegen.py or the build's configure step runs this tool again on a processed tree):
    ctx.<register> again, without the declarations and the stores before the returns. This tool then makes its own
    locals of them. (sa2-nx's.)"""
    regs = LEAF_REG.findall(body)
    if not regs:
        return body
    names = re.compile(r'\bl_(%s)\b' % '|'.join(re.escape(r) for r in regs))
    out = []
    for l in body.split('\n'):
        # REX_LEAF_DEAD_R / _F (tools/leaf_dead_stores.py) are write-backs no caller reads: in the ctx form every
        # register is written where the function writes it, so they go like the stores (sa2-nx, banjo-nx).
        if re.match(r'\tREX_LEAF_LOCAL\(', l) or \
                re.match(r'\t(?:REX_LEAF_(?:STORE|DEAD_R|DEAD_F)\(l_\w+, \w+\);\s?)+$', l):
            continue
        l = re.sub(r'\{ (?:REX_LEAF_(?:STORE|DEAD_R|DEAD_F)\(l_\w+, \w+\); )+return; \}', 'return;', l)
        out.append(names.sub(lambda m: 'ctx.' + m.group(1), l))
    if 'REX_LEAF_' in '\n'.join(out):
        sys.exit('localize_native: a leaf-local form this tool does not know (tools/leaf_locals.py changed?)')
    return '\n'.join(out)


def function_starts(gen):
    starts = set()
    for path in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        for m in re.finditer(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)', open(path, errors='replace').read(), re.M):
            starts.add(int(m.group(1), 16))
    return sorted(starts)


def code_hash(image, starts, addr):
    following = [s for s in starts if s > addr]
    end = following[0] if following else addr + 4
    code = image[addr - BASE:end - BASE]
    h = 0xCBF29CE484222325
    for b in code:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return len(code), h


def type_of(reg):
    if reg.startswith('cr'):
        return 'PPCCRRegister'
    if reg == 'xer':
        return 'PPCXERRegister'
    if reg.startswith('v'):
        return 'PPCVRegister'
    return 'PPCRegister'


def logged(line):
    """The same line with its guest stores recorded first (PEGGLE1_LOG_*: see peggle1_hot_guest.cpp)."""
    line = re.sub(r'\bREX_STORE_U(8|16|32|64)\(', r'PEGGLE1_LOG_STORE_U\1(', line)
    # D-form stores (codegen dform_disp_split, 2026-10-07): unlogged, the guard's rollback missed them, so verify mode
    # reported 7 false differences in the Wwise leaves and a checked call left the copy's stores in guest memory.
    line = re.sub(r'\bREX_STORE_D_U(8|16|32|64)\(', r'PEGGLE1_LOG_STORE_D_U\1(', line)
    line = line.replace('simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(', 'PEGGLE1_LOG_STORE_V128((')
    line = re.sub(r'\bmemset\(\(void\*\)REX_RAW_ADDR\(([^)]*)\), 0, (\d+)\);', r'PEGGLE1_LOG_ZERO(\1, \2);', line)
    return line


def localize(addr, what, gen, outdir, sync='smart'):
    name = 'sub_' + addr
    body, path = find_body(gen, name)
    if body is None:
        sys.exit(f'{name} not found in {gen}')
    lines = [l for l in body.splitlines() if not l.strip().startswith('//')]  # drop the PPC disassembly comments
    backward = set()
    seen = set()
    for l in lines:
        m = LABEL.match(l.strip())
        if m:
            seen.add(m.group(1))
        for target in re.findall(r'goto (loc_[0-9A-F]+);', l):
            if target in seen:
                backward.add(target)
    regs = sorted({m.group(1) for l in lines for m in REG.finditer(l)},
                  key=lambda r: (re.sub(r'\d', '', r), int(re.sub(r'\D', '', r) or 0)))
    out_regs = '; '.join(f'ctx.{r} = l_{r}' for r in regs) or '(void)0'
    in_regs = '; '.join(f'l_{r} = ctx.{r}' for r in regs) or '(void)0'
    reach = reach_for(gen) if sync == 'smart' else None
    synced = []  # registers synced per call (statistics)
    smart_sites = 0

    def call_sync(m):
        nonlocal smart_sites
        callee = m.group(1)
        call = m.group(0)
        full = '{ PEGGLE1_SYNC_OUT(); ' + call + ' PEGGLE1_SYNC_IN(); }'
        regset = ALL if (reach is None or callee is None) else reach.of(callee)
        if regset is ALL:
            synced.append(len(regs))
            return full
        if os.environ.get('PEGGLE1_LOCALIZE_PLANT'):  # planted-bug test of the verify check: pretend r3 is out of reach
            regset = set(regset) - {'r3'}
        part = [r for r in regs if r in regset]
        rest = [r for r in regs if r not in regset]
        synced.append(len(part))
        if not rest:
            return full  # the callee can reach every localized register: nothing to save
        smart_sites += 1
        before = ''.join(f'ctx.{r} = l_{r}; ' for r in part)
        after = ''.join(f' l_{r} = ctx.{r};' for r in part)
        # The verify-mode check is out of line (peggle1::hot::SmartCall): one call in the cold path, with the registers
        # to compare as a small static table, so it adds little code to the copy (I-cache on the A57).
        spans = ', '.join(f'PEGGLE1_SPAN({r})' for r in rest)
        checked = ('if (::peggle1::enhancements::g_verify_mode) [[unlikely]] { '
                   f'static constexpr ::peggle1::hot::RegSpan kRest[] = {{{spans}}}; '
                   f'::peggle1::hot::SmartCall(ctx, base, &{callee}, kRest, {len(rest)}, 0x{addr}u, "{callee}"); '
                   f'}} else {{ {call} }}')
        return ('{ if (::peggle1::hot::g_smart_sync.active) [[likely]] { ' + before + checked + after +
                ' } else ' + full + ' }')

    code = []
    calls = 0
    unsupported = []
    for l in lines:
        l = REG.sub(lambda m: 'l_' + m.group(1), l)
        # Calls and returns get braces: the generated code also writes them as the body of a one-line `if`, where a
        # bare "SYNC_OUT(); return;" would return unconditionally (a bug sa2-nx hit).
        l, n = CALL.subn(call_sync, l)
        calls += n
        l = re.sub(r'\breturn;', '{ PEGGLE1_SYNC_OUT(); return; }', l)
        code.append(l)
        m = LABEL.match(l.strip())
        if m and m.group(1) in backward:
            code.append('\tPEGGLE1_LOOP_BARRIER();')
        if (re.search(r'\b(stvewx|lwarx|stwcx|ldarx|stdcx)\b', l) or '__sync_' in l or 'REX_GUEST_CAS' in l or
                'REX_ENTER_GLOBAL_LOCK' in l):  # REX_GUEST_CAS: tools/guest_cas.py's name for the same builtin
            unsupported.append(l.strip())
    code.append('\tPEGGLE1_SYNC_OUT();')  # falling off the end of the function is a return too
    if unsupported:
        sys.exit(f'{name}: atomics or the global lock, not localized:\n' + '\n'.join(unsupported[:6]))
    decl = '\n'.join(f'  {type_of(r)} l_{r} = ctx.{r};' for r in regs)
    # Every direct callee needs a declaration (recompiled functions, helpers, kernel imports).
    callees = {m.group(1) for l in lines for m in CALL.finditer(l) if m.group(1)}
    # tools/indirect_calls.py: a promoted call names its target's hookable symbol in its PC self-check, also where
    # the call itself goes to __imp__sub_X.
    callees |= {m.group(1) for l in lines for m in PROMOTED.finditer(l)}
    callees = sorted(callees)
    externs = ''.join(f'REX_EXTERN({c});\n' for c in callees)
    rel = os.path.relpath(path, ROOT).replace('\\', '/')

    def function(fname, body_lines):
        return (f'inline void {fname}(PPCContext& ctx, uint8_t* base) {{\n{decl}\n'
                + '\n'.join(body_lines) + '\n}\n')

    leaf = calls == 0
    if calls and sync == 'smart':
        sync_text = (f'with smart sync around {smart_sites} of its {calls} call(s) (registers per call: '
                     + ', '.join(str(x) for x in synced) + f' of {len(regs)})')
    else:
        sync_text = f'all of them written back and reloaded around each of its {calls} call(s)'
    text = f'''// {name}: {what}.
// GENERATED by tools/localize_native.py from {rel} (do not edit; the build re-runs the tool after every codegen):
// the recompiled function's own operations, with its {len(regs)} context registers in C++ locals (written back at every
// return; {sync_text}), plain guest memory accesses and {len(backward)} loop barrier(s).
// {'Leaf function: NativeLogged records every guest store for the guard.' if leaf else 'It calls other functions: the hook runs it unguarded (exact by construction).'}
#pragma once

{externs}
namespace peggle1::hot::n_{addr} {{

#define PEGGLE1_SYNC_OUT() do {{ {out_regs}; }} while (0)
#define PEGGLE1_SYNC_IN() do {{ {in_regs}; }} while (0)
{function('Native', code)}'''
    if leaf:
        text += '\n' + function('NativeLogged', [logged(l) for l in code])
    text += f'''#undef PEGGLE1_SYNC_OUT
#undef PEGGLE1_SYNC_IN

}}  // namespace peggle1::hot::n_{addr}
'''
    out = os.path.join(outdir, f'n_{addr}.h')
    os.makedirs(outdir, exist_ok=True)
    old = open(out, encoding='utf-8').read() if os.path.exists(out) else None
    if old != text:  # unchanged files keep their time stamp: no rebuild
        open(out, 'w', encoding='utf-8', newline='\n').write(text)
    return leaf, len(regs), calls, len(backward), synced, smart_sites


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('address', nargs='?')
    ap.add_argument('--what', default='')
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated', 'default'))
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'default_image.bin'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'app', 'generated', 'hot'))
    ap.add_argument('--sync', choices=('smart', 'full'), default='smart',
                    help='smart: each call syncs only what its callee can reach (default); full: every register')
    ap.add_argument('--list', help='"<address> <what>" lines (app/src/cpu/hot/localized.txt): localize them all and '
                                   'write the hook lines (localized_hooks.inc)')
    a = ap.parse_args()
    if not a.list:
        if not a.address:
            ap.error('an address or --list')
        addr = a.address.upper().replace('SUB_', '').replace('0X', '')
        leaf, regs, calls, loops, synced, smart = localize(addr, a.what or 'sub_' + addr, a.gen, a.out, a.sync)
        print(f'sub_{addr}: {regs} registers localized, {calls} calls ({smart} with smart sync: {synced}), '
              f'{loops} loop barriers, {"leaf (logged variant)" if leaf else "not a leaf"}')
        return
    image = open(a.image, 'rb').read()
    starts = function_starts(a.gen)
    hooks = ['// GENERATED by tools/localize_native.py --list app/src/cpu/hot/localized.txt (do not edit): one hook per',
             '// localized copy. Leaf functions are guarded with the store log; the others run unguarded.', '']
    count = smart_total = calls_total = saved = 0
    for line in open(a.list, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        addr, _, what = line.partition(' ')
        addr = addr.upper()
        what = what.strip() or 'sub_' + addr
        leaf, regs, calls, loops, synced, smart = localize(addr, what, a.gen, a.out, a.sync)
        smart_total += smart
        calls_total += calls
        saved += sum(regs - s for s in synced)
        size, h = code_hash(image, starts, int(addr, 16))
        desc = f'Native localized copy of sub_{addr}: {what}'.replace('"', "'")
        macro = 'PEGGLE1_HOT_HOOK_LOGGED' if leaf else 'PEGGLE1_HOT_HOOK_UNGUARDED'
        hooks.append(f'#include "n_{addr}.h"')
        hooks.append(f'{macro}({addr}, peggle1_cpu_native_sub_{addr}, "{desc}", peggle1::hot::n_{addr}, {size}, '
                     f'0x{h:016X}ull)')
        count += 1
    text = '\n'.join(hooks) + '\n'
    inc = os.path.join(a.out, 'localized_hooks.inc')
    os.makedirs(a.out, exist_ok=True)  # peggle1-nx: a tree whose list is empty has made no copies yet
    old = open(inc, encoding='utf-8').read() if os.path.exists(inc) else None
    if old != text:
        open(inc, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'{count} localized copies in {os.path.relpath(a.out, ROOT)}; {smart_total} of {calls_total} call sites with '
          f'smart sync ({saved} register write-back/reload pairs fewer per pass through every call site)')


if __name__ == '__main__':
    main()
