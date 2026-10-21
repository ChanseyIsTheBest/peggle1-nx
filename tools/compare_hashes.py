#!/usr/bin/env python3
"""Lockstep comparison of raw recordings by exact frame hashes (sa2-nx's method: every frame's hash looked up among the
other run's frames at any offset; futari-nx's refinement: only frames that reproduce between baseline runs count).

Record the runs with the same input script and the deterministic settings (vblank_per_swap = true,
vblank_per_swap_count = 2, clock_per_swap_hz = 30; peggle1_video_from_frame), at least two baselines and one test run:

  python tools/compare_hashes.py TEST.rgba BASE1.rgba BASE2.rgba [BASE3.rgba ...]

A frame is reproducible when all baselines hold the same image at the same game frame (the .frames labels). Every
reproducible image must appear in the test run (at any game frame: loading and other host-timed events can shift the
later frames by a few). A missing one is a real difference between the test run and the baselines; the tool lists
them. It also prints how many test frames appear anywhere in the baselines, for reference.
"""
import hashlib
import os
import sys


def load(path):
    """A run's frames, from its raw recording (NAME.rgba) or from the hash list written the first time the recording
    was read (NAME.hash, one hash per frame, a few KB): only the .hash, .frames and .txt files of a run have to be kept
    (the disk-space rule the sa2-nx session relayed, 2026-10-13: a raw recording is 350 MB here)."""
    stem = os.path.splitext(path)[0]
    raw, hash_path = stem + '.rgba', stem + '.hash'
    labels_path = stem + '.frames'
    labels = [int(x) for x in open(labels_path).read().split()] if os.path.exists(labels_path) else None
    if os.path.exists(hash_path) and (not os.path.exists(raw) or os.path.getmtime(hash_path) >= os.path.getmtime(raw)):
        hashes = [bytes.fromhex(x) for x in open(hash_path).read().split()]
    else:
        w, h = (int(x) for x in open(stem + '.txt').read().split()[:2])
        size = w * h * 4
        hashes = []
        with open(raw, 'rb') as f:
            while True:
                b = f.read(size)
                if len(b) < size:
                    break
                hashes.append(hashlib.blake2b(b, digest_size=16).digest())
        with open(hash_path, 'w') as f:
            f.write('\n'.join(x.hex() for x in hashes) + '\n')
    if labels is None or len(labels) < len(hashes):
        labels = list(range(len(hashes)))
    return dict(zip(labels, hashes))  # game frame -> hash (a repeated label keeps the last image)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--hash':  # only write NAME.hash next to a recording
        print(f'{len(load(sys.argv[2]))} frames hashed -> {os.path.splitext(sys.argv[2])[0]}.hash')
        return 0
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    test = load(sys.argv[1])
    bases = [load(p) for p in sys.argv[2:]]
    common = set(bases[0])
    for b in bases[1:]:
        common &= set(b)
    reproducible = sorted(l for l in common if len({b[l] for b in bases}) == 1)
    test_hashes = set(test.values())
    missing = [l for l in reproducible if bases[0][l] not in test_hashes]
    at_same = sum(1 for l in reproducible if test.get(l) == bases[0][l])
    base_hashes = set().union(*(set(b.values()) for b in bases))
    found = sum(1 for h in test.values() if h in base_hashes)
    print(f'baselines: {len(bases)}, game frames in all of them: {len(common)}, reproducible (same image in every '
          f'baseline): {len(reproducible)}')
    print(f'test run: {len(test)} frames; reproducible images found in it: {len(reproducible) - len(missing)} of '
          f'{len(reproducible)} ({at_same} at the same game frame); MISSING: {len(missing)}')
    print(f'test frames whose image appears anywhere in the baselines: {found} of {len(test)}')
    if missing:
        print('missing at game frames:', ' '.join(str(l) for l in missing[:60]))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
