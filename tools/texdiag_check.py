#!/usr/bin/env python3
"""Compares a run's data checks (peggle1_diag_textures, io_diag_crc: configs/peggle1_test_texdiag.toml) with the
archive the run used, stage by stage:

  [io] crc        every NtReadFile of 4 KB or more of main.rsb against the same bytes of the archive
  [texdiag] stream   every stream the game inflated against the packet part of the same length (and where the first
                  megabyte that differs is)
  [texdiag] texture  every texture of 1 MB or more the renderer picked up against the atlas of the same size in the
                  archive, slice by slice; with --dumps, byte by byte, and a preview of what was in guest memory

  python tools/texdiag_check.py --log <peggle1_NNN.log> --rsb <main.rsb> [--dumps <texdiag folder>] [--out <folder>]

Nothing here is game data kept in the repository: the archive is read, previews go to --out (default out/texdiag).
"""
import argparse
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ps4_dlc'))
import rsb  # noqa: E402

MB = 1 << 20


def runs(indices):
    """[1,2,3,7,8] -> '1-3, 7-8'"""
    out, start, prev = [], None, None
    for i in indices:
        if start is None:
            start = prev = i
        elif i == prev + 1:
            prev = i
        else:
            out.append((start, prev))
            start = prev = i
    if start is not None:
        out.append((start, prev))
    return ', '.join('%d' % a if a == b else '%d-%d' % (a, b) for a, b in out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--log', required=True)
    ap.add_argument('--rsb', required=True)
    ap.add_argument('--dumps')
    ap.add_argument('--out', default=os.path.join('out', 'texdiag'))
    args = ap.parse_args()

    a = rsb.Rsb(args.rsb)
    raw = open(args.rsb, 'rb')
    text = open(args.log, encoding='utf-8', errors='replace').read()

    # ---- file reads --------------------------------------------------------------------------------------------
    reads = re.findall(r"\[io\] crc \d+: (\d+) bytes of '([^']*)' at (\d+) into ([0-9A-F]{8}): crc32 ([0-9A-F]{8})",
                       text)
    ok = bad = other = 0
    groups = sorted(a.rsgs, key=lambda g: g['offset'])

    def packet_at(off):
        for g in groups:
            if g['offset'] <= off < g['offset'] + g['length']:
                return g['name']
        return 'archive header'

    bad_lines = []
    for n, path, off, guest, crc in reads:
        if not path.lower().endswith('main.rsb'):
            other += 1
            continue
        n, off = int(n), int(off)
        raw.seek(off)
        want = zlib.crc32(raw.read(n)) & 0xFFFFFFFF
        if want == int(crc, 16):
            ok += 1
        else:
            bad += 1
            bad_lines.append('    %d bytes at %d (%s) into %s: read %s, the archive has %08X' % (
                n, off, packet_at(off), guest, crc, want))
    print('file reads of main.rsb: %d checked, %d equal to the archive, %d DIFFERENT (%d reads of other files not '
          'checked)' % (ok + bad, ok, bad, other))
    for line in bad_lines[:40]:
        print(line)

    # ---- inflate streams ---------------------------------------------------------------------------------------
    streams = re.findall(r'\[texdiag\] stream (\d+): (\d+) bytes inflated in (\d+) flushes, crc32 ([0-9A-F]{8}), '
                         r'adler32 ([0-9A-F]{8}); crc32 at each MB: ([0-9A-F -]*)', text)
    by_size = {}
    for g in a.rsgs:
        if g['part1_size']:
            by_size.setdefault(g['part1_size'], []).append(g)
    cache = {}

    def part1(g):
        if g['name'] not in cache:
            if len(cache) > 6:
                cache.pop(next(iter(cache)))
            cache[g['name']] = a.part(g, 1)
        return cache[g['name']]

    s_ok = s_bad = s_unknown = 0
    print('\ninflate streams: %d in the log' % len(streams))
    for n, size, flushes, crc, adler, marks in streams:
        size, crc = int(size), int(crc, 16)
        cands = by_size.get(size, [])
        if not cands:
            s_unknown += 1
            continue
        hit = None
        for g in cands:
            if zlib.crc32(part1(g)) & 0xFFFFFFFF == crc:
                hit = g
                break
        if hit:
            s_ok += 1
            print('  stream %s: %9d bytes = %s, equal' % (n, size, hit['name']))
            continue
        s_bad += 1
        marks = [int(m, 16) for m in marks.split()] if marks.strip() != '-' else []
        best = None
        for g in cands:
            data = part1(g)
            c, first = 0, None
            for i, m in enumerate(marks):
                c = zlib.crc32(data[i * MB:(i + 1) * MB], c)
                if (c & 0xFFFFFFFF) != m:
                    first = i
                    break
            good = len(marks) if first is None else first
            if best is None or good > best[1]:
                best = (g, good, first)
        g, good, first = best
        where = ('the first %d MB are equal, MB %d is the first that differs' % (good, first)) if first is not None \
            else 'every whole MB is equal: the difference is in the last partial MB'
        print('  stream %s: %9d bytes, DIFFERENT from %s (%d candidates of that size): %s; adler32 in the run %s, '
              'of the archive %08X' % (n, size, g['name'], len(cands), where, adler,
                                       zlib.adler32(part1(g)) & 0xFFFFFFFF))
    print('  %d equal to a packet of the archive, %d DIFFERENT, %d of a size no packet has (other data)' % (
        s_ok, s_bad, s_unknown))

    # ---- textures ----------------------------------------------------------------------------------------------
    textures = re.findall(r'\[texdiag\] texture (\d+): ([0-9A-F]{8}) (\d+)x(\d+) format (\d+), (\d+) bytes, '
                          r'(CHANGED|first upload) \((\d+) zero bytes\), pool block ([0-9A-F]{8}), crc32 '
                          r'([0-9A-F]{8}), file ([^;]*); slices: ([0-9A-F ]*)', text)
    atlases = {}
    for g in a.rsgs:
        if not g['ptx_count']:
            continue
        for path, e in a.packet_files(g):
            if e['type'] == 1:
                atlases.setdefault((e['w'], e['h'], e['size']), []).append((g, path, e))
    print('\ntextures of 1 MB or more: %d in the log' % len(textures))
    os.makedirs(args.out, exist_ok=True)
    t_ok = t_bad = t_unknown = 0
    for n, addr, w, h, fmt_, size, state, zeros, pool, crc, file_, slices in textures:
        w, h, size, crc = int(w), int(h), int(size), int(crc, 16)
        slices = [int(x, 16) for x in slices.split()]
        cands = atlases.get((w, h, size), [])
        if not cands:
            t_unknown += 1
            print('  texture %s at %s %dx%d (%s): no atlas of that size in the archive (a video frame or a render '
                  'target)' % (n, addr, w, h, state))
            continue
        best = None
        for g, path, e in cands:
            data = part1(g)[e['offset']:e['offset'] + e['size']]
            if zlib.crc32(data) & 0xFFFFFFFF == crc:
                best = (g, path, e, len(slices), [])
                break
            wrong = [k for k in range(len(slices))
                     if zlib.crc32(data[size * k // len(slices):size * (k + 1) // len(slices)]) & 0xFFFFFFFF
                     != slices[k]]
            if best is None or len(wrong) < len(best[4]):
                best = (g, path, e, len(slices) - len(wrong), wrong)
        g, path, e, good, wrong = best
        name = '%s %s' % (g['name'], os.path.basename(path.replace('\\', '/')))
        if not wrong:
            t_ok += 1
            print('  texture %s at %s %dx%d (%s, pool %s) = %s, equal' % (n, addr, w, h, state, pool, name))
            continue
        t_bad += 1
        rows = h // len(slices)
        print('  texture %s at %s %dx%d (%s, pool %s, %d zero bytes): DIFFERENT from %s: %d of %d slices equal; wrong '
              'slices %s (a slice is %d pixel rows)' % (n, addr, w, h, state, pool, int(zeros), name, good,
                                                       len(slices), runs(wrong), rows))
        dump = os.path.join(args.dumps, file_.strip()) if args.dumps and file_.strip() != 'not written' else None
        if dump and os.path.isfile(dump):
            compare_dump(dump, part1(g)[e['offset']:e['offset'] + e['size']], w, h, args.out)
    print('  %d equal to an atlas of the archive, %d DIFFERENT, %d without an atlas of that size' % (
        t_ok, t_bad, t_unknown))


def compare_dump(dump, want, w, h, out):
    """What the wrong bytes are: where they start, how many, and what they look like."""
    import numpy as np
    got = np.fromfile(dump, dtype=np.uint8)
    ref = np.frombuffer(want, dtype=np.uint8)
    n = min(len(got), len(ref))
    diff = got[:n] != ref[:n]
    count = int(diff.sum())
    print('      %s: %d of %d bytes differ' % (os.path.basename(dump), count, n))
    if not count:
        return
    idx = np.flatnonzero(diff)
    pitch = (w // 4) * 16
    print('      first at byte %d (block row %d, block %d), last at byte %d' % (
        idx[0], idx[0] // pitch, (idx[0] % pitch) // 16, idx[-1]))
    wrong = got[:n][diff]
    print('      of the wrong bytes: %.1f %% are zero where the archive is not, %.1f %% are non-zero where the '
          'archive is zero' % (100.0 * float((wrong == 0).sum()) / count,
                               100.0 * float((ref[:n][diff] == 0).sum()) / count))
    # Pages (4 KB) and megabytes with differences: a pattern at those sizes points at memory, not at the data.
    pages = np.unique(idx // 4096)
    print('      %d of %d pages (4 KB) have differences; the first: %s' % (
        len(pages), (n + 4095) // 4096, ', '.join(str(int(p)) for p in pages[:16])))
    per_mb = np.bincount(idx // MB, minlength=(n + MB - 1) // MB)
    print('      wrong bytes per MB: %s' % ' '.join(str(int(c)) for c in per_mb))
    # Is a wrong stretch the right data from somewhere else? Try the first run of at least 64 wrong bytes.
    start = int(idx[0])
    piece = got[start:start + 64].tobytes()
    found = want.find(piece)
    if found >= 0:
        print('      the 64 bytes at the first difference are the archive\'s bytes at %d (%+d)' % (
            found, found - start))
    try:
        from PIL import Image
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ps4_dlc'))
        import ptx
        for label, data in (('run', got.tobytes()), ('archive', want)):
            rgba = ptx.decode_bc3(ptx.x360_blocks(data, w, h))
            im = Image.fromarray(rgba, 'RGBA')
            bg = Image.new('RGBA', im.size, (40, 40, 40, 255))
            bg.alpha_composite(im)
            scale = max(w, h) / 1024.0
            target = os.path.join(out, '%s_%s.jpg' % (os.path.splitext(os.path.basename(dump))[0], label))
            bg.convert('RGB').resize((int(w / scale), int(h / scale))).save(target, quality=88)
        print('      previews: %s_run.jpg and _archive.jpg in %s' % (
            os.path.splitext(os.path.basename(dump))[0], out))
    except Exception as error:  # the comparison above stands without the pictures
        print('      no preview (%s)' % error)


if __name__ == '__main__':
    main()
