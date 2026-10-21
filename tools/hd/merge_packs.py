#!/usr/bin/env python3
"""Keeps the HD pack in step with a changed archive without encoding all of it again.

The pack is keyed by the 720 atlases' content (XXH3 of their BC3 data, as build_hd_pack.py keys them). This lists
the archive's 720 atlases with their keys and takes each one's HD entry from the packs given (a later pack wins).
Atlases no pack has are reported with their groups: build those alone into a partial pack first
(build_hd_pack.py --groups A,B --out <partial>) and give it last.

  python -P -E tools/hd/merge_packs.py --x360 <main.rsb> [--out <folder>] <pack folder> [<pack folder> ...]
Without --out: only the report. With it: <folder>/peggle1_hd.pack and peggle1_hd.txt (the lines of the packs the
entries come from), verified as the game reads it.
"""
import argparse
import os
import struct
import sys

import xxhash

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_hd_pack as b  # noqa: E402


def archive_keys(x):
    """{key: (group, atlas)} of every 720 atlas the builder would key."""
    out = {}
    m = b.manifest(x)
    for g in m['groups']:
        res = g.get('res')
        name = g['id'].s
        if res is None or res.s != '720' or name.upper() not in x.by_name:
            continue
        if not x.by_name[name.upper()]['ptx_count']:
            continue
        for atlas, (raw, w, h) in b.textures_of(x, name).items():
            size = (w // 4) * (h // 4) * 16
            if w % 8 or h % 8 or len(raw) < size:
                continue
            out[xxhash.xxh3_64_intdigest(bytes(raw[:size]))] = (name, atlas)
    return out


def read_pack(folder):
    """{key: (header fields, chunk sizes, the bytes after the index entry, its txt line)}."""
    path = os.path.join(folder, 'peggle1_hd.pack')
    lines = {}
    txt = os.path.join(folder, 'peggle1_hd.txt')
    if os.path.exists(txt):
        for line in open(txt, encoding='utf-8'):
            if line.strip():
                lines[int(line.split()[0], 16)] = line.rstrip('\n')
    out = {}
    with open(path, 'rb') as f:
        magic, version, count, chunk = struct.unpack('<4I', f.read(16))
        assert magic == b.MAGIC and version == b.VERSION and chunk == b.CHUNK, (path, magic, version, chunk)
        index = [struct.unpack('<QQQII4H', f.read(40)) for _ in range(count)]
        for key, offset, digest, raw, chunks, gw, gh, hw, hh in index:
            f.seek(offset)
            sizes = struct.unpack('<%dI' % chunks, f.read(4 * chunks))
            data = f.read(sum(sizes))
            out[key] = ((key, digest, raw, gw, gh, hw, hh), sizes, data, lines.get(key, '%016X' % key))
    return out


def write(folder, entries):
    entries = sorted(entries, key=lambda e: e[0][0])
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, 'peggle1_hd.pack')
    offset = 16 + 40 * len(entries)
    index = bytearray(struct.pack('<4I', b.MAGIC, b.VERSION, len(entries), b.CHUNK))
    for (key, digest, raw, gw, gh, hw, hh), sizes, data, _ in entries:
        index += struct.pack('<QQQII4H', key, offset, digest, raw, len(sizes), gw, gh, hw, hh)
        offset += 4 * len(sizes) + len(data)
    with open(path, 'wb') as f:
        f.write(index)
        for _, sizes, data, _ in entries:
            f.write(struct.pack('<%dI' % len(sizes), *sizes))
            f.write(data)
    with open(os.path.join(folder, 'peggle1_hd.txt'), 'w', encoding='utf-8') as f:
        for e in entries:
            f.write(e[3] + '\n')
    return path, offset


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--x360', required=True)
    ap.add_argument('--out')
    ap.add_argument('packs', nargs='+')
    a = ap.parse_args()
    x = b.rsblib.Rsb(a.x360)
    keys = archive_keys(x)
    have = {}
    for folder in a.packs:
        p = read_pack(folder)
        for k, e in p.items():
            if k in keys:
                have[k] = (e, folder)
        print('%s: %d entries, %d of them atlases of this archive' % (folder, len(p), sum(1 for k in p if k in keys)))
    missing = sorted({keys[k][0] for k in keys if k not in have})
    print('archive: %d atlases; %d have an HD entry; groups with atlases that have none: %s' % (
        len(keys), len(have), ','.join(missing) if missing else 'none'))
    if a.out:
        path, size = write(a.out, [e for e, _ in have.values()])
        print('%s: %d textures, %d bytes, verified %d images' % (path, len(have), size, b.verify(path)))


if __name__ == '__main__':
    main()
