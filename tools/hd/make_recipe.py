"""Writes the recipe of the HD atlas pack: what the web installer (peggle2-nx-installer, lib/hd/hdpack.js) needs to
make every image of peggle1_hd.pack again from the user's two archives, without a texel of the game in it.

build_hd_pack.py decides, for every sprite of every 720 atlas, which PS4 picture replaces it (its twin, another
sprite of the size, a sprite found by picture, cut to the canvas by content, with the 720 sprite kept under it): those
decisions compare pictures and take most of its time. This runs them (build_group in its --stats mode, which then
also lists every placed sprite) and writes, per HD texture, the placed sprites in order:

  { "version": 1, "chunk": 262144,
    "sources": [[PS4 group, PS4 atlas], ...],
    "textures": [ { "key": "<XXH3-64 of the 720 atlas, 16 hex digits>", "group": 720 group, "atlas": 720 atlas,
                    "gw": , "gh": , "hw": , "hh": , ["hash": "<XXH3-64 of the HD image's BC3, from --pack>",]
                    "patches": [[ax, ay, aw, ah, source, bx, by, bw, bh(, rect, under)], ...] }, ... ] }

  ax..ah    the 720 sprite's rectangle in its atlas (the HD patch goes to 1.5 times it, place())
  source    index into "sources"; bx..bh the PS4 sprite's rectangle in that atlas
  rect      [x0, y0, x1, y1] (floats, of the PS4 sprite's texels) when the picture is that rectangle of the sprite
            (matching.recut: Image.transform EXTENT, BICUBIC), null when it is the sprite as it is
  under     1 when the 720 sprite is kept under the picture (matching.extras / with_extras), else absent or 0
The rest (the 720 atlas enlarged, edge fill, BC3, LZ4) follows from the pictures. Textures are sorted by key; a
texture found in two groups is kept once, as build_hd_pack.py does (--names picks the group a pack's listing has).

  python -P -E tools/hd/make_recipe.py --x360 <main.rsb> --ps4 <ps4 main.rsb> --out <folder>
      [--groups NAME,...] [--jobs N] [--names <peggle1_hd.txt>] [--pack <peggle1_hd.pack>]
Writes <folder>/peggle1_hd_recipe.json.gz and <folder>/sprites.txt (the builder's report lines, to compare with a
build's). --pack: the recipe must cover exactly that pack's keys (checked), and each texture gets its image hash.
"""
import argparse
import gzip
import json
import multiprocessing
import os
import struct
import sys
import time

import xxhash

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_hd_pack as b  # noqa: E402

VERSION = 1


def pack_index(path):
    """{key: (hash, raw, gw, gh, hw, hh)} of a peggle1_hd.pack."""
    with open(path, 'rb') as f:
        magic, version, count, chunk = struct.unpack('<4I', f.read(16))
        assert magic == b.MAGIC and version == b.VERSION and chunk == b.CHUNK, path
        out = {}
        for _ in range(count):
            key, _, digest, raw, _, gw, gh, hw, hh = struct.unpack('<QQQII4H', f.read(40))
            out[key] = (digest, raw, gw, gh, hw, hh)
    return out


def archive_keys(x):
    """{key: (group, atlas)} of every 720 atlas build_hd_pack.py keys (as merge_packs.py lists them)."""
    out = {}
    for g in b.manifest(x)['groups']:
        res = g.get('res')
        name = g['id'].s
        if res is None or res.s != '720' or name.upper() not in x.by_name or not x.by_name[name.upper()]['ptx_count']:
            continue
        for atlas, (raw, w, h) in b.textures_of(x, name).items():
            size = (w // 4) * (h // 4) * 16
            if w % 8 or h % 8 or len(raw) < size:
                continue
            out.setdefault(xxhash.xxh3_64_intdigest(bytes(raw[:size])), (name, atlas))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--x360', required=True)
    ap.add_argument('--ps4', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--groups', default='')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--names', default='', help="a pack's peggle1_hd.txt: which group a texture of two groups is from")
    ap.add_argument('--pack', default='', help='a peggle1_hd.pack the recipe must cover; gives each texture its hash')
    ap.add_argument('--min-psnr', type=float, default=20.0)
    ap.add_argument('--sure-psnr', type=float, default=30.0)
    a = ap.parse_args()
    started = time.time()
    x, p = b.rsblib.Rsb(a.x360), b.rsblib.Rsb(a.ps4)
    only = {g.strip().upper() for g in a.groups.split(',') if g.strip()}
    jobs = b.make_jobs(x, p, only)
    print(f'{len(jobs)} groups ({time.time() - started:.0f} s for the manifests)', flush=True)
    names = {}
    if a.names:
        for line in open(a.names, encoding='utf-8'):
            if line.strip():
                names[int(line.split()[0], 16)] = line.split()[1]
    opt = dict(stats=True, recipe=True, previews='', min_psnr=a.min_psnr, sure_psnr=a.sure_psnr, match_previews='')
    os.makedirs(a.out, exist_ok=True)
    found = {}  # key -> [entries of every group that has it]
    log = open(os.path.join(a.out, 'sprites.txt'), 'w', encoding='utf-8')
    with multiprocessing.Pool(a.jobs, b.init, (a.x360, a.ps4)) as pool:
        for done, (name, out, stats, rows, took) in enumerate(pool.imap(b.build_group, [(j, opt) for j in jobs])):
            for r in rows:
                log.write(r + '\n')
            for e in out:
                found.setdefault(e['key'], []).append(e)
            print(f'[{done + 1}/{len(jobs)}] {name}: {len(out)} textures, '
                  f'{sum(len(e["patches"]) for e in out)} sprites placed ({took:.0f} s)', flush=True)
    log.close()

    entries, twice = [], 0
    for key, cands in found.items():
        e = next((c for c in cands if c['name'] == names.get(key)), cands[0])
        if len(cands) > 1:
            twice += 1
            same = all(c['patches'] == cands[0]['patches'] for c in cands)
            print(f'{key:016X} in {len(cands)} groups ({", ".join(c["name"] for c in cands)}): kept {e["name"]}'
                  f'{"" if same else " (their sprites differ)"}')
        entries.append(e)
    entries.sort(key=lambda e: e['key'])
    sources, index = [], {}
    textures = []
    for e in entries:
        group, atlas = e['name'].split('/')
        t = dict(key='%016X' % e['key'], group=group, atlas=atlas, gw=e['gw'], gh=e['gh'], hw=e['hw'], hh=e['hh'])
        patches = []
        for ax, ay, aw, ah, pg, pa, bx, by, bw, bh, rect, under in e['patches']:
            s = index.setdefault((pg, pa), len(sources))
            if s == len(sources):
                sources.append([pg, pa])
            row = [ax, ay, aw, ah, s, bx, by, bw, bh]
            if rect is not None or under:
                row += [list(rect) if rect is not None else None, 1 if under else 0]
            patches.append(row)
        t['patches'] = patches
        textures.append(t)

    problems = 0
    if a.pack:
        have = pack_index(a.pack)
        keys = archive_keys(x)
        mine = {e['key'] for e in entries}
        print(f'{a.pack}: {len(have)} textures; the archive has {len(keys)} keyed 720 atlases; the recipe {len(mine)}')
        for what, ks in (('in the pack, not in the recipe', set(have) - mine),
                         ('in the recipe, not in the pack', mine - set(have)),
                         ('in the archive, not in the recipe', set(keys) - mine),
                         ('in the recipe, not in the archive', mine - set(keys))):
            if ks and not only:
                problems += len(ks)
                print(f'  {len(ks)} {what}: ' + ' '.join('%016X' % k for k in sorted(ks)[:20]))
        for t, e in zip(textures, entries):
            h = have.get(e['key'])
            if h is None:
                continue
            if h[2:] != (e['gw'], e['gh'], e['hw'], e['hh']):
                problems += 1
                print(f'  {t["key"]}: sizes {h[2:]} in the pack, {(e["gw"], e["gh"], e["hw"], e["hh"])} here')
            t['hash'] = '%016X' % h[0]
    recipe = dict(version=VERSION, chunk=b.CHUNK, sources=sources, textures=textures)
    path = os.path.join(a.out, 'peggle1_hd_recipe.json.gz')
    data = json.dumps(recipe, separators=(',', ':')).encode()
    with gzip.open(path, 'wb', compresslevel=9) as f:
        f.write(data)
    print(f'{path}: {len(textures)} textures ({twice} found in two groups), '
          f'{sum(len(t["patches"]) for t in textures)} sprites from {len(sources)} PS4 atlases, '
          f'{sum(1 for t in textures for q in t["patches"] if len(q) > 9 and q[9] is not None)} recut, '
          f'{sum(1 for t in textures for q in t["patches"] if len(q) > 10 and q[10])} with the 720 sprite under; '
          f'{len(data)} bytes of JSON, {os.path.getsize(path)} gzipped ({time.time() - started:.0f} s)')
    if problems:
        print(f'{problems} problem(s)')
        sys.exit(1)


if __name__ == '__main__':
    main()
