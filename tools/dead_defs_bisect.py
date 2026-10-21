#!/usr/bin/env python3
"""Bisect tools/dead_ctx_defs.py's rewrites over the generated files (banjo-nx, 2026-10-15): which file's (and which
function's) left-out writes break a run. Keeps both versions of every file under out/dead_bisect and writes into
app/generated only the files whose text changes, so the build recompiles only those. Build with
PEGGLE1_DEAD_DEFS_HOLD=1 so post_codegen.py leaves the files as set (peggle2-nx's copy: the tree is
app/generated/default).

  python tools/dead_defs_bisect.py save             the tree as it is now (rewrites on) -> out/dead_bisect/on, and its
                                                    restored form -> out/dead_bisect/off
  python tools/dead_defs_bisect.py list             the files that have rewrites, one per line
  python tools/dead_defs_bisect.py set FILE...      those files with the rewrites, every other file without
  python tools/dead_defs_bisect.py functions FILE   the functions of FILE that have rewrites, in order
  python tools/dead_defs_bisect.py setfn FILE NAME...  FILE with the rewrites of those functions only (other files
                                                    untouched)
"""
import glob
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dead_ctx_defs as dcd  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'app', 'generated', 'default')
CACHE = os.path.join(ROOT, 'out', 'dead_bisect')
NL = chr(10)


def read_lines(path):
    return open(path, encoding='utf-8').read().split(NL)


def write_if_changed(target, text):
    if open(target, encoding='utf-8').read() != text:
        open(target, 'w', encoding='utf-8', newline=NL).write(text)
        return True
    return False


def main():
    cmd = sys.argv[1]
    if cmd == 'save':
        for sub in ('on', 'off'):
            os.makedirs(os.path.join(CACHE, sub), exist_ok=True)
        for path in glob.glob(os.path.join(GEN, '*_recomp.*.cpp')):
            name = os.path.basename(path)
            shutil.copyfile(path, os.path.join(CACHE, 'on', name))
            shutil.copyfile(path, os.path.join(CACHE, 'off', name))
        dcd.restore(os.path.join(CACHE, 'off'))
        print('saved')
    elif cmd == 'list':
        for path in sorted(glob.glob(os.path.join(CACHE, 'on', '*_recomp.*.cpp'))):
            text = open(path, encoding='utf-8').read()
            if 'REX_DEAD_DEF(' in text or 'REX_DEAD_REC(' in text:
                sys.stdout.write(os.path.basename(path) + NL)
    elif cmd == 'set':
        wanted = set(a.strip() for a in sys.argv[2:] if a.strip())
        written = 0
        for path in glob.glob(os.path.join(CACHE, 'on', '*_recomp.*.cpp')):
            name = os.path.basename(path)
            source = os.path.join(CACHE, 'on' if name in wanted else 'off', name)
            written += write_if_changed(os.path.join(GEN, name), open(source, encoding='utf-8').read())
        print('set: %d files with rewrites, %d files written' % (len(wanted), written))
    elif cmd == 'functions':
        current, names = None, []
        for line in read_lines(os.path.join(CACHE, 'on', sys.argv[2].strip())):
            m = dcd.NAMED_HEAD.match(line)
            if m:
                current = m.group(1)
            elif current and ('REX_DEAD_DEF(' in line or 'REX_DEAD_REC(' in line) and (not names or names[-1] != current):
                names.append(current)
        sys.stdout.write(NL.join(names) + NL)
    elif cmd == 'setfn':
        name = sys.argv[2].strip()
        keep = set(a.strip() for a in sys.argv[3:] if a.strip())
        on = read_lines(os.path.join(CACHE, 'on', name))
        off = read_lines(os.path.join(CACHE, 'off', name))
        if len(on) != len(off):
            sys.exit('setfn: on and off versions of %s are not line-aligned' % name)
        out, current = [], None
        for a, b in zip(on, off):
            m = dcd.NAMED_HEAD.match(a)
            if m:
                current = m.group(1)
            out.append(a if current in keep else b)
        changed = write_if_changed(os.path.join(GEN, name), NL.join(out))
        print('setfn: %s with %d functions rewritten (%s)' % (name, len(keep), 'written' if changed else 'unchanged'))


if __name__ == '__main__':
    main()
