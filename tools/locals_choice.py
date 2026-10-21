#!/usr/bin/env python3
"""Which guest functions are better off WITHOUT the loop / leaf locals, judged from the compiled A57 code
(marathon-nx's proposal 3; docs/codegen-enhancements.md, "proposal 3").

tools/loop_locals.py and tools/leaf_locals.py move the ctx registers of call-free loops and of leaf functions with
loops into C++ locals. That usually saves a store and a reload per register per iteration, but a local wants a host
register: where about forty are live at once (the large localized copies of 2026-10-15 grew 19-54 %) GCC spills them
to the stack and the function gets longer, sometimes inside its loops. Marathon's (different) locals transform made
679 of 1,028 functions longer. This script finds Banjo's losers by compiling every generated file that the two tools
changed twice with the Switch build's own command line:
    A  as the build does (locals on);
    B  the same with -DREX_LOOP_LOCALS_OFF -DREX_LEAF_LOCALS_OFF: every local is a reference to its ctx register,
       the code as it was before the two tools (their macros' own switch-back, so nothing else changes);
and comparing each function's machine code (objdump -dr) in the two objects.

Per function and form: instructions, loads (ld*, cas*, swp*), stores (st*), sp-relative loads and stores (spills
and stack slots), and a loop-weighted cost: an instruction in a loop weighs 10, every other one 1, and a memory
instruction counts twice: cost = sum(weight * (1 + is_mem)). What is "in a loop" (--loops):
  cfg     (default, the list) the instruction's basic block is on a cycle of the function's control-flow graph
          (a strongly connected component of more than one block, or a block that jumps to itself). Branches with a
          relocation leave the function (tail calls, jumps to a .cold part); `bl`/`blr` return; GCC's jump tables
          (`adr` + `add ..., sxtb/sxth #2` + `br`) are read from the function's .rodata.<symbol> section.
  ranges  the range [target, branch] of every backward branch without a relocation (b, b.cond, cbz/cbnz,
          tbz/tbnz). Simpler, and wrong where GCC puts an exit path after the function's `ret` and jumps back
          from it (sub_82C37078 and sub_821E0090 of 2026-10-16: 30-70 exit instructions counted as loop, which made
          them false losers). Reported next to the cfg numbers for comparison.
A `.cold` part is added to its function (its own loops weighted the same way). Code that can only run after a call
of the trap helper ppc_trap ([[gnu::cold]]; a guest tw/td assert that logs and continues) is left out of the costs:
GCC often copies the rest of the function into the trap path, and that copy differs between the forms for no
run-time effect (sub_824FFFD8: 154 vs 85 instructions, the extra ones mostly such a copy). Static counts: they say
what the compiler made of the function, not how often it runs.

Only the functions the tools changed are judged: those whose generated text has REX_LOOP_LOCAL( (loop_locals.py;
also REX_LOOP_IN / REX_LOOP_OUT) or REX_LEAF_LOCAL( (leaf_locals.py). A function is a loser when B's cost is lower
than A's by more than --threshold percent of A's cost. The opt-out list names those, one per line, prefixed with the
tool whose macros the function has ("loop sub_82XXXXXX" / "leaf sub_82XXXXXX"); the names are DEFINE_REX_FUNC names.

Why exact: the compile command is the build's own (rule command and the edge's DEFINES, INCLUDES and FLAGS from
build.ninja), only re-pointed at the copied generated folder (its -I and the CMake pch wrapper, rewritten to include
the copy's *_pch.h; no .gch, so the pch is parsed as text: the same code, slower). Before the comparison the
instructions of form A of the first file are compared with the build's own object, when it exists (relocation
lines and string offsets aside they must match). Caveat (inferred, not measured per call site): B turns the locals
off in the whole file, so a caller into which GCC inlined a changed leaf changes too; such functions without macros
are counted separately and are not in the list. Their inlined copies follow the callee's choice once the opt-out is
applied by the tools.

  python tools/locals_choice.py --gen out/locals_choice/generated --ninja app/out/switch/build.ninja
                                --out out/locals_choice [--jobs 6] [--threshold 2] [--loops cfg|ranges]
                                [--reuse] [--keep-objects] [--files 0,5,17] [--mode locals|r1|lr]
peggle2-nx (2026-10-19): --mode r1 judges tools/r1_local.py's functions (REX_R1_LOCAL(); form B with
-DREX_R1_LOCAL_OFF) and writes r1_local_choice.json, the functions whose cost with the mirror is LOWER than without by
more than --threshold percent (banjo-nx: the mirror everywhere is a loss, only the per-function choice pays);
--mode lr judges tools/lr_preserve.py's rewritten functions (REX_LR_WRITE_SAME(; form B with -DREX_LR_WRITE_SAME_OFF)
and writes lr_write_same_choice.json, every function whose cost with the rewrite is not higher than without by more
than --threshold percent. The tree must have been processed with that step on every function it takes (r1:
PEGGLE1_R1_LOCAL_ALL=1 tools/post_codegen.py --gen <copy>; lr: no choice list in app/config).
--gen: a COPY of the generated folder (*_recomp.N.cpp, *_funcs.N.h, *_pch.h), so a codegen run elsewhere cannot
change the inputs mid-way. --reuse: keep objects that are newer than their source. Objects (objA/, objB/) are
deleted at the end unless --keep-objects. Writes to --out: locals_optout.txt (the list), locals_choice.csv (every
changed function, both forms) and locals_choice_report.txt (the summary printed here).
Reads build.ninja and its included rules only; writes nothing outside --out. Needs devkitA64 (DEVKITPRO or
C:/devkitPro).
"""
import argparse
import bisect
import concurrent.futures
import datetime
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys

DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''
OBJDUMP = DEVKITA64 + 'aarch64-none-elf-objdump' + EXE
OFF = ['-DREX_LOOP_LOCALS_OFF', '-DREX_LEAF_LOCALS_OFF']
TU = re.compile(r'(\w+)_recomp\.(\d+)\.cpp$')
FUNC = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
HEAD = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
INS = re.compile(r'^\s+([0-9a-f]+):\t(\S+)(?:\t(.*))?$')
RELOC = re.compile(r'^\s+[0-9a-f]+: R_AARCH64_')
BRANCH = re.compile(r'^(?:b|b\.\w+|bc\.\w+|cbn?z|tbn?z)$')
TARGET = re.compile(r'(?:^|[\s,])([0-9a-f]+) <')
LOOP_WEIGHT = 10
COLD_CALL = re.compile(r'R_AARCH64_CALL26\s+_Z8ppc_trap')  # the generated code's trap helper ([[gnu::cold]])


# ---------------------------------------------------------------------------------------------------------------
# build.ninja: the compile command of the generated files

def ninja_unescape(s):
    return re.sub(r'\$([ :$\n])', lambda m: '' if m.group(1) == '\n' else m.group(1), s)


def ninja_text(path, seen=None):
    """build.ninja with its `include`d files appended (rules.ninja), continuation lines joined."""
    seen = seen if seen is not None else set()
    path = os.path.abspath(path)
    if path in seen:
        return ''
    seen.add(path)
    text = open(path, encoding='utf-8').read().replace('$\n', '')
    out = [text]
    for m in re.finditer(r'^(?:include|subninja) (.+)$', text, re.M):
        inc = ninja_unescape(m.group(1).strip())
        if not os.path.isabs(inc):
            inc = os.path.join(os.path.dirname(path), inc)
        if os.path.exists(inc):
            out.append(ninja_text(inc, seen))
    return '\n'.join(out)


def split_cmd(s):
    lex = shlex.shlex(s, posix=True)
    lex.whitespace_split = True
    lex.escape = ''  # Windows paths keep their backslashes
    lex.commenters = ''
    return list(lex)


def norm(p):
    return os.path.normcase(os.path.normpath(p.replace('\\', '/')))


def compile_command(ninja, gen, out):
    """(compiler argv without -o/-c/source, prefix, original generated folder, build object of file 0 or None)."""
    text = ninja_text(ninja)
    edge = None
    for block in re.split(r'\n(?=build )', text):
        m = re.match(r'build ((?:[^:$]|\$.)+): (\S+) ((?:[^ |$]|\$.)+)', block)
        if m and TU.search(ninja_unescape(m.group(3))):
            src = ninja_unescape(m.group(3))
            if edge is None or int(TU.search(src).group(2)) < int(TU.search(edge[2]).group(2)):
                edge = (ninja_unescape(m.group(1)), m.group(2), src, dict(re.findall(r'\n  (\w+) = (.*)', block)))
    if edge is None:
        sys.exit('locals_choice: no build statement for a *_recomp.N.cpp in %s' % ninja)
    obj, rule, src, var = edge
    m = re.search(r'^rule %s\n((?:  .*\n?)*)' % re.escape(rule), text, re.M)
    if not m:
        sys.exit('locals_choice: no rule %s in %s or its includes' % (rule, ninja))
    command = re.search(r'^  command = (.*)$', m.group(1), re.M).group(1)
    command = re.sub(r'\$\{?(LAUNCHER|CODE_CHECK)\}?', '', command)
    command = re.sub(r'\$\{?(\w+)\}?', lambda m: {'out': '@OUT@', 'in': '@IN@'}.get(
        m.group(1), ninja_unescape(var.get(m.group(1), ''))), command)
    argv = split_cmd(command)
    orig_gen = norm(os.path.dirname(src))
    gen = os.path.abspath(gen)
    wrapper = os.path.join(out, 'pch_wrapper.hxx')
    cmd = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ('-MD', '-MMD'):
            pass
        elif a in ('-MT', '-MF', '-o', '-c'):
            i += 1
        elif a == '@IN@':
            pass
        elif a == '-include':
            hxx = argv[i + 1]
            body = open(hxx, encoding='utf-8').read()
            pch = [p for p in re.findall(r'#include "([^"]+)"', body)]
            if not pch or not all(norm(os.path.dirname(p)) == orig_gen for p in pch):
                sys.exit('locals_choice: the forced include %s does not name the generated pch' % hxx)
            for p in pch:
                body = body.replace(p, os.path.join(gen, os.path.basename(p)).replace('\\', '/'))
            open(wrapper, 'w', encoding='utf-8', newline='\n').write(
                '/* tools/locals_choice.py: %s, re-pointed at the copied generated folder */\n' % hxx + body)
            cmd += ['-include', wrapper]
            i += 1
        elif a.startswith('-I') and norm(a[2:] or argv[i + 1]) == orig_gen:
            if a == '-I':
                i += 1
            cmd.append('-I' + gen)
        else:
            cmd.append(a)
        i += 1
    ref = os.path.join(os.path.dirname(os.path.abspath(ninja)), obj)
    return cmd, TU.search(src).group(1), orig_gen, (int(TU.search(src).group(2)), ref) if os.path.exists(ref) else None


# ---------------------------------------------------------------------------------------------------------------
# The generated text: which functions the two tools changed

MODES = {  # peggle2-nx: mode -> (form B's defines, the marker of a function the step changed, its tool label)
    'locals': (OFF, None, None),
    'r1': (['-DREX_R1_LOCAL_OFF'], 'REX_R1_LOCAL()', 'r1'),
    'lr': (['-DREX_LR_WRITE_SAME_OFF'], 'REX_LR_WRITE_SAME(', 'lr'),
    # the census of the whole round (every function: every one has instruction comments); no list is written
    'round': (['-DREX_VPERM1_OFF', '-DREX_FP_SIGN_INTEGER', '-DREX_LEAF_DEAD_KEEP', '-DREX_DEAD_DEFS_KEEP',
               '-DREX_SINGLE_FLOATS_OFF', '-DREX_INLINE_MEMCALLS_OFF', '-DREX_VEC_UNALIGNED_LOOPS',
               '-DREX_LOOP_BARRIER_FULL', '-DREX_RO_CFG_OFF', '-DREX_FLUSH_HOIST_OFF', '-DREX_LR_WRITE_SAME_OFF',
               '-DREX_R1_LOCAL_OFF', '-DREX_VSEL_OFF', '-DREX_INLINE_LEAVES_OFF'], '\t// ', 'all'),
}
MODE = 'locals'


def changed_functions(path):
    """{name: (tool, locals)} for the functions of one file with the macros of loop_locals.py / leaf_locals.py (or,
    with --mode r1 / lr, of r1_local.py / lr_preserve.py: (tool, count of its macro)."""
    found = {}
    name = None
    loop = leaf = 0
    marker, label = MODES[MODE][1], MODES[MODE][2]
    if marker:
        for line in open(path, encoding='utf-8'):
            if name is None:
                m = FUNC.match(line.rstrip('\n'))
                if m:
                    name, loop = m.group(1), 0
                continue
            if line.startswith('}'):
                if loop:
                    found[name] = (label, loop)
                name = None
                continue
            if marker in line and not line.lstrip().startswith('//'):
                loop += line.count(marker)
        return found
    for line in open(path, encoding='utf-8'):
        if name is None:
            m = FUNC.match(line.rstrip('\n'))
            if m:
                name, loop, leaf = m.group(1), 0, 0
            continue
        if line.startswith('}'):
            if loop or leaf:
                found[name] = ('loop+leaf' if loop and leaf else 'loop' if loop else 'leaf', loop + leaf)
            name = None
            continue
        s = line.lstrip()
        if s.startswith('//'):
            continue
        if s.startswith('REX_LOOP_LOCAL('):
            loop += 1
        elif s.startswith('REX_LEAF_LOCAL('):
            leaf += 1
        elif not loop and ('REX_LOOP_IN(' in s or 'REX_LOOP_OUT(' in s):
            loop += 1
    return found


# ---------------------------------------------------------------------------------------------------------------
# The machine code

def parts_of(obj):
    """{symbol: [[address, mnemonic, operands, relocation or None]]} of an object, from objdump -dr."""
    res = subprocess.run([OBJDUMP, '-dr', '--no-show-raw-insn', obj], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, encoding='utf-8', errors='replace')
    if res.returncode:
        raise RuntimeError('objdump %s: %s' % (obj, res.stderr.strip()))
    parts = {}
    cur = None
    for line in res.stdout.split('\n'):
        m = HEAD.match(line)
        if m:
            cur = parts.setdefault(m.group(1), [])
            continue
        if cur is None:
            continue
        if RELOC.match(line):
            if cur:
                cur[-1][3] = line.split(':', 1)[1].strip()
            continue
        m = INS.match(line)
        if m:
            cur.append([int(m.group(1), 16), m.group(2), m.group(3) or '', None])
        elif line.startswith('Disassembly of section'):
            cur = None
    return parts


def rodata_sections(obj):
    """{section name: bytes} of the .rodata.* sections of an ELF64 object (where GCC puts a function's jump tables)."""
    data = open(obj, 'rb').read()
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)

    def header(i):
        name, typ, _, _, off, size = struct.unpack_from('<IIQQQQ', data, shoff + i * shentsize)
        return name, typ, off, size
    _, _, stroff, _ = header(shstrndx)
    out = {}
    for i in range(shnum):
        name, typ, off, size = header(i)
        sname = data[stroff + name:data.index(b'\0', stroff + name)].decode('ascii', 'replace')
        if sname.startswith('.rodata.') and typ == 1:  # SHT_PROGBITS
            out[sname] = data[off:off + size]
    return out


def jump_targets(ins, k, table, offsets):
    """Targets of the `br` at ins[k] when it is GCC's jump-table dispatch
        ldrb/ldrh/ldr wE, [xT, wI, uxtw ...];  adr xA, L;  add xJ, xA, wE, sxtb/sxth/sxtw #2;  br xJ
    read from the function's .rodata section (`table`; `offsets`: the table starts its relocations name). None when
    the `br` is not that shape (an indirect tail jump: no successor in the function); 'unknown' when the table cannot
    be found."""
    reg = ins[k][2].strip()
    add = adr = load = None
    for j in range(k - 1, max(-1, k - 8), -1):
        mn, ops = ins[j][1], ins[j][2]
        if add is None and mn == 'add' and ops.startswith(reg + ','):
            add = re.match(r'\w+, (x\d+), (w\d+), ([su])xt([bhw]) #2$', ops)
            if add is None:
                return None
        elif add is not None and adr is None and mn == 'adr' and ops.startswith(add.group(1) + ','):
            adr = int(TARGET.search(ops).group(1), 16)
        elif add is not None and load is None and mn.startswith('ldr') and ops.startswith(add.group(2) + ','):
            load = re.match(r'w\d+, \[(x\d+), (w\d+|x\d+)', ops)
    if add is None or adr is None:
        return None
    if table is None or load is None:
        return 'unknown'
    width = {'b': 1, 'h': 2, 'w': 4}[add.group(4)]
    signed = add.group(3) == 's'
    # Which table: the nearest earlier (else any later) `add xT, xT, :lo12:.rodata.<function>+off` of the base.
    base = load.group(1)
    start = None
    for j in list(range(k - 1, -1, -1)) + list(range(len(ins) - 1, k, -1)):
        mn, ops, rel = ins[j][1], ins[j][2], ins[j][3]
        if mn == 'add' and ops.startswith(base + ',') and rel and 'ADD_ABS_LO12_NC' in rel and '.rodata.' in rel:
            m = re.search(r'\+0x([0-9a-f]+)$', rel)
            start = int(m.group(1), 16) if m else 0
            break
    if start is None and len(offsets) <= 1:  # the base came from a stack slot, but the function has one table
        start = offsets[0] if offsets else 0
    if start is None:
        return 'unknown'
    later = [o for o in offsets if o > start]
    end = min(later) if later else len(table)
    count = (end - start) // width
    # The bound check in front of the dispatch (cmp wI, #N; b.hi default) gives the number of entries.
    for j in range(k - 1, max(-1, k - 12), -1):
        m = re.match(r'(w\d+), #(0x[0-9a-f]+|\d+)', ins[j][2])
        if ins[j][1] == 'cmp' and m:
            count = min(count, int(m.group(2), 0) + 1)
            break
    fmt = {1: 'b', 2: 'h', 4: 'i'}[width]
    fmt = fmt if signed else fmt.upper()
    if start + width * count > len(table):
        return 'unknown'  # a table that ends before its count (2026-10-16: seen in an args_in_registers trial build)
    return {adr + 4 * struct.unpack_from('<' + fmt, table, start + width * e)[0] for e in range(count)}


def loop_flags(ins, table):
    """Per instruction: is it in a cycle of the part's control-flow graph (a block of a strongly connected component
    with more than one block, or with an edge to itself)? A branch with a relocation leaves the part (a tail call, a
    jump to or from a .cold part); `bl`/`blr` return; `ret` and indirect tail jumps end a path; jump tables are
    read. Also the number of jump tables that could not be read (their `br` then counts as able to go anywhere), and
    per instruction whether it can run without a call of the cold trap helper before it on its path (GCC copies the
    code after a guest trap into the trap path: such copies never run in a game that does not trap)."""
    n = len(ins)
    if not n:
        return [], 0, []
    index = {a[0]: k for k, a in enumerate(ins)}
    offsets = sorted({int(m.group(1), 16) if m.group(1) else 0 for i in ins if i[3] and '.rodata.' in i[3]
                      for m in [re.search(r'(?:\+0x([0-9a-f]+))?$', i[3])]})
    succ = {}
    leaders = {0}
    unresolved = 0
    trap_calls = set()
    for k, (addr, mn, ops, rel) in enumerate(ins):
        if mn in ('ret', 'eret', 'udf', 'brk', 'hlt'):
            succ[k] = []
        elif mn == 'br':
            t = jump_targets(ins, k, table, offsets)
            if t == 'unknown':
                unresolved += 1
                succ[k] = list(range(n))  # conservative: anywhere
            else:
                succ[k] = sorted(index[x] for x in (t or ()) if x in index)
        elif BRANCH.match(mn):
            t = TARGET.search(ops)
            inside = [] if rel or not t or int(t.group(1), 16) not in index else [index[int(t.group(1), 16)]]
            succ[k] = inside + ([] if mn == 'b' else [k + 1] if k + 1 < n else [])
        elif mn == 'bl' and rel and COLD_CALL.search(rel):
            succ[k] = [k + 1] if k + 1 < n else []
            trap_calls.add(k)
        else:
            continue
        leaders.update(succ[k])
        if k + 1 < n:
            leaders.add(k + 1)
    starts = sorted(leaders)
    block_of = [0] * n
    for b, s in enumerate(starts):
        for k in range(s, starts[b + 1] if b + 1 < len(starts) else n):
            block_of[k] = b
    edges = []
    for b, s in enumerate(starts):
        last = (starts[b + 1] if b + 1 < len(starts) else n) - 1
        if last in succ:
            edges.append(sorted({block_of[x] for x in succ[last]}))
        else:
            edges.append([b + 1] if b + 1 < len(starts) else [])
    # Tarjan's strongly connected components, iteratively.
    nb = len(starts)
    idx = [-1] * nb
    low = [0] * nb
    on = [False] * nb
    stack = []
    cyclic = [False] * nb
    counter = 0
    for root in range(nb):
        if idx[root] >= 0:
            continue
        work = [(root, 0)]
        idx[root] = low[root] = counter
        counter += 1
        stack.append(root)
        on[root] = True
        while work:
            v, i = work[-1]
            if i < len(edges[v]):
                work[-1] = (v, i + 1)
                w = edges[v][i]
                if idx[w] < 0:
                    idx[w] = low[w] = counter
                    counter += 1
                    stack.append(w)
                    on[w] = True
                    work.append((w, 0))
                elif on[w]:
                    low[v] = min(low[v], idx[w])
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[v])
            if low[v] == idx[v]:
                comp = []
                while True:
                    w = stack.pop()
                    on[w] = False
                    comp.append(w)
                    if w == v:
                        break
                if len(comp) > 1 or v in edges[v]:
                    for w in comp:
                        cyclic[w] = True
    # Blocks reachable from the entry without passing a call of the trap helper.
    warm = [False] * nb
    todo = [0]
    warm[0] = True
    while todo:
        b = todo.pop()
        last = (starts[b + 1] if b + 1 < nb else n) - 1
        if last in trap_calls:
            continue
        for w in edges[b]:
            if not warm[w]:
                warm[w] = True
                todo.append(w)
    return [cyclic[block_of[k]] for k in range(n)], unresolved, [warm[block_of[k]] for k in range(n)]


def range_flags(ins):
    """Per instruction: is it inside [target, branch] of a backward branch without a relocation?"""
    addrs = [i[0] for i in ins]
    depth = [0] * (len(ins) + 1)  # coverage of the ranges, as a difference array over the instructions
    for k, (addr, mn, ops, rel) in enumerate(ins):
        if not rel and BRANCH.match(mn):
            t = TARGET.search(ops)
            if t and int(t.group(1), 16) <= addr:
                depth[bisect.bisect_left(addrs, int(t.group(1), 16))] += 1
                depth[k + 1] -= 1
    flags = []
    cover = 0
    for k in range(len(ins)):
        cover += depth[k]
        flags.append(cover > 0)
    return flags


def measure(ins, table, method):
    """Counts of one part: instructions, loads, stores, sp loads, sp stores, loop instructions and cost by `method`
    ('cfg' or 'ranges'), the same two by the other method, jump tables that could not be read, instructions only on
    a trap path (left out of both costs)."""
    cfg, unresolved, warm = loop_flags(ins, table)
    rng = range_flags(ins)
    first, second = (cfg, rng) if method == 'cfg' else (rng, cfg)
    n = ld = st = spld = spst = in1 = c1 = in2 = c2 = trap = 0
    for k, (addr, mn, ops, rel) in enumerate(ins):
        n += 1
        if not warm[k]:
            trap += 1
        load = mn.startswith('ld') or mn.startswith('cas') or mn.startswith('swp')
        store = mn.startswith('st')
        sp = '[sp' in ops
        ld += load
        st += store
        spld += load and sp
        spst += store and sp
        mem = (1 + (load or store)) * warm[k]
        in1 += first[k] and warm[k]
        c1 += (LOOP_WEIGHT if first[k] else 1) * mem
        in2 += second[k] and warm[k]
        c2 += (LOOP_WEIGHT if second[k] else 1) * mem
    return [n, ld, st, spld, spst, in1, c1, in2, c2, unresolved, trap]


def function_costs(obj, method):
    """{function name (without __imp__): counts} with .cold parts added to their function."""
    out = {}
    tables = rodata_sections(obj)
    for sym, ins in parts_of(obj).items():
        name = sym[len('__imp__'):] if sym.startswith('__imp__') else sym
        base = sym[:-len('.cold')] if sym.endswith('.cold') else sym
        if name.endswith('.cold'):
            name = name[:-len('.cold')]
        c = measure(ins, tables.get('.rodata.' + base), method)
        if name in out:
            out[name] = [a + b for a, b in zip(out[name], c)]
        else:
            out[name] = c
    return out


def instruction_stream(obj):
    """Instructions of an object without relocation lines and addresses of literal strings (for the self-check)."""
    return {s: [(a, m, re.sub(r'\.str[\w.]*\+0x[0-9a-f]+', '.str', o)) for a, m, o, _ in ins]
            for s, ins in parts_of(obj).items()}


# ---------------------------------------------------------------------------------------------------------------

class Row:
    """One function with locals: its counts in form A (on) and B (off)."""
    def __init__(self, name, file, tool, locals_, A, B):
        self.name, self.file, self.tool, self.locals, self.A, self.B = name, file, tool, locals_, A, B

    def diff(self, key):
        return self.A[key] - self.B[key]

    def pct(self, key):
        return 100.0 * self.diff(key) / self.A[key] if self.A[key] else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True, help='COPY of the generated folder')
    ap.add_argument('--ninja', required=True, help="the Switch build's build.ninja (read only)")
    ap.add_argument('--out', required=True, help='output folder (objects, list, csv, report)')
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--threshold', type=float, default=2.0, help='percent of the locals form\'s cost (default 2)')
    ap.add_argument('--reuse', action='store_true', help='keep objects newer than their source')
    ap.add_argument('--keep-objects', action='store_true')
    ap.add_argument('--files', help='only these file numbers, e.g. 0,5,17 (a trial)')
    ap.add_argument('--mode', choices=sorted(MODES), default='locals',
                    help='which step to judge (peggle2-nx): the loop / leaf locals, r1_local.py or lr_preserve.py')
    ap.add_argument('--loops', choices=('cfg', 'ranges'), default='cfg',
                    help='what is a loop for the list: cycles of the control-flow graph (default) or '
                         'backward-branch ranges (both are reported)')
    a = ap.parse_args()
    global MODE
    MODE = a.mode
    form_b = MODES[MODE][0]
    out = os.path.abspath(a.out)
    gen = os.path.abspath(a.gen)
    if norm(out).startswith(norm(gen)) or norm(gen) == norm(out):
        sys.exit('locals_choice: --out must not be inside --gen')
    obj_b = 'objB' if MODE == 'locals' else 'objB_' + MODE  # peggle2-nx: the modes share form A (--reuse)
    for d in ('objA', obj_b):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    cmd, prefix, orig_gen, ref = compile_command(a.ninja, gen, out)
    if norm(orig_gen) == norm(gen):
        print('locals_choice: WARNING: --gen is the build\'s own generated folder, not a copy')
    files = sorted((int(TU.search(f).group(2)), f) for f in os.listdir(gen)
                   if TU.search(f) and TU.search(f).group(1) == prefix)
    if a.files:
        want = {int(x) for x in a.files.split(',')}
        files = [(n, f) for n, f in files if n in want]
    changed = {}
    for n, f in files:
        c = changed_functions(os.path.join(gen, f))
        if c:
            changed[n] = c
    total_loop = sum(1 for c in changed.values() for t, _ in c.values() if 'loop' in t)
    total_leaf = sum(1 for c in changed.values() for t, _ in c.values() if 'leaf' in t)
    print('locals_choice: %d of %d files have functions with locals (%d loop_locals, %d leaf_locals)' %
          (len(changed), len(files), total_loop, total_leaf))
    print('  compile: %s' % subprocess.list2cmdline(cmd + ['-o', '<obj>', '-c', '<file>']))
    print('  form B adds: %s' % ' '.join(form_b))

    def build(n):
        src = os.path.join(gen, '%s_recomp.%d.cpp' % (prefix, n))
        res = []
        for form, extra in (('A', []), ('B', form_b)):
            obj = os.path.join(out, 'objA' if form == 'A' else obj_b, '%d.o' % n)
            if a.reuse and os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src):
                res.append(obj)
                continue
            p = subprocess.run(cmd + extra + ['-o', obj, '-c', src], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding='utf-8', errors='replace')
            if p.returncode:
                raise RuntimeError('compile %s (%s):\n%s' % (src, form, p.stdout[-4000:]))
            if p.stdout.strip():
                print('  %s (%s): %s' % (os.path.basename(src), form, p.stdout.strip()[:500]))
            res.append(obj)
        return n, function_costs(res[0], a.loops), function_costs(res[1], a.loops)

    results = {}
    start = datetime.datetime.now()
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for k, fut in enumerate(concurrent.futures.as_completed([ex.submit(build, n) for n in sorted(changed)])):
            n, ca, cb = fut.result()
            results[n] = (ca, cb)
            if (k + 1) % 10 == 0 or k + 1 == len(changed):
                print('  %d / %d files (%s)' % (k + 1, len(changed), str(datetime.datetime.now() - start)[:7]),
                      flush=True)

    # Self-check: form A of the first file against the build's own object.
    lines = []
    if ref and ref[0] in results:
        mine = instruction_stream(os.path.join(out, 'objA', '%d.o' % ref[0]))
        theirs = instruction_stream(ref[1])
        lines.append('self-check: form A of %s_recomp.%d.cpp %s the build\'s object (%s)' % (
            prefix, ref[0], 'has the same instructions as' if mine == theirs else 'DIFFERS FROM', ref[1]))
    else:
        lines.append('self-check: skipped (no build object of the first file, or it has no locals)')

    rows = []
    collateral = []
    for n, (ca, cb) in sorted(results.items()):
        for name in sorted(set(ca) & set(cb)):
            if name in changed[n]:
                tool, nloc = changed[n][name]
                rows.append(Row(name, n, tool, nloc, ca[name], cb[name]))
            elif ca[name][:5] != cb[name][:5] or ca[name][6] != cb[name][6]:
                collateral.append((name, n, ca[name][6] - cb[name][6]))
        for name in changed[n]:
            if name not in ca or name not in cb:
                lines.append('missing in an object: %s (file %d)' % (name, n))
    other = 'ranges' if a.loops == 'cfg' else 'cfg'

    def summary(rows, key, label):
        losers = sorted((r for r in rows if r.pct(key) > a.threshold), key=lambda r: -r.diff(key))
        winners = [r for r in rows if r.pct(key) < -a.threshold]
        tot_a = sum(r.A[key] for r in rows)
        tot_b = sum(r.B[key] for r in rows)
        gain = sum(r.diff(key) for r in losers)

        def count(rs, tool):
            return sum(1 for r in rs if tool in r.tool)
        return losers, [
            'loop-weighted cost (%s), all of them: locals on %d, off %d (on - off = %+d, %+.2f %% of off)' % (
                label, tot_a, tot_b, tot_a - tot_b, 100.0 * (tot_a - tot_b) / tot_b if tot_b else 0),
            '  losers (off cheaper by more than %.1f %% of on): %d (loop %d, leaf %d); on - off = %+d (on %d, off %d)'
            % (a.threshold, len(losers), count(losers, 'loop'), count(losers, 'leaf'), gain,
               sum(r.A[key] for r in losers), sum(r.B[key] for r in losers)),
            '  winners (on cheaper by more than %.1f %%): %d (loop %d, leaf %d); off - on = %+d' % (
                a.threshold, len(winners), count(winners, 'loop'), count(winners, 'leaf'),
                -sum(r.diff(key) for r in winners)),
            '  within +-%.1f %%: %d (identical cost: %d)' % (
                a.threshold, len(rows) - len(losers) - len(winners), sum(1 for r in rows if r.diff(key) == 0)),
            '  with the losers off: total %d -> %d (%+.2f %%)' % (tot_a, tot_a - gain,
                                                                 -100.0 * gain / tot_a if tot_a else 0)]

    def count(rs, tool):
        return sum(1 for r in rs if tool in r.tool)
    lines.append('functions with locals compared: %d (loop_locals %d, leaf_locals %d); identical instructions in '
                 'both forms: %d' % (len(rows), count(rows, 'loop'), count(rows, 'leaf'),
                                     sum(1 for r in rows if r.A[:5] == r.B[:5] and r.A[6] == r.B[6])))
    lines.append('instructions, all of them: on %d, off %d; sp loads+stores: on %d, off %d' % (
        sum(r.A[0] for r in rows), sum(r.B[0] for r in rows), sum(r.A[3] + r.A[4] for r in rows),
        sum(r.B[3] + r.B[4] for r in rows)))
    lines.append('instructions only on a trap path (after a call of ppc_trap; not in the costs): on %d, off %d' % (
        sum(r.A[10] for r in rows), sum(r.B[10] for r in rows)))
    losers, text = summary(rows, 6, 'loops = ' + a.loops + ', the list')
    lines += text
    other_losers, text = summary(rows, 8, 'loops = ' + other + ', for comparison')
    lines += text
    both = {r.name for r in losers} & {r.name for r in other_losers}
    lines.append('losers by both loop methods: %d; only by %s: %d; only by %s: %d' % (
        len(both), a.loops, len(losers) - len(both), other, len(other_losers) - len(both)))
    lines.append('jump tables that could not be read (their br counted as going anywhere): %d in %d functions' % (
        sum(r.A[9] + r.B[9] for r in rows), sum(1 for r in rows if r.A[9] or r.B[9])))
    lines.append('functions without locals whose code changed (inlined callees with locals, inferred): %d, '
                 'on - off = %+d' % (len(collateral), sum(c[2] for c in collateral)))
    lines += ['', 'top losers (%s): cost on / off, on - off, %% of on; the same by %s; instructions on / off; '
              'sp loads+stores on / off; locals:' % (a.loops, other)]
    for r in losers[:30]:
        lines.append('  %-4s %-26s f%-3d cost %7d / %7d %+7d %5.1f %%  (%s %+7d)  ins %5d / %5d  sp %4d / %4d  '
                     'locals %d' % (r.tool, r.name, r.file, r.A[6], r.B[6], r.diff(6), r.pct(6), other[:3],
                                    r.diff(8), r.A[0], r.B[0], r.A[3] + r.A[4], r.B[3] + r.B[4], r.locals))
    report = '\n'.join(lines) + '\n'
    print(report, end='')
    open(os.path.join(out, 'locals_choice_report.txt'), 'w', encoding='utf-8', newline='\n').write(
        'tools/locals_choice.py, %s\ncompile: %s\nform B adds: %s\n\n' % (
            datetime.date.today().isoformat(), subprocess.list2cmdline(cmd + ['-o', '<obj>', '-c', '<file>']),
            ' '.join(form_b)) + report)

    with open(os.path.join(out, 'locals_choice.csv'), 'w', encoding='utf-8', newline='\n') as f:
        keys = ['ins', 'loads', 'stores', 'sp_loads', 'sp_stores', 'loop_ins_' + a.loops, 'cost_' + a.loops,
                'loop_ins_' + other, 'cost_' + other, 'unread_tables', 'trap_path_ins']
        f.write('function,file,tool,locals,' + ','.join('%s_on,%s_off' % (k, k) for k in keys) +
                ',cost_on_minus_off,percent_of_on,optout\n')
        for r in sorted(rows, key=lambda r: -r.diff(6)):
            f.write('%s,%d,%s,%d,%s,%d,%.2f,%d\n' % (r.name, r.file, r.tool, r.locals, ','.join(
                '%d,%d' % (r.A[k], r.B[k]) for k in range(len(keys))), r.diff(6), r.pct(6),
                r.pct(6) > a.threshold))

    if MODE == 'round':
        if not a.keep_objects:
            for d in ('objA', obj_b):
                shutil.rmtree(os.path.join(out, d), ignore_errors=True)
        return
    if MODE != 'locals':
        import json
        if MODE == 'r1':   # the winners: the mirror cheaper by more than the threshold
            chosen = sorted(r.name for r in rows if r.pct(6) < -a.threshold)
        else:              # every rewrite that is not a loss by more than the threshold
            chosen = sorted(r.name for r in rows if r.pct(6) <= a.threshold)
        name = 'r1_local_choice.json' if MODE == 'r1' else 'lr_write_same_choice.json'
        json.dump(chosen, open(os.path.join(out, name), 'w', encoding='utf-8'), indent=0)
        print('locals_choice (--mode %s): %d of %d functions chosen -> %s; on - off of the chosen: %+d' % (
            MODE, len(chosen), len(rows), name, sum(r.diff(6) for r in rows if r.name in set(chosen))))
        if not a.keep_objects:
            for d in ('objA', obj_b):
                shutil.rmtree(os.path.join(out, d), ignore_errors=True)
        return
    with open(os.path.join(out, 'locals_optout.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Guest functions whose A57 code is cheaper WITHOUT the loop / leaf locals (tools/locals_choice.py,\n'
                '# %s; marathon-nx proposal 3). One per line: "<tool> <DEFINE_REX_FUNC name>", tool = loop\n'
                '# (tools/loop_locals.py: REX_LOOP_LOCAL) or leaf (tools/leaf_locals.py: REX_LEAF_LOCAL).\n'
                '# Rule: every generated file with locals compiled twice with the Switch build\'s command line, as\n'
                '# built and with -DREX_LOOP_LOCALS_OFF -DREX_LEAF_LOCALS_OFF; per function (objdump -dr) the\n'
                '# loop-weighted cost sum(w * (1 + is_mem)), w = %d for an instruction in a loop, else 1;\n'
                '# loops = %s;\n'
                '# code that only runs after a call of ppc_trap (GCC\'s copies into the trap path) left out.\n'
                '# Listed when the off form is cheaper by more than %.1f %% of the on form: %d of %d functions\n'
                '# with locals, on - off = %+d. Static counts of the compiled code, not run-time samples.\n'
                % (datetime.date.today().isoformat(), LOOP_WEIGHT,
                   'cycles of the control-flow graph (jump tables read)' if a.loops == 'cfg' else
                   'ranges [target, branch] of backward branches', a.threshold, len(losers), len(rows),
                   sum(r.diff(6) for r in losers)))
        for r in sorted(losers, key=lambda r: (r.tool, r.name)):
            for tool in r.tool.split('+'):
                f.write('%s %s\n' % (tool, r.name))
    if not a.keep_objects:
        for d in ('objA', obj_b):
            shutil.rmtree(os.path.join(out, d), ignore_errors=True)
    print('locals_choice: wrote locals_optout.txt, locals_choice.csv, locals_choice_report.txt in %s' % out)


if __name__ == '__main__':
    main()
