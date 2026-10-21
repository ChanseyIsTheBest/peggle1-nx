#!/usr/bin/env python3
"""Compares two raw recordings of the recorder (peggle1_video_s: .rgba + .txt) frame by frame, for example the native
renderer against the emulated GPU, or a build with an optimization against the one without.

Runs with the same input script (timed in game frames) see the same inputs at the same game frame, but the recorder
can skip a flip now and then, so each frame of A is matched to the most similar frame of B within +-window frames
(mean absolute difference of 160x90 grayscale thumbnails, near the previous match), then compared at full capture
resolution:

  mad      mean absolute difference over all channels (0-255)
  changed  share of pixels with any channel differing by more than --tolerance (default 8, to ignore
           dithering/rounding)
  max      largest channel difference

It prints a summary, writes a CSV of every frame and, for the worst frames, A | B | diff images (the diff amplified
x4) to --out.

  python tools/compare_raw.py A.rgba B.rgba [--out out/compare] [--window 6] [--step 1] [--worst 12]
  python tools/compare_raw.py A.rgba B.rgba --identical      # exit 1 unless every matched frame is identical
"""
import argparse
import csv
import os
import sys

import numpy as np
from PIL import Image

TW, TH = 160, 90


def open_raw(path):
    w, h, fps, count = open(path[:-5] + '.txt').read().split()
    w, h, count = int(w), int(h), int(count)
    data = np.memmap(path, dtype=np.uint8, mode='r', shape=(count, h, w, 4))
    return data, w, h, count


def game_frames(path):
    """Game frame number of each recorded frame (.frames written by the recorder), or None."""
    p = path[:-5] + '.frames'
    if not os.path.exists(p):
        return None
    return [int(x) for x in open(p).read().split()]


def thumb(frame):
    h, w = frame.shape[:2]
    g = frame[..., :3].astype(np.float32).mean(axis=2)
    return g[: h - h % TH, : w - w % TW].reshape(TH, h // TH, TW, w // TW).mean(axis=(1, 3))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--out', default='out/compare')
    ap.add_argument('--window', type=int, default=6)
    ap.add_argument('--step', type=int, default=1, help='compare every Nth frame of A')
    ap.add_argument('--worst', type=int, default=12)
    ap.add_argument('--tolerance', type=int, default=8)
    ap.add_argument('--identical', action='store_true', help='strict: tolerance 0 and exit 1 on any difference')
    ap.add_argument('--first', type=int, default=0)
    ap.add_argument('--last', type=int, default=-1)
    args = ap.parse_args()
    if args.identical:
        args.tolerance = 0
    A, wa, ha, na = open_raw(args.a)
    B, wb, hb, nb = open_raw(args.b)
    if (wa, ha) != (wb, hb):
        sys.exit(f'sizes differ: {wa}x{ha} vs {wb}x{hb}')
    os.makedirs(args.out, exist_ok=True)
    thumbs_b = {}

    def tb(j):
        if j not in thumbs_b:
            thumbs_b[j] = thumb(B[j])
        return thumbs_b[j]

    last = na - 1 if args.last < 0 else min(args.last, na - 1)
    ga, gb = game_frames(args.a), game_frames(args.b)
    index_b = None
    if ga and gb:
        # Both recordings carry their game frame numbers: start each search at B's frame of the same game frame.
        import bisect
        index_b = lambda g: min(nb - 1, bisect.bisect_left(gb, g))
        print(f'pairing by game frame: A {ga[0]}..{ga[-1]}, B {gb[0]}..{gb[-1]}')
    rows = []
    offset = 0  # running offset of B against A
    for i in range(args.first, last + 1, args.step):
        ta = thumb(A[i])
        best = None
        center = index_b(ga[i]) if index_b else i + offset
        for j in range(max(0, center - args.window), min(nb, center + args.window + 1)):
            score = float(np.abs(ta - tb(j)).mean())
            if best is None or score < best[0]:
                best = (score, j)
        if best is None:
            break
        j = best[1]
        offset = j - i
        if index_b and not (ga[i] - args.window <= gb[j] <= ga[i] + args.window):
            pass  # outside the window in game frames: still reported, the thumbnails chose it
        fa = A[i][..., :3].astype(np.int16)
        fb = B[j][..., :3].astype(np.int16)
        d = np.abs(fa - fb)
        mad = float(d.mean())
        changed = float((d.max(axis=2) > args.tolerance).mean())
        rows.append((i, j, mad, changed, int(d.max()), ga[i] if ga else -1, gb[j] if gb else -1))
        if len(thumbs_b) > 64:
            for k in [k for k in thumbs_b if k < j - args.window]:
                del thumbs_b[k]
    with open(os.path.join(args.out, 'frames.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['a', 'b', 'mad', 'changed', 'max', 'game_a', 'game_b'])
        w.writerows(rows)
    if not rows:
        sys.exit('nothing compared')
    mads = np.array([r[2] for r in rows])
    ch = np.array([r[3] for r in rows])
    identical = sum(1 for r in rows if r[4] == 0)
    print(f'{len(rows)} frames compared ({os.path.basename(args.a)} vs {os.path.basename(args.b)}), tolerance '
          f'{args.tolerance}: identical {identical}, mean mad {mads.mean():.3f}, median changed '
          f'{np.median(ch) * 100:.3f} %, worst mad {mads.max():.2f} at a#{rows[int(mads.argmax())][0]}')
    for limit in (0.001, 0.01, 0.05):
        print(f'  frames with more than {limit * 100:g} % of pixels changed: {int((ch > limit).sum())}')
    worst = sorted(rows, key=lambda r: -r[3])[: args.worst]
    for i, j, mad, changed, mx, g1, g2 in worst:
        fa = np.ascontiguousarray(A[i][..., :3])
        fb = np.ascontiguousarray(B[j][..., :3])
        diff = np.clip(np.abs(fa.astype(np.int16) - fb.astype(np.int16)) * 4, 0, 255).astype(np.uint8)
        img = Image.new('RGB', (wa * 3, ha))
        img.paste(Image.fromarray(fa), (0, 0))
        img.paste(Image.fromarray(fb), (wa, 0))
        img.paste(Image.fromarray(diff), (wa * 2, 0))
        img.save(os.path.join(args.out, f'a{i:05d}_b{j:05d}.png'))
        print(f'  a#{i} b#{j} (game frames {g1}/{g2}): mad {mad:.2f}, changed {changed * 100:.2f} %, max {mx}')
    if args.identical and identical != len(rows):
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
