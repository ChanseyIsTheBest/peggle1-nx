"""Lists every sound medium of an RSB archive: the files embedded in its sound banks and its streamed .WEM files.

  python -P -E tools/ps4_dlc/audio_index.py <main.rsb> <out.tsv>

Columns: source id, where ("bank" or "stream"), group, file, offset of the medium in the group's data, bytes, format
tag, channels, rate.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsb as rsblib
import wwise


def wem_format(data, at, E):
    """(format tag, channels, rate) of the RIFF / RIFX file at `at`."""
    head = data[at:at + 64]
    p = head.find(b'fmt ')
    if p < 0:
        return 0, 0, 0
    return struct.unpack_from(E + 'HHI', head, p + 8)


def scan(r):
    """[(source id, 'bank' | 'stream', group, file, offset in the group's part 0, bytes, tag, channels, rate)]."""
    rows = []
    for g in r.rsgs:
        sel = [(nm, e) for nm, e in r.packet_files(g) if e['type'] == 0 and nm.upper().endswith(('.BNK', '.WEM'))]
        if not sel:
            continue
        part = r.part(g, 0)
        for nm, e in sel:
            base = os.path.basename(nm.replace('\\', '/')).upper()
            data = part[e['offset']:e['offset'] + e['size']]
            if base.endswith('.WEM'):
                E = '<' if data[:4] == b'RIFF' else '>'
                rows.append((int(base[:-4]), 'stream', g['name'], nm, e['offset'], len(data)) + wem_format(data, 0, E))
                continue
            _E, media = wwise.media(data)
            for sid, (off, size) in media.items():
                F = '<' if data[off:off + 4] == b'RIFF' else '>'
                rows.append((sid, 'bank', g['name'], nm, e['offset'] + off, size) + wem_format(data, off, F))
    return rows


def load(r, path):
    """scan(r), kept in the file `path` between runs."""
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, 'w', encoding='utf-8') as f:
            for row in scan(r):
                f.write('\t'.join(str(x) for x in row) + '\n')
    rows = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            c = line.rstrip('\n').split('\t')
            rows.append((int(c[0]), c[1], c[2], c[3], int(c[4]), int(c[5]), int(c[6]), int(c[7]), int(c[8])))
    return rows


if __name__ == '__main__':
    if os.path.exists(sys.argv[2]):
        os.remove(sys.argv[2])
    print(len(load(rsblib.Rsb(sys.argv[1]), sys.argv[2])), 'media listed in', sys.argv[2])
