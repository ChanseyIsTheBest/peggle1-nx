#!/usr/bin/env python3
"""(peggle2-nx's copy of banjo-nx's tool, 2026-10-19: Peggle's image dump out/default_image.bin, the tree
app/generated/default; compile switches PEGGLE1_GEN_DEAD_DEFS and PEGGLE1_GEN_DEAD_DEFS_POISON in app/CMakeLists.txt.
The functions Peggle's game patches change are opaque: tools/leaf_dead_stores.py, OPAQUE_MARK. banjo-nx's text:)
Guest register writes no instruction can read, left out (post-codegen step; banjo-nx, 2026-10-15). Works on any
ReXGlue port that runs tools/leaf_dead_stores.py (it reuses that tool's guest-level analysis).

Why. In a function that calls anything, the registers the generator keeps in the PPCContext (the volatile ones it does
not turn into locals) are written to ctx by every instruction that sets them. The C++ compiler keeps those stores: a
call may read ctx, and every guest memory access goes through a pointer it cannot tell apart from ctx. Many of them are
dead in the guest program itself: the register is written again before anything reads it. The hottest example in Banjo
is the bytecode interpreter sub_82948928 (9.8 % of its game thread on the console): every opcode dispatch loads the
jump table entry into r0 and the table address into r12 for the `mtctr; bctr` the generated `switch` no longer needs,
three context stores and a guest load per opcode, and every case returns to the loop head, which writes them again.

What. tools/leaf_dead_stores.py's analysis gives, per function, the registers its callees may read (UE) and the
registers its callers may read after it returns (LiveOut). With them, per function, a backward liveness over its
guest instructions (calls read their callee's UE and kill nothing; exits keep LiveOut; a switch's targets are its
successors; an indirect call, trap or unknown call reads everything). An instruction whose only effect is to write one
general or float register that is live nowhere after it is dead. Its C++ statement, when it is exactly one
`ctx.<reg>.<field> = <expression without calls>;`, becomes REX_DEAD_DEF(<reg>, <statement>): nothing, the statement
again with REX_DEAD_DEFS_KEEP, or the register poisoned with REX_DEAD_DEFS_POISON (a test build: a wrong verdict
shows as a crash or a picture difference). Records (`add.`), carries, updates (`lwzu`) and anything writing more than
one register keep their statements.

The soundness argument is leaf_dead_stores.py's (lesson (e): the 360 compiler allocates registers across functions, so
nothing is assumed from the ABI; a register is dead only if no path reads it). Run after leaf_dead_stores.py.

  python tools/dead_ctx_defs.py --gen app/generated/default [--check] [--list FILE]
"""
import argparse
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import leaf_dead_stores as lds  # noqa: E402

ASSIGN = re.compile(r'^ctx\.([rf]\d+)\.(u64|s64|u32|s32|f64|u8|s8|u16|s16)\s*=\s*(.+);$')
LR_READ = re.compile(r'^REX_LR_READ\(ctx\.(r\d+)\);$')
RECORD_CMP = re.compile(r'^cr0\.compare<int32_t>\(ctx\.(r\d+)\.s32, 0, xer\);$')
SWITCH = re.compile(r'switch \(ctx\.(r\d+)\.u32\)')
SIDE_EFFECTS = re.compile(r'\(ctx, base\)|\+\+|--|REX_CALL|ppc_trap|__builtin_trap|REX_STORE|=\s*[^=]*=')
# Instruction forms with more than the one register write the def/use table shows.
SKIP_FORMS = re.compile(r'^(?:addic|addc|adde|addme|addze|subfc|subfe|subfme|subfze|subfic|srawi?|mtxer|mfxer|'
                        r'mtcrf|mtspr|mfspr|mtctr|mtlr|lwarx|ldarx|stwcx|stdcx|mffs|mtfs.*|\w+u|\w+ux)\.?$')


# --abi-indirect: what a caller through a pointer can read after the call. It cannot know which function it called, so
# the Xbox 360 calling convention bounds it: the non-volatile registers, the stack, TOC and thread pointers, and the
# return registers (r3, r4, f1-f4, generously). Direct calls stay without any convention (lesson (e)).
# Registers never left out, whatever the guest program reads: the stack, TOC and thread pointers (host code reads
# ctx.r1: kernel calls, the exception runtime walking the guest stack) and the non-volatile registers (the exception
# runtime's funclets read their parent's frame registers, a flow this analysis does not see). 22 writes in Banjo.
NEVER_DEAD = lds.R(1) | lds.R(2) | lds.R(13) | sum(lds.R(n) for n in range(14, 32)) | sum(lds.F(n) for n in range(14, 32))
ABI_OUT = ~lds.VOLATILE & lds.ALL | lds.ALWAYS | lds.R(4) | lds.F(2) | lds.F(3) | lds.F(4)
# And what a function called through a pointer can be given: everything but the scratch registers r0, r11, r12 and f0
# (the arguments r3-r10 / f1-f13, the non-volatile registers it may save, the stack, TOC and thread pointers).
ABI_IN = lds.ALL & ~(lds.R(0) | lds.R(11) | lds.R(12) | lds.F(0))


NAMED_HEAD = re.compile(r'^DEFINE_REX_FUNC\((?:__imp__)?(\w+)\) \{$')
TABLE = re.compile(r'\{ 0x([0-9A-Fa-f]{8}), (\w+) \},')
NAMED_CALL = re.compile(r'\b(__imp__)?([A-Za-z_]\w*)\(ctx, base\)')


def named_functions(gen):
    """name -> address of the guest functions the generator names (rex_strncat, meInternalFree: the registration
    table of <project>_init.cpp)."""
    out = {}
    for path in glob.glob(os.path.join(gen, '*_init.cpp')):
        for m in TABLE.finditer(open(path, encoding='utf-8').read()):
            if not m.group(2).startswith('sub_'):
                out[m.group(2)] = int(m.group(1), 16)
    return out


def hooked_names(names):
    """The named functions the port's own sources mention (a hook by name replaces the generated body)."""
    found = set()
    roots = [os.path.join(lds.ROOT, 'app', 'src'), os.path.join(lds.ROOT, 'sdk', 'src'),
             os.path.join(lds.ROOT, 'sdk', 'include')]
    for root in roots:
        for d, _, fs in os.walk(root):
            for f in fs:
                if not f.endswith(('.cpp', '.h', '.hpp', '.inc', '.c')):
                    continue
                try:
                    text = open(os.path.join(d, f), encoding='utf-8', errors='ignore').read()
                except OSError:
                    continue
                found |= names & set(re.findall(r'\b[A-Za-z_]\w*\b', text))
    return found


def all_functions(gen, named):
    """address -> (file, body lines) for every function of the tree, the named ones included (lds.functions has only
    sub_X: a sub_X also called from a named function would miss that caller's needs)."""
    out = {}
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        lines = open(path, encoding='utf-8').read().split('\n')
        i = 0
        while i < len(lines):
            m = NAMED_HEAD.match(lines[i])
            if not m:
                i += 1
                continue
            name = m.group(1)
            addr = int(name[4:], 16) if re.fullmatch(r'sub_[0-9A-F]{8}', name) else named.get(name)
            j = i + 1
            while j < len(lines) and lines[j] != '}':
                j += 1
            if addr is not None:
                out[addr] = (path, lines[i + 1:j])
            i = j + 1
    return out


def fix_named_calls(nodes, chunks, named, hooked):
    """A call of a named guest function becomes a call edge (parse_function took it for an unknown host call that
    reads everything). Known unless a source of the port names it (its hook runs instead)."""
    if len(nodes) != len(chunks):
        return
    for i, (mnemonic, statements) in enumerate(chunks):
        if mnemonic not in ('bl', 'b', 'bctr', 'bctrl') and not mnemonic.startswith('b'):
            continue
        text = ' '.join(t for _, t in statements)
        calls = [(imp, name) for imp, name in NAMED_CALL.findall(text) if name in named]
        if len(calls) != 1 or nodes[i].call:
            continue
        others = [name for _, name in NAMED_CALL.findall(text) if name not in named and not name.startswith('sub_')]
        if others or 'REX_CALL_INDIRECT' in text or 'REX_PROMOTED' in text or 'ppc_trap' in text:
            continue
        imp, name = calls[0]
        nodes[i].use = 0
        nodes[i].call = (named[name], bool(imp) or name not in hooked)


# Kernel exports (__imp__KeQuerySystemTime: host code written against the guest calling convention) read what a call
# through a pointer can be given, ABI_IN, with --abi-indirect; those that capture or restore a whole register context
# keep everything.
CONTEXT_EXPORT = re.compile(r'Context|Capture|Unwind|jmp|Continue|Exception|Raise|Thread|Apc|Dpc')


def fix_imports(nodes, chunks):
    if len(nodes) != len(chunks):
        return
    for i, (mnemonic, statements) in enumerate(chunks):
        if nodes[i].use != lds.ALL or nodes[i].call:
            continue
        text = ' '.join(t for _, t in statements)
        if 'REX_CALL_INDIRECT' in text or 'REX_PROMOTED' in text or 'ppc_trap' in text:
            continue
        names = re.findall(r'\b(\w+)\(ctx, base\)', text)
        if names and all(n.startswith('__imp__') and not n.startswith('__imp__sub_') and
                         not CONTEXT_EXPORT.search(n) for n in names):
            nodes[i].use = ABI_IN


def analysis(gen, image, abi_indirect=False):
    """leaf_dead_stores.py's steps 1-3: (bodies, parsed, ue, live_out, natives-adjusted calls)."""
    named = named_functions(gen)
    hooked = hooked_names(set(named))
    bodies = all_functions(gen, named)
    starts = set(bodies)
    taken = lds.address_taken(gen, image, starts, bodies)
    # A named function the sources name by its address (a toml hook, a native) is hooked as well.
    hooked |= {name for name, addr in named.items()
               if any(reason.startswith('named in') for reason in taken.get(addr, ()))}
    parsed = {addr: lds.parse_function(lines) for addr, (_, lines) in bodies.items()}
    for addr, (_, lines) in bodies.items():
        chunks = chunks_of(lines)
        fix_named_calls(parsed[addr], chunks, named, hooked)
        fix_switches(parsed[addr], chunks, abi_indirect)
        if abi_indirect:
            fix_imports(parsed[addr], chunks)
    hand = os.path.join(lds.ROOT, 'app', 'src', 'cpu', 'hot', 'hand.txt')
    natives = set()
    if os.path.exists(hand):
        natives = set(int(m.group(1), 16) for m in re.finditer(r'^([0-9A-Fa-f]{8})\b',
                                                                 open(hand, encoding='utf-8').read(), re.M))
    for nodes in parsed.values():
        for node in nodes:
            if node.call and not node.call[1] and node.call[0] in natives:
                node.call = (node.call[0], True)
    ALL = lds.ALL
    summary = {}
    for addr, nodes in parsed.items():
        undefined = lds.forward_undefined(nodes)
        local, calls = 0, []
        for node, state in zip(nodes, undefined):
            local |= node.use & state
            if node.call:
                calls.append((node.call, state))
        summary[addr] = (local, calls)
    ue = {addr: s[0] for addr, s in summary.items()}
    callers_of = collections.defaultdict(set)
    for addr, (_, calls) in summary.items():
        for (callee, known), _ in calls:
            callers_of[callee].add(addr)
    work = set(summary)
    while work:
        addr = work.pop()
        local, calls = summary[addr]
        new = local
        for (callee, known), state in calls:
            new |= (ue.get(callee, ALL) if known else ALL) & state
        if new != ue[addr]:
            ue[addr] = new
            work |= callers_of[addr]

    def use_of(node):
        if node.call:
            callee, known = node.call
            return node.use | (ue.get(callee, ALL) if known else ALL)
        return node.use

    sites = collections.defaultdict(list)
    tails = collections.defaultdict(set)
    direct = collections.Counter()
    for addr, nodes in parsed.items():
        if not any(node.call for node in nodes):
            continue
        live_a = lds.backward(nodes, use_of, 0)
        live_t = lds.backward(nodes, lambda node: 0, ALL)
        for i, node in enumerate(nodes):
            if not node.call:
                continue
            callee, known = node.call
            direct[callee] += 1
            if not known:
                continue
            if node.tail:
                tails[callee].add(addr)
            if not node.tail or node.succ:
                out_a = 0
                out_t = ALL if (node.exit and not node.tail) else 0
                for s in node.succ:
                    out_a |= live_a[s]
                    out_t |= live_t[s]
                sites[callee].append((addr, out_a, out_t))
    live_out = {}
    for addr, nodes in parsed.items():
        if addr in taken:
            # A guest function pointer (a data word, a lis pair) is called by guest code that cannot know it; the
            # entry point and an address the port's sources name (host code, hooks, natives) keep everything.
            pointer_only = taken[addr] <= {'data word', 'lis pair'}
            live_out[addr] = ABI_OUT if abi_indirect and pointer_only else ALL
        elif not direct[addr]:
            thunk = len(nodes) == 4 and nodes[3].tail
            live_out[addr] = 0 if thunk else ALL
        else:
            live_out[addr] = 0
    for nodes in parsed.values():
        for node in nodes:
            if node.call and not node.call[1] and node.call[0] in live_out:
                live_out[node.call[0]] = ALL
    callees_of = collections.defaultdict(set)
    for callee, lst in sites.items():
        for caller, _, _ in lst:
            callees_of[caller].add(callee)
    for callee, callers in tails.items():
        for caller in callers:
            callees_of[caller].add(callee)
    if abi_indirect:
        # A function both taken and called directly also gets what its direct callers need (the fixed point below);
        # one that is called through a hook keeps everything.
        pass
    work = set(addr for addr in parsed if live_out[addr] != ALL)
    while work:
        addr = work.pop()
        if live_out[addr] == ALL:
            continue
        new = live_out[addr]
        for caller, out_a, out_t in sites.get(addr, ()):
            new |= out_a | (live_out.get(caller, ALL) & out_t)
        for caller in tails.get(addr, ()):
            new |= live_out.get(caller, ALL)
        if new != live_out[addr]:
            live_out[addr] = new
            work |= callees_of[addr]
    return bodies, parsed, use_of, live_out


def fix_switches(nodes, chunks, abi_indirect=False):
    """The generated `switch (ctx.rN.u32)` that replaces a jump table's `bctr` reads rN (the guest bctr reads only
    ctr), and the `mtctr` in front of it feeds a ctr nothing reads any more: the jump table entry it loads is dead.
    With abi_indirect, a call through a pointer reads ABI_IN instead of everything."""
    if len(nodes) != len(chunks):
        return
    for i, (mnemonic, statements) in enumerate(chunks):
        text = ' '.join(t for _, t in statements)
        if abi_indirect and ('REX_CALL_INDIRECT' in text or 'REX_PROMOTED' in text) and 'ppc_trap' not in text:
            nodes[i].use = ABI_IN
        m = SWITCH.search(text)
        if not m:
            continue
        nodes[i].use |= lds.regs(m.group(1))
        if i > 0 and chunks[i - 1][0] == 'mtctr' and len(chunks[i - 1][1]) == 1 and \
                re.match(r'^ctr\.u64 = ctx\.r\d+\.u64;$', chunks[i - 1][1][0][1]):
            nodes[i - 1].use = 0


def chunks_of(lines):
    """Per guest instruction (in parse_function's order): (mnemonic, [(line index, stripped text)])."""
    out = []
    for k, line in enumerate(lines):
        if lds.LABEL.match(line):
            continue
        m = lds.COMMENT.match(line)
        if m:
            out.append((m.group(1), []))
            continue
        if out and line.strip():
            out[-1][1].append((k, line.strip()))
    return out


def removable(node, chunk):
    """('ok', kind, [(line index, new text)]) for a dead single-register write this step can leave out, or
    ('kept', reason, None)."""
    mnemonic, statements = chunk
    if node.call or SKIP_FORMS.match(mnemonic):
        return 'kept', 'form with other effects', None
    if mnemonic.endswith('.'):
        # A record form: the register is dead, cr0 is not. The value goes to a temporary for the comparison.
        if len(statements) != 2:
            return 'kept', 'record form of another shape', None
        (k0, t0), (k1, t1) = statements
        m, c = ASSIGN.match(t0), RECORD_CMP.match(t1)
        if not m or not c or m.group(1) != c.group(1) or SIDE_EFFECTS.search(m.group(3)) or \
                lds.regs(m.group(1)) != node.defn:
            return 'kept', 'record form of another shape', None
        reg, field, expression = m.groups()
        text = 'REX_DEAD_REC(%s, %s, %s, %s)' % (reg, t0[:-1], 'rex_dead.%s = %s' % (field, expression),
                                                 t1[:-1].replace('ctx.%s.s32' % reg, 'rex_dead.s32'))
        return 'ok', 'record forms (cr0 kept)', [(k0, text + ';'), (k1, '')]
    if len(statements) != 1:
        return 'kept', 'not one statement', None
    k, text = statements[0]
    m = LR_READ.match(text)
    if m:
        if lds.regs(m.group(1)) != node.defn:
            return 'kept', 'writes another register', None
        return 'ok', 'mflr', [(k, 'REX_DEAD_DEF(%s, %s);' % (m.group(1), text[:-1]))]
    m = ASSIGN.match(text)
    if not m or SIDE_EFFECTS.search(m.group(3)):
        return 'kept', 'not a plain ctx assignment', None
    if lds.regs(m.group(1)) != node.defn:
        return 'kept', 'writes another register', None
    return 'ok', 'plain writes', [(k, 'REX_DEAD_DEF(%s, %s);' % (m.group(1), text[:-1]))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--image', default=os.path.join(lds.ROOT, 'out', 'default_image.bin'))
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    ap.add_argument('--list', help='write the dead writes per function to this file')
    ap.add_argument('--abi-indirect', action='store_true',
                    help='bound what callers through a pointer read by the calling convention (see ABI_OUT)')
    ap.add_argument('--restore-only', action='store_true',
                    help='take an earlier run\'s rewrites back out and stop (the step switched off)')
    a = ap.parse_args()
    if a.restore_only:
        print('dead_ctx_defs: off (experimental): %d files restored to their statements' % restore(a.gen))
        return
    if not a.check:
        restored = restore(a.gen)
        if restored:
            print('dead_ctx_defs: %d files restored to their statements before the analysis' % restored)
    bodies, parsed, use_of, live_out = analysis(a.gen, a.image, a.abi_indirect)
    counts = collections.Counter()
    per_function = collections.Counter()
    rewrite = collections.defaultdict(dict)   # path -> {line index in its file: (reg, text)}
    for addr, (path, lines) in bodies.items():
        nodes = parsed[addr]
        if not nodes:
            continue
        chunks = chunks_of(lines)
        if len(chunks) != len(nodes):
            counts['functions skipped (parse mismatch)'] += 1
            continue
        # Removing a dead instruction removes its reads too, which can make others dead: to a fixed point (the
        # callers' LiveOut and the callees' UE stay as computed with every read in: a superset, so still sound).
        removed = {}
        dropped = set()   # statements left out whole: their reads go too. A record form keeps its computation (for
                          # cr0), so it still reads its operands: the write that feeds it stays live (the first build
                          # left out the `lbz` feeding a `rlwinm.` bit test, and the test read a stale register).
        while True:
            live_in = lds.backward(nodes, lambda n: 0 if id(n) in dropped else use_of(n), live_out[addr])
            new = 0
            for i, node in enumerate(nodes):
                if id(node) in removed or not node.defn or node.defn & (node.defn - 1) or node.defn & NEVER_DEAD:
                    continue          # no register write, or more than one
                out = live_out[addr] if node.exit else 0
                for s in node.succ:
                    out |= live_in[s]
                if node.defn & out:
                    continue
                verdict = removable(node, chunks[i])
                if verdict[0] == 'kept':
                    continue
                removed[id(node)] = (i, verdict)
                if verdict[1] != 'record forms (cr0 kept)':
                    dropped.add(id(node))
                new += 1
            if not new:
                break
        for i, node in enumerate(nodes):
            if not node.defn or node.defn & (node.defn - 1) or id(node) in removed:
                continue
            out = live_out[addr] if node.exit else 0
            for s in node.succ:
                out |= live_in[s]
            if not node.defn & out:
                counts['kept: ' + removable(node, chunks[i])[1]] += 1
        for i, verdict in removed.values():
            kind, what, edits = verdict
            counts['left out: ' + what] += 1
            per_function[addr] += 1
            for k, new_text in edits:
                rewrite[path][(addr, k)] = (lines[k].strip(), new_text)
    for k, v in counts.most_common():
        print('dead_ctx_defs: %8d  %s' % (v, k))
    print('dead_ctx_defs: %d functions with writes left out' % len(per_function))
    if a.list:
        with open(a.list, 'w') as f:
            for addr, n in per_function.most_common():
                f.write('sub_%08X %d\n' % (addr, n))
    if a.check:
        return
    install(a.gen, bodies, rewrite)


MARKER = '// rexglue ports: guest register writes nothing reads (tools/dead_ctx_defs.py)'
END = '// (end of the macros of tools/dead_ctx_defs.py)\n'
MACROS = MARKER + '''
#ifndef REX_DEAD_DEF
// REX_DEAD_REC: a record form whose register is dead: the value goes to a temporary for cr0.
#if defined(REX_DEAD_DEFS_KEEP)
#define REX_DEAD_DEF(reg, statement) statement
#define REX_DEAD_REC(reg, statement, dead, compare)   do { statement; cr0.compare<int32_t>(ctx.reg.s32, 0, xer); } while (0)
#elif defined(REX_DEAD_DEFS_POISON)
#define REX_DEAD_DEF(reg, statement) (ctx.reg.u64 = 0xDEADBEEFDEADBEEFull)
#define REX_DEAD_REC(reg, statement, dead, compare) \
  do { PPCRegister rex_dead; dead; compare; ctx.reg.u64 = 0xDEADBEEFDEADBEEFull; } while (0)
#else
#define REX_DEAD_DEF(reg, statement) ((void)0)
#define REX_DEAD_REC(reg, statement, dead, compare) do { PPCRegister rex_dead; dead; compare; } while (0)
#endif
#endif  // REX_DEAD_DEF
''' + END


UNDO_DEF = re.compile(r'^(\s*)REX_DEAD_DEF\(\w+, (.*)\);$')
UNDO_REC = re.compile(r'^(\s*)REX_DEAD_REC\((\w+), (ctx\.\w+\.\w+ = .*), rex_dead\.\w+ = .*, '
                      r'cr0\.compare<int32_t>\(rex_dead\.s32, 0, xer\)\);$')


ORIGINALS = {}   # path -> (text, atime, mtime) before restore(): install() gives an unchanged file its time back


def restore(gen):
    """An earlier run's rewrites back to the statements they wrapped (the generator does not always rewrite a file
    whose output did not change): a re-run analyses the original statements."""
    changed = 0
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        if 'REX_DEAD_DEF(' not in text and 'REX_DEAD_REC(' not in text:
            continue
        st = os.stat(path)
        ORIGINALS[path] = (text, st.st_atime_ns, st.st_mtime_ns)
        lines = text.split('\n')
        out = []
        k = 0
        while k < len(lines):
            line = lines[k]
            m = UNDO_DEF.match(line)
            if m:
                out.append('%s%s;' % (m.group(1), m.group(2)))
                k += 1
                continue
            m = UNDO_REC.match(line)
            if m:
                indent, reg, assign = m.groups()
                out.append('%s%s;' % (indent, assign))
                out.append('%scr0.compare<int32_t>(ctx.%s.s32, 0, xer);' % (indent, reg))
                k += 2 if k + 1 < len(lines) and lines[k + 1] == '' else 1
                continue
            if 'REX_DEAD_DEF(' in line or 'REX_DEAD_REC(' in line:
                sys.exit('dead_ctx_defs: cannot restore %s line %d: %s' % (path, k + 1, line.strip()[:100]))
            out.append(line)
            k += 1
        open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
        changed += 1
    return changed


def install(gen, bodies, rewrite):
    """The dead statements wrapped, each checked to be the statement the analysis saw. A file that ends up as it was
    before restore() gets its time back, so the build does not compile it again."""
    named = named_functions(gen)
    changed = 0
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        if 'REX_DEAD_DEF(' in text or 'REX_DEAD_REC(' in text:
            sys.exit('dead_ctx_defs: %s still has the macros of this step after restore()' % path)
        file_lines = text.split('\n')
        dead = rewrite.get(path)
        if dead:
            i = 0
            while i < len(file_lines):
                m = NAMED_HEAD.match(file_lines[i])
                if not m:
                    i += 1
                    continue
                name = m.group(1)
                addr = int(name[4:], 16) if re.fullmatch(r'sub_[0-9A-F]{8}', name) else named.get(name)
                j = i + 1
                while j < len(file_lines) and file_lines[j] != '}':
                    j += 1
                body_start = i + 1
                for k in range(body_start, j):
                    key = (addr, k - body_start)
                    if key not in dead:
                        continue
                    original, replacement = dead[key]
                    line = file_lines[k]
                    if line.strip() != original:
                        sys.exit('dead_ctx_defs: %s line %d is not the analysed statement' % (path, k + 1))
                    indent = line[:len(line) - len(line.lstrip())]
                    file_lines[k] = indent + replacement if replacement else ''
                i = j + 1
        new = '\n'.join(file_lines)
        before = ORIGINALS.get(path)
        if new != text:
            open(path, 'w', encoding='utf-8', newline='\n').write(new)
            if before and before[0] == new:
                os.utime(path, ns=(before[1], before[2]))
            else:
                changed += 1
    pch = glob.glob(os.path.join(gen, '*_pch.h'))[0]
    text = open(pch, encoding='utf-8').read()
    if MACROS not in text:
        anchor = '#define REX_LOAD_U8(x)'
        if MARKER in text:
            begin = text.index(MARKER)
            text = text[:begin] + MACROS + text[text.index(END, begin) + len(END):]
        elif anchor in text:
            text = text.replace(anchor, MACROS + anchor, 1)
        else:
            sys.exit('ERROR (post-codegen patch): dead_ctx_defs: no REX_LOAD_U8 in %s' % pch)
        open(pch, 'w', encoding='utf-8', newline='\n').write(text)
    print('dead_ctx_defs: %d files changed' % changed)


if __name__ == '__main__':
    main()
