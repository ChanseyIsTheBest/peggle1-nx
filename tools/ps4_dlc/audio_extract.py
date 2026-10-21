"""Copies the sound banks and streamed sound files of an RSB archive into a folder (under out/, never the repository).

  python -P -E tools/ps4_dlc/audio_extract.py <main.rsb> <out folder> [name filter, e.g. GLOBAL_DATA]

Writes <out>/<NAME>.BNK and <out>/<id>.WEM as they are in the archive, and <out>/files.tsv (group, path, bytes).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsb as rsblib


def main():
    r = rsblib.Rsb(sys.argv[1])
    out = sys.argv[2]
    want = sys.argv[3].upper() if len(sys.argv) > 3 else ''
    os.makedirs(out, exist_ok=True)
    rows = []
    for g in r.rsgs:
        sel = [(nm, e) for nm, e in r.packet_files(g) if e['type'] == 0 and nm.upper().endswith(('.BNK', '.WEM'))]
        if not sel:
            continue
        data = None
        for nm, e in sel:
            rows.append((g['name'], nm, e['size']))
            base = os.path.basename(nm.replace('\\', '/')).upper()
            if want and want not in base:
                continue
            if data is None:
                data = r.part(g, 0)
            with open(os.path.join(out, base), 'wb') as f:
                f.write(data[e['offset']:e['offset'] + e['size']])
    with open(os.path.join(out, 'files.tsv'), 'w', encoding='utf-8') as f:
        for row in rows:
            f.write('%s\t%s\t%d\n' % row)
    print(len(rows), 'sound files listed in', os.path.join(out, 'files.tsv'))


main()
