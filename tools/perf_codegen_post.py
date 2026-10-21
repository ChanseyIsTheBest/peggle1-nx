#!/usr/bin/env python3
"""Post-codegen performance passes for app/generated/default (from futari-nx's tool of the same name, which took them
from masseffect-nx's tools/direct_calls.py and tools/pch_no_volatile.py). Run after EVERY codegen: tools/codegen.ps1
and the build's codegen step (app/CMakeLists.txt) both do. Every pass is idempotent and refuses to run on a generated
tree whose shape changed.

1. Direct calls. Every recompiled sub_X is a weak alias of __imp__sub_X so that a REX_HOOK can replace it at link time,
   and the codegen always calls sub_X: the compiler cannot inline a weak function. `sub_X(ctx, base);` becomes
   `__imp__sub_X(ctx, base);` when sub_X has no hook. Any 82xxxxxx address in the app or SDK sources, the tomls or the
   localized-copy list counts as hooked (slightly more than needed, and safe). peggle1_init.cpp / peggle1_register.cpp
   are not touched, so indirect calls still go through the hooks.
2. Guest loads and stores without `volatile` (REX_LOAD_* / REX_STORE_* in peggle1_pch.h; the MMIO macros keep it).
   masseffect-nx measured +15-20 %. Define REX_GUEST_VOLATILE as volatile to get the old behaviour without a codegen.
3. Loop barriers (REQUIRED with 2). GCC on the Switch hoisted guest polling loads out of loops in futari-nx: 172
   functions became branch-to-self infinite loops (tools/find_self_loops.py), one of which hung its boot. Every loop
   header (a label a later goto of the same function jumps back to) gets REX_LOOP_BARRIER(), a compiler memory barrier:
   no guest load is reused across iterations, including loads of callees inlined into the loop. Straight-line code
   keeps the plain loads.

  python tools/perf_codegen_post.py [--undo] [--check] [--no-direct] [--no-volatile]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
SDK = os.path.join(ROOT, 'sdk')
GEN = os.path.join(APP, 'generated', 'default')
PREFIX = 'peggle1_recomp.'
PCH = 'peggle1_pch.h'
SOURCES = [os.path.join(APP, 'src'), os.path.join(SDK, 'src'), os.path.join(SDK, 'include'),
           os.path.join(APP, 'CMakeLists.txt'), os.path.join(APP, 'generated', 'hot')]
SOURCES += [os.path.join(APP, f) for f in os.listdir(APP) if f.endswith('.toml')]


def hooked():
    names = set()
    for root in SOURCES:
        if not os.path.exists(root):
            continue
        paths = [root] if os.path.isfile(root) else [
            os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs
            if not f.endswith(('.a', '.obj', '.o', '.png', '.jpg', '.bin'))]
        for path in paths:
            try:
                text = open(path, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            for m in re.findall(r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])', text):
                names.add('sub_' + m.upper())
    return names


def recomp_files(folder):
    files = [f for f in sorted(os.listdir(folder)) if f.startswith(PREFIX) and f.endswith('.cpp')]
    if not files:
        sys.exit('perf_codegen_post: no %s*.cpp in %s' % (PREFIX, folder))
    return files


def read(path):
    return open(path, encoding='utf-8', newline='').read()


def write(path, text):
    open(path, 'w', encoding='utf-8', newline='').write(text)


def direct_calls(folder, with_hook, undo, check):
    call = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
    direct = re.compile(r'__imp__sub_([0-9A-F]{8})\(ctx, base\);')
    changed = files = already = rehooked = 0
    for f in recomp_files(folder):
        path = os.path.join(folder, f)
        text = read(path)
        already += len(direct.findall(text))
        if undo:
            new, n = direct.subn(lambda m: 'sub_%s(ctx, base);' % m.group(1), text)
        else:
            # A hook added after an earlier run turns its direct calls back into calls of the hookable name (the
            # codegen itself never writes __imp__sub_ calls).
            text_hooked = direct.sub(lambda m: 'sub_%s(ctx, base);' % m.group(1) if 'sub_' + m.group(1) in with_hook
                                     else m.group(0), text)
            new = call.sub(lambda m: m.group(0) if 'sub_' + m.group(1) in with_hook
                           else '__imp__sub_%s(ctx, base);' % m.group(1), text_hooked)
            n = len(call.findall(text_hooked)) - len(call.findall(new))
            rehooked += len(direct.findall(text)) - len(direct.findall(text_hooked))
        if new != text:
            files += 1
            if not check:
                write(path, new)
        changed += n
    if not undo and changed == 0 and already == 0:
        sys.exit('direct calls: nothing to convert and no direct calls in %s: the generated call shape changed' % folder)
    return changed, files, already, rehooked


MARKER = '// peggle2-nx: guest loads/stores without volatile'


def no_volatile(folder, undo, check):
    path = os.path.join(folder, PCH)
    s = read(path)
    nl = '\r\n' if '\r\n' in s else '\n'
    if undo:
        if MARKER not in s:
            return 'already volatile'
        s = s.replace('(REX_GUEST_VOLATILE u', '(volatile u')
        start = s.index(MARKER)
        end = s.index('#ifndef REX_GUEST_VOLATILE', start)
        end = s.index('#endif', end) + len('#endif') + len(nl)
        s = s[:start] + s[end:]
        if not check:
            write(path, s)
        return 'volatile restored'
    if 'REX_GUEST_VOLATILE' in s:
        return 'already plain'
    out, changed = [], 0
    for line in s.split(nl):
        if line.startswith(('#define REX_LOAD_U', '#define REX_STORE_U', '#define REX_LOAD_D_U', '#define REX_STORE_D_U')):
            n = line.replace('(volatile u', '(REX_GUEST_VOLATILE u')
            changed += n != line
            line = n
        out.append(line)
    if changed == 0 or '#define REX_LOAD_U8(x)' not in s:
        sys.exit('no volatile: no REX_LOAD_*/REX_STORE_* "(volatile u" macros in %s: the generated pch changed' % path)
    block = nl.join([MARKER + ' (tools/perf_codegen_post.py; masseffect-nx t153). Define',
                     '// REX_GUEST_VOLATILE as volatile to restore the generated behaviour. The MMIO macros keep volatile.',
                     '#ifndef REX_GUEST_VOLATILE', '#define REX_GUEST_VOLATILE', '#endif', ''])
    s = nl.join(out).replace('#define REX_LOAD_U8(x)', block + '#define REX_LOAD_U8(x)', 1)
    if not check:
        write(path, s)
    return '%d macros plain' % changed


LABEL = re.compile(r'^(loc_[0-9A-F]{8}):\s*$')
GOTO = re.compile(r'goto (loc_[0-9A-F]{8});')
BARRIER = '\tREX_LOOP_BARRIER();'
BARRIER_MARKER = '// peggle2-nx: loop barrier (tools/perf_codegen_post.py pass 3)'


def loop_barriers(folder, undo, check):
    headers_total = files = 0
    for f in recomp_files(folder):
        path = os.path.join(folder, f)
        text = read(path)
        nl = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(nl)
        if undo:
            out = [l for l in lines if l != BARRIER]
        else:
            headers, seen = set(), set()
            for line in lines:
                if line.startswith('DEFINE_REX_FUNC('):
                    seen = set()
                    continue
                m = LABEL.match(line)
                if m:
                    seen.add(m.group(1))
                    continue
                for target in GOTO.findall(line):
                    if target in seen:
                        headers.add(target)
            out = []
            for i, line in enumerate(lines):
                out.append(line)
                m = LABEL.match(line)
                if m and m.group(1) in headers and (i + 1 >= len(lines) or lines[i + 1] != BARRIER):
                    out.append(BARRIER)
            headers_total += len(headers)
        if out != lines:
            files += 1
            if not check:
                write(path, nl.join(out))
    pch = os.path.join(folder, PCH)
    text = read(pch)
    nl = '\r\n' if '\r\n' in text else '\n'
    if not undo and BARRIER_MARKER not in text:
        if '#define REX_LOAD_U8(x)' not in text:
            sys.exit('loop barriers: no REX_LOAD_U8 in %s: the generated pch changed' % pch)
        macro = nl.join([BARRIER_MARKER, '#ifndef REX_LOOP_BARRIER',
                         '#define REX_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")', '#endif', ''])
        text = text.replace('#define REX_LOAD_U8(x)', macro + '#define REX_LOAD_U8(x)', 1)
        if not check:
            write(pch, text)
    return '%s %d loop headers in %d files' % ('barriers removed in' if undo else 'barriers at', headers_total, files)


def main():
    undo = '--undo' in sys.argv
    check = '--check' in sys.argv
    folder = GEN
    parts = []
    with_hook = hooked()
    if '--no-direct' not in sys.argv:
        n, files, already, rehooked = direct_calls(folder, with_hook, undo, check)
        parts.append('%s %d calls in %d files (%d direct before%s)' % (
            'undone' if undo else 'direct', n, files, already, ', %d back to their hook' % rehooked if rehooked else ''))
    if '--no-volatile' not in sys.argv:
        parts.append(no_volatile(folder, undo, check))
        parts.append(loop_barriers(folder, undo, check))
    print('perf_codegen_post: ' + '; '.join(parts) + '; %d hooked addresses respected%s' % (
        len(with_hook), ' (check only: nothing written)' if check else ''))


if __name__ == '__main__':
    main()
