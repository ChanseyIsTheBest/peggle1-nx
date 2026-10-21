#!/usr/bin/env python3
"""Checks a Switch build before it is packaged (from sasrt-nx's check_switch_build.py and nfsmw-nx's lessons).

  python tools/check_switch_build.py [--elf out/nx/peggle1] [--nro out/nx/peggle1.nro]

Checks (exit 1 if any fails):
  - _start is at address 0 (nfsmw-nx: a section-ordering file that moved the start-up code made the NRO unbootable);
  - the build names the optimised codegen flavour (the only one delivered);
  - the NRO links the nfsmw NVK driver of the SDK (its "Mesa x.y.z" string; the generic portlibs Mesa once got in);
  - branch-to-self loops in recompiled functions are only in the functions with a known guest halt (KNOWN_HALTS; a
    polling loop GCC turned into "b ." with non-volatile memory means a missing loop barrier);
  - no vector fmla/fmls in recompiled functions: the generated code has no vector fused multiply-add (VMX multiply-adds
    are a multiply and an add), so any would be contraction that leaked past -ffp-contract=off (e.g. at an LTO link).
    Scalar fmadd/fmsub/fnmadd/fnmsub come from the guest's own std::fma and are only counted.
  - no table lookup that uses one register as table and as index (tbl vN, {vN}, vN) in recompiled functions. No
    generated statement permutes a vector by itself; GCC writes that when a register it needs was given to something
    else. Console, 2026-10-13: the early register allocation pass of devkitA64's GCC 16.1 (-mearly-ra) put data loads
    into the register holding the byte-reversal constant in the two unaligned streaming copies (sub_8260E9C0,
    sub_8260E164); every atlas copied through them was garbage. A canary, not a proof: it also failed on the first
    attempted fix (vector registers as references), which is how that was found not to be one.
  - every compile command of the build (build.ninja next to the ELF) and the link have -mearly-ra=none: that pass
    stays off for the console (app/CMakeLists.txt).
  - the two streaming copies, compiled alone with the build's own command (tools/switch/compile_one_function.py),
    give every table lookup the constant it needs on every path (tools/switch/check_lookup_constants.py: a data-flow
    over the assembly; 90 of 97 lookups fail it with the pass on).
"""
import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The functions whose original PowerPC code has a "b ." halt. Optimisation may duplicate a halt instruction, so the
# check is by function: a branch-to-self anywhere else is a polling loop that lost its barrier.
# peggle1-nx (2026-10-21, the first Switch build; tools/ppc_dis.py): Peggle 1's seven, all in the network libraries'
# error paths (`lbz r11,2145(r3)`; clear it if set; `b .`): 0x8263B494, 0x8263DD9C, 0x82640060, 0x826454E4,
# 0x82645C84 and 0x82645EB0, 0x82647D64, 0x826480E8. (Peggle 2's were sub_8242E838 and sub_82527248.)
KNOWN_HALTS = ['sub_8263B480', 'sub_8263DD30', 'sub_8263FFC0', 'sub_82645340', 'sub_82645B70', 'sub_826477E8',
               'sub_82648018']
# Vector fused multiply-adds that come from the guest's OWN fused operations (fmadds -> std::fma), two of them paired
# into one vector instruction by GCC (exact: each lane is the same fused operation). Checked in the disassembly:
# fcvtl to double, fmla v.2d, fcvtn back, as double(float(std::fma(a, b, c))) twice.
KNOWN_VECTOR_FMA = {'sub_823F6360': 'two std::fma of the localized copy n_823F6360 (first LTO + PGO build, 2026-10-14)'}
# The game's streaming copies for an unaligned source (65 lvx128, one lvsl, 72 vperm128 per kilobyte): the functions
# GCC's early register allocation pass miscompiled (2026-10-13). They are compiled alone and their lookups checked.
# peggle1-nx (2026-10-21): Peggle 1's two (72 vperm each, with lvsl: the same shape); Peggle 2's were sub_8260E9C0 and
# sub_8260E164.
STREAMING_COPIES = ['sub_82467580', 'sub_82466D24']
DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''


def generated_calls():
    """name -> the recompiled functions its generated C++ body calls directly (app/generated/*/ *_recomp.*.cpp)."""
    import glob
    out = {}
    head = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\) \{$', re.M)
    call = re.compile(r'\b(?:__imp__)?(sub_[0-9A-F]{8})\(ctx, base\)')
    for path in glob.glob(os.path.join(ROOT, 'app', 'generated', '*', '*_recomp.*.cpp')):
        text = open(path, encoding='utf-8', errors='replace').read()
        heads = list(head.finditer(text))
        for i, m in enumerate(heads):
            end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
            out[m.group(1)] = set(call.findall(text, m.end(), end))
    return out


def chain_to(calls, f, targets, depth=4, seen=()):
    """The shortest-first chain of direct calls from f to one of the targets, at most `depth` calls long, or None."""
    callees = calls.get(f, ())
    hit = sorted(callees & targets)
    if hit:
        return [f, hit[0]]
    if depth > 1:
        for c in sorted(callees):
            if c not in seen:
                rest = chain_to(calls, c, targets, depth - 1, seen + (f,))
                if rest:
                    return [f] + rest
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', default=os.path.join(ROOT, 'out', 'nx', 'peggle1'))
    ap.add_argument('--nro', default=os.path.join(ROOT, 'out', 'nx', 'peggle1.nro'))
    ap.add_argument('--halts', default=','.join(KNOWN_HALTS),
                    help='the recompiled functions with a genuine guest halt (b . in the original PowerPC code)')
    ap.add_argument('--ninja', default=None, help='the build.ninja of the build (default: next to the ELF)')
    ap.add_argument('--no-lookups', action='store_true',
                    help='skip compiling the two streaming copies alone (about 40 s) for the lookup-constant check')
    ap.add_argument('--sdk-vulkan', default='C:/devkitPro/mesa-switch-main-nfsmw-source/mesa-switch-main/'
                    'mesa-unified-install/opt/devkitpro/portlibs/switch/lib/libvulkan.a')
    a = ap.parse_args()
    failures = []

    nm = subprocess.run([DEVKITA64 + 'aarch64-none-elf-nm' + EXE, a.elf], capture_output=True, text=True).stdout
    start = next((int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' _start')), None)
    print(f'_start at {start:#x}' if start is not None else '_start not found')
    if start != 0:
        failures.append('_start is not at address 0')

    nro = open(a.nro, 'rb').read()
    if b'optimised codegen' not in nro and b'optimised+perf' not in nro:  # sa2-nx's wording / peggle2-nx's profile
        failures.append('the NRO does not name the optimised codegen flavour')
    else:
        print('flavour: optimised codegen')
    mesa = sorted(set(re.findall(rb'Mesa \d+\.\d+\.\d+ \(git-[0-9a-f]+\)', nro)))
    # 2026-10-20: the Switch build carries no Vulkan driver unless its CMake cache says REXGLUE_SWITCH_LINK_NVK=ON.
    cache = os.path.join(os.path.dirname(os.path.abspath(a.elf)), 'CMakeCache.txt')
    link_nvk = True
    if os.path.exists(cache):
        m = re.search(r'^REXGLUE_SWITCH_LINK_NVK:BOOL=(\w+)', open(cache, encoding='utf-8', errors='replace').read(), re.M)
        link_nvk = bool(m) and m.group(1).upper() in ('ON', 'TRUE', '1', 'YES')
    if link_nvk:
        sdk = sorted(set(re.findall(rb'Mesa \d+\.\d+\.\d+ \(git-[0-9a-f]+\)', open(a.sdk_vulkan, 'rb').read())))
        print('Mesa in the NRO:', [m.decode() for m in mesa], '| in the SDK:', [m.decode() for m in sdk])
        if not mesa or mesa != sdk:
            failures.append('the NRO does not carry the SDK driver\'s Mesa string')
    else:
        print('Vulkan driver: none (REXGLUE_SWITCH_LINK_NVK OFF); Mesa strings in the NRO:', [m.decode() for m in mesa])
        if mesa:
            failures.append('the NRO carries a Mesa driver although the build links none')

    # One streaming pass over the disassembly: per function, b-to-self and fused multiply-adds.
    proc = subprocess.Popen([DEVKITA64 + 'aarch64-none-elf-objdump' + EXE, '-d', '--no-show-raw-insn', a.elf],
                            stdout=subprocess.PIPE, text=True, errors='replace', bufsize=1 << 20)
    func = None
    self_loops = []
    vector_fma = collections.Counter()
    scalar_fma = 0
    undefined_tbl = collections.Counter()
    same_tbl = re.compile(r'^v\d+\.16b, \{v(\d+)\.16b\}, v(\d+)\.16b')
    head = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
    ins = re.compile(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$')
    for line in proc.stdout:
        m = head.match(line)
        if m:
            func = m.group(1)
            continue
        m = ins.match(line)
        if not m or not func:
            continue
        addr, op, args = m.groups()
        guest = func.startswith(('sub_8', '__imp__sub_8'))
        if op == 'b' and args.split(' ')[0] == addr:
            if guest:
                self_loops.append(func)
        elif op == 'tbl' and guest:
            t = same_tbl.match(args)
            if t and t.group(1) == t.group(2):
                undefined_tbl[func] += 1
        elif op in ('fmla', 'fmls') and guest:
            vector_fma[func] += 1
        elif op in ('fmadd', 'fmsub', 'fnmadd', 'fnmsub') and guest:
            scalar_fma += 1
    proc.wait()
    known = set(a.halts.split(','))
    functions = {f.replace('__imp__', '') for f in self_loops}
    unknown = sorted(functions - known)
    # Round 7: GCC may inline a known halt into a caller (non_volatile_as_local made the callees smaller: the halts of
    # sub_828DB988 and sub_8293FA08 then appear in sub_828D71E8 and sub_8293FE28), and with a higher inline limit
    # that caller into its own caller (sub_828C5D60 inlines sub_828D71E8). A function whose generated body reaches
    # a known-halt function through at most four direct calls is accepted, and named with the chain.
    inlined = []
    if unknown:
        calls = generated_calls()
        for f in list(unknown):
            chain = chain_to(calls, f, known)
            if chain:
                inlined.append(f'{f} (inlines {" -> ".join(chain[1:])})')
                unknown.remove(f)
    print(f'branch-to-self in recompiled functions: {len(self_loops)} instructions in {len(functions)} functions, '
          f'{len(functions & known)} of them known guest halts'
          + (f'; {len(inlined)} more calling one that GCC inlined: {"; ".join(inlined)}' if inlined else ''))
    if unknown:
        failures.append('branch-to-self outside the known guest halts (a polling loop without its barrier?): '
                        + ', '.join(unknown[:20]))
    explained = {f: n for f, n in vector_fma.items() if f.replace('__imp__', '') in KNOWN_VECTOR_FMA}
    for f in explained:
        del vector_fma[f]
    print(f'fused multiply-adds in recompiled functions: scalar {scalar_fma} (the guest\'s own), vector '
          f'{sum(vector_fma.values())} in {len(vector_fma)} functions (must be 0)'
          + (f'; known pairs of the guest\'s own std::fma: {", ".join(f"{k} x{v}" for k, v in explained.items())}'
             if explained else ''))
    if vector_fma:
        failures.append('vector fmla/fmls in recompiled code (contraction leaked): '
                        + ', '.join(f'{k} x{v}' for k, v in vector_fma.most_common(10)))
    print(f'table lookups with the table as its own index in recompiled functions: {sum(undefined_tbl.values())} '
          f'in {len(undefined_tbl)} functions (must be 0)')
    if undefined_tbl:
        failures.append('tbl with the same register as table and index (a register GCC needed was given away: is '
                        '-mearly-ra=none missing?): ' + ', '.join(f'{k} x{v}' for k, v in
                                                                     undefined_tbl.most_common(10)))
    # The early register allocation pass must be off in every compile command and at the link.
    ninja = a.ninja or os.path.join(os.path.dirname(os.path.abspath(a.elf)), 'build.ninja')
    if os.path.exists(ninja):
        compiles = without = links = links_without = 0
        missing = collections.Counter()
        kind = target = None
        for line in open(ninja, encoding='utf-8', errors='replace'):
            if line.startswith('build '):
                m = re.search(r': (?:C|CXX)_COMPILER__(\S+?)_unscanned_', line)
                kind = 'compile' if m else 'link' if re.search(r': (?:C|CXX)_EXECUTABLE_LINKER__', line) else None
                target = m.group(1) if m else None
            elif kind == 'compile' and line.startswith('  FLAGS = '):
                compiles += 1
                if '-mearly-ra=none' not in line:
                    without += 1
                    missing[target] += 1
            elif kind == 'link' and line.startswith('  LINK_FLAGS = '):
                links += 1
                links_without += '-mearly-ra=none' not in line
        print(f'-mearly-ra=none: in {compiles - without} of {compiles} compile commands and {links - links_without} '
              f'of {links} links')
        if not compiles or without or not links or links_without:
            failures.append('commands without -mearly-ra=none (GCC\'s early register allocation pass miscompiled the '
                            'streaming copies): ' + ', '.join(f'{k} x{v}' for k, v in missing.most_common(8))
                            + (f'; {links_without} of {links} links' if links_without or not links else ''))
    else:
        print(f'-mearly-ra=none: not checked (no {ninja})')

    # The two streaming copies compiled alone with the build's own command: every lookup must get its constant.
    if os.path.exists(ninja) and not a.no_lookups:
        here = os.path.dirname(os.path.abspath(__file__))
        build = os.path.dirname(ninja)
        files = []
        for f in STREAMING_COPIES:
            # -fno-lto: with LTO the build's own command makes LTO bytecode, no instructions to check (2026-10-14);
            # the same compiler flags otherwise (the link has -mearly-ra=none too, checked above).
            run = subprocess.run([sys.executable, '-P', '-E', os.path.join(here, 'switch', 'compile_one_function.py'), f,
                                  '--build', os.path.abspath(build), '--', '-fno-lto'], capture_output=True, text=True)
            if run.returncode:
                failures.append(f'{f} did not compile alone: {run.stderr[-300:] or run.stdout[-300:]}')
            else:
                files.append(os.path.join(build, 'one_function', f + '_build.s'))
        run = subprocess.run([sys.executable, '-P', '-E', os.path.join(here, 'switch', 'check_lookup_constants.py')]
                             + files, capture_output=True, text=True)
        print(run.stdout.rstrip())
        if run.returncode or len(files) != len(STREAMING_COPIES):
            failures.append('a table lookup of a streaming copy does not get its constant (tools/switch/'
                            'check_lookup_constants.py)')
    if failures:
        print('\nFAILED:\n  - ' + '\n  - '.join(failures))
        sys.exit(1)
    print('\nall checks passed')


if __name__ == '__main__':
    main()
