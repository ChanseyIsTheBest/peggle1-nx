"""Lists the composites of an RSB whose name matches a regex, with their packets and files.

  python -P -E tools/ps4_dlc/list_groups.py <main.rsb> <regex> [--files N]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsb as rsblib


def main():
    r = rsblib.Rsb(sys.argv[1])
    rx = re.compile(sys.argv[2], re.I)
    show = int(sys.argv[sys.argv.index('--files') + 1]) if '--files' in sys.argv else 3
    n = 0
    for c in r.composites:
        if not rx.search(c['name']):
            continue
        n += 1
        parts = []
        for idx, res, lang in c['subs']:
            g = r.rsgs[idx]
            files = r.packet_files(g)
            tex = [(e['w'], e['h']) for _, e in files if e['type'] == 1]
            plain = [(p.split(chr(92))[-1], e['size']) for p, e in files if e['type'] == 0]
            d = f'    {g["name"]} res {res} lang {lang!r}:'
            if tex:
                d += ' atlases ' + ' '.join(f'{w}x{h}' for w, h in tex)
            if plain:
                d += f' {len(plain)} files {sum(sz for _, sz in plain)} bytes: ' + \
                    ', '.join(f'{nm} ({sz})' for nm, sz in plain[:show]) + (' ...' if len(plain) > show else '')
            parts.append(d)
        print(c['name'])
        print('\n'.join(parts))
    print(n, 'composites')


if __name__ == '__main__':
    main()
