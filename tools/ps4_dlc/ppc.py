"""The PowerPC listing of recompiled functions (the comments of app/generated/default), and who calls a function.

  python -P -E tools/ps4_dlc/ppc.py <address or sub_XXXXXXXX> [--max N]      the listing
  python -P -E tools/ps4_dlc/ppc.py --callers <address>                     the functions that call it directly
"""
import glob
import os
import re
import sys

GEN = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'app', 'generated',
                   'default')


def name_of(a):
    a = a.upper().replace('0X', '').replace('SUB_', '')
    return 'sub_' + a


def listing(name):
    for p in glob.glob(os.path.join(GEN, '*_recomp.*.cpp')):
        t = open(p, encoding='utf-8', errors='replace').read()
        m = re.search(r'^DEFINE_REX_FUNC\(' + name + r'\) \{\n(.*?)^\}\n', t, re.S | re.M)
        if m:
            out = []
            for line in m.group(1).split('\n'):
                if line.startswith('\t// '):
                    out.append('    ' + line[4:])
                elif re.match(r'loc_[0-9A-F]+:', line):
                    out.append(line)
            return out, os.path.basename(p)
    return None, None


def main():
    if sys.argv[1] == '--callers':
        name = name_of(sys.argv[2])
        rx = re.compile(r'\b(?:__imp__)?' + name + r'\(ctx, base\)')
        for p in sorted(glob.glob(os.path.join(GEN, '*_recomp.*.cpp'))):
            t = open(p, encoding='utf-8', errors='replace').read()
            for m in rx.finditer(t):
                start = t.rfind('DEFINE_REX_FUNC(', 0, m.start())
                caller = t[start + 16:t.index(')', start)]
                line = t.rfind('\t// ', 0, m.start())
                print(caller, t[line + 4:t.index('\n', line)].strip())
        return
    name = name_of(sys.argv[1])
    limit = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 100000
    lines, where = listing(name)
    if lines is None:
        sys.exit(name + ' not found')
    print(f'{name} ({where}, {sum(1 for l in lines if l.startswith("    "))} instructions)')
    print('\n'.join(lines[:limit]))


if __name__ == '__main__':
    main()
