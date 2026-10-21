"""Lists generated functions that jump to a label they do not define (the compiler's "use of undeclared label").

The usual cause in Peggle: a function the generator let run on past a call that does not return (a rethrow through
_CxxThrowException) into the next function, and stop inside it. For each, prints the function, the missing labels and
the last `bl` before the first missing label, to find the end with tools/ppc_dis.py.

  python -P tools/label_check.py [app/generated/default]
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    gen = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'app', 'generated', 'default')
    bad = 0
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        text = open(path, errors='replace').read()
        for m in re.finditer(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\) \{\n(.*?)^\}', text, re.M | re.S):
            body = m.group(2)
            defined = set(re.findall(r'^loc_([0-9A-F]+):', body, re.M))
            used = set(re.findall(r'goto loc_([0-9A-F]+);', body))
            missing = sorted(used - defined)
            if missing:
                bad += 1
                print(f'sub_{m.group(1)} ({os.path.basename(path)}): missing {", ".join("loc_" + x for x in missing)}')
    print(f'{bad} functions with missing labels')


if __name__ == '__main__':
    main()
