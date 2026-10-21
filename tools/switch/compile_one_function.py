#!/usr/bin/env python3
"""Compiles ONE recompiled function alone with the console build's own compiler command, to assembly, optionally with
extra options, to see what devkitA64's GCC makes of it (2026-10-13: how the fault of GCC's early register allocation
pass was found and how the fix was checked; docs/peggle1-notes.md, "Console: large atlases").

  python tools/switch/compile_one_function.py sub_8260E9C0 [--tag name] [--out out/nx/one_function]
      [--sub OLD NEW]... [-- extra compiler options ...]

The command (DEFINES, FLAGS, INCLUDES) comes from out/nx/build.ninja, from the generated file that holds the
function. The precompiled header is NOT used: GCC takes a stale .gch without a word when the header changed after the
last build, so a copy of cmake_pch.hxx is included as a plain header (about 15 s per compile). --sub replaces text in
the function's body first (e.g. to try another helper). Options after "--" go last, so they override the build's
(-O2, -mearly-ra=all, -fdump-rtl-all ...). Prints the path of the .s file; tools/switch/check_lookup_constants.py reads
it.
"""
import argparse
import glob
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def unescape(text):
    """Ninja's $-escapes of a value."""
    return text.replace('$ ', ' ').replace('$:', ':').replace('$$', '$')


def main():
    argv = sys.argv[1:]
    extra = []
    if '--' in argv:
        extra = argv[argv.index('--') + 1:]
        argv = argv[:argv.index('--')]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('function')
    ap.add_argument('--tag', default='build')
    ap.add_argument('--build', default=os.path.join(ROOT, 'out', 'nx'))
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated', 'default'))
    ap.add_argument('--out', default=None)
    ap.add_argument('--sub', nargs=2, action='append', default=[], metavar=('OLD', 'NEW'))
    a = ap.parse_args(argv)
    out = a.out or os.path.join(a.build, 'one_function')
    os.makedirs(out, exist_ok=True)

    source = body = head = None
    key = 'DEFINE_REX_FUNC(%s) {' % a.function
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        if key in text:
            start = text.index(key)
            body = text[start:text.index('\n}\n', start) + 3]
            head = text[:text.index('DEFINE_REX_FUNC(')]
            source = path
            break
    if not body:
        sys.exit('ERROR: no %s in %s' % (key, a.gen))
    for old, new in a.sub:
        if old not in body:
            sys.exit('ERROR: --sub: %r is not in the function' % old)
        body = body.replace(old, new)

    # The compile command of that file in build.ninja.
    ninja = open(os.path.join(a.build, 'build.ninja'), encoding='utf-8', errors='replace').read()
    name = os.path.basename(source)
    m = re.search(r'^build \S*%s\.obj: (?:CXX)_COMPILER__\S+ .*\n((?:  .*\n)+)' % re.escape(name), ninja, re.M)
    if not m:
        sys.exit('ERROR: no compile command for %s in build.ninja' % name)
    values = dict(re.findall(r'^  (\w+) = (.*)$', m.group(1), re.M))
    flags = shlex.split(unescape(values.get('DEFINES', '') + ' ' + values.get('FLAGS', '') + ' '
                                 + values.get('INCLUDES', '')))
    # The precompiled header as a plain header, from a copy (see the docstring).
    command = []
    skip = False
    for i, f in enumerate(flags):
        if skip:
            skip = False
            continue
        if f == '-include' and flags[i + 1].endswith('cmake_pch.hxx'):
            copy = os.path.join(out, 'pch_copy', 'cmake_pch.hxx')
            os.makedirs(os.path.dirname(copy), exist_ok=True)
            open(copy, 'w', encoding='utf-8', newline='\n').write(open(flags[i + 1], encoding='utf-8').read())
            command += ['-include', copy.replace('\\', '/')]
            skip = True
        elif f != '-Winvalid-pch':
            command.append(f)
    cxx = re.search(r'^CMAKE_CXX_COMPILER:\w+=(.*)$', open(os.path.join(a.build, 'CMakeCache.txt')).read(), re.M)
    cpp = os.path.join(out, '%s_%s.cpp' % (a.function, a.tag))
    asm = os.path.join(out, '%s_%s.s' % (a.function, a.tag))
    open(cpp, 'w', encoding='utf-8', newline='\n').write(head + '\n' + body)
    run = subprocess.run([cxx.group(1)] + command + extra + ['-S', '-o', asm, cpp], cwd=a.build, capture_output=True,
                         text=True)
    if run.returncode:
        print(run.stderr[-3000:])
        sys.exit(run.returncode)
    text = open(asm).read()
    print('%s\n  options after the build\'s: %s\n  %d lines, %d tbl (%d with two table registers), early register '
          'allocation: %s' % (asm.replace('\\', '/'), ' '.join(extra) or '(none)', len(text.splitlines()),
                              text.count('\ttbl\t'), sum(1 for l in text.splitlines() if '\ttbl\t' in l and ' - ' in l),
                              next((f for f in reversed(command + extra) if f.startswith('-mearly-ra')),
                                   'the compiler\'s default')))


if __name__ == '__main__':
    main()
