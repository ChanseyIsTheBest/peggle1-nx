"""Converts the PS4 composites whose name matches a regex (and that the 360 archive lacks) into the cache build_mod.py
uses, so the slow texture work is done once.

  python -P -E tools/ps4_dlc/warm_cache.py <x360 main.rsb> <ps4 main.rsb> <regex> [--cache DIR] [--previews DIR]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert
import rsb as rsblib
import rton


def main():
    x360, ps4 = rsblib.Rsb(sys.argv[1]), rsblib.Rsb(sys.argv[2])
    rx = re.compile(sys.argv[3], re.I)
    cache = sys.argv[sys.argv.index('--cache') + 1] if '--cache' in sys.argv else os.path.join('out', 'ps4_dlc', 'cache')
    previews = sys.argv[sys.argv.index('--previews') + 1] if '--previews' in sys.argv else None
    man, _ = rton.decode(ps4.read('__MANIFESTGROUP__', 'PROPERTIES' + chr(92) + 'RESOURCES.RTON'))
    groups = {g['id'].s: g for g in man['groups']}
    done = 0
    for c in ps4.composites:
        if not rx.search(c['name']) or c['name'].upper() in x360.comp_by_name:
            continue
        t = time.time()
        try:
            r = convert.convert_composite(ps4, groups, c['name'], cache, previews)
        except Exception as e:  # report and go on: one odd group must not stop the batch
            print(f'{c["name"]}: FAILED {type(e).__name__}: {e}', flush=True)
            continue
        done += 1
        tex = sum(len(d) for p in r['packets'] for _, d, _, _ in p['textures'])
        print(f'{r["info"]} [{tex / 1048576:.1f} MB of textures, {time.time() - t:.0f} s]', flush=True)
    print(done, 'composites converted or already cached')


if __name__ == '__main__':
    main()
