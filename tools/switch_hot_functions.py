#!/usr/bin/env python3
"""Hottest functions of a Switch run, from the SDK profiler's samples (logs/rex/rex_profile.log).

Turn sampling on by creating an empty file logs/rex/stacks_profile.flag next to the NRO before starting the game: the
profiler then pauses each busy thread 1000 times a second and writes, every 10 s, every pc it saw with its count
("pc image+0x... xN"). This adds them up per function (self time), with the link map of the SAME build
(out/nx/peggle1.map, written by the Switch link) saying which function and which object each address belongs to:

  python tools/switch_hot_functions.py rex_profile.log [--map out/nx/peggle1.map] [--top 40] [--thread mainThr]
      [--from-report 3]

From futari-nx's tool. Guest functions show as sub_XXXXXXXX from "default" (default.xex's recompiled code: Peggle 2
has no other code module). Everything else is host code (the renderer, the app, the SDK, libnx, Mesa).
"""
import argparse
import bisect
import collections
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_map(path):
    """[(start, end, name, object)] for every .text.* input section of the link map, sorted by start."""
    sections = []
    pending = None
    text_re = re.compile(r'^ \.text\.(\S+)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+))?\s*$')
    cont_re = re.compile(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)\s*$')
    in_memory_map = False
    with open(path, 'r', errors='replace') as f:
        for line in f:
            # Before this header the map lists the input sections --gc-sections DISCARDED, all at address 0.
            if not in_memory_map:
                in_memory_map = line.startswith('Linker script and memory map')
                continue
            if pending is not None:
                m = cont_re.match(line)
                if m:
                    start, size = int(m.group(1), 16), int(m.group(2), 16)
                    if size:
                        sections.append((start, start + size, pending, m.group(3)))
                pending = None
                continue
            m = text_re.match(line)
            if not m:
                continue
            if m.group(2):
                start, size = int(m.group(2), 16), int(m.group(3), 16)
                if size:
                    sections.append((start, start + size, m.group(1), m.group(4)))
            else:
                pending = m.group(1)
    sections.sort()
    return sections


def module_of(obj):
    """Where a function comes from: default.xex's recompiled code, the renderer, the app, the SDK or a library."""
    o = obj.replace('\\', '/')
    if 'peggle1_recomp.dir' in o:
        return 'default'
    if 'peggle1.dir/src/me/' in o:
        return 'renderer'
    if 'peggle1.dir/' in o:
        return 'app'
    m = re.search(r'([^/]+\.a)\(', o)
    if m:
        return m.group(1)
    return os.path.basename(o)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('profile')
    ap.add_argument('--map', default=os.path.join(ROOT, 'out', 'nx', 'peggle1.map'))
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--thread', default='', help='only threads whose name contains this')
    ap.add_argument('--from-report', type=int, default=0, help='skip the first N 10-s reports (boot, menus)')
    a = ap.parse_args()

    sections = load_map(a.map)
    starts = [s[0] for s in sections]
    if not sections:
        sys.exit('no .text sections in ' + a.map)

    pc_re = re.compile(r'^\s+[0-9.]+%\s+pc image\+0x([0-9a-f]+) x(\d+)')
    thread_re = re.compile(r'^-- thread \d+ "([^"]*)": CPU ([0-9.]+)%')
    per_thread = collections.defaultdict(collections.Counter)
    totals = collections.Counter()
    report = -1
    thread = None
    unknown = collections.Counter()
    with open(a.profile, 'r', errors='replace') as f:
        for line in f:
            if line.startswith('===='):
                report += 1
                thread = None
                continue
            m = thread_re.match(line)
            if m:
                thread = m.group(1)
                continue
            m = pc_re.match(line)
            if not m or thread is None or report < a.from_report:
                continue
            if a.thread and a.thread not in thread:
                continue
            pc, n = int(m.group(1), 16), int(m.group(2))
            i = bisect.bisect_right(starts, pc) - 1
            if i >= 0 and pc < sections[i][1]:
                key = (sections[i][2], module_of(sections[i][3]))
            else:
                key = ('?', '?')
                unknown[pc & ~0xFFF] += n
            per_thread[thread][key] += n
            totals[thread] += n
    if not totals:
        sys.exit('no "pc image+0x... xN" samples: was logs/rex/stacks_profile.flag present, and is this a build with '
                 'counts in the profile?')
    grand = sum(totals.values())
    combined = collections.Counter()
    for t, c in per_thread.items():
        combined.update(c)
    print(f'{grand} busy samples (not waiting in the kernel) in {len(totals)} threads')
    for t, n in totals.most_common():
        print(f'  {n * 100.0 / grand:5.1f}%  {t}')
    print(f'\n== all threads: top {a.top} functions by self time ==')
    for (name, mod), n in combined.most_common(a.top):
        guest = name.replace('__imp__', '')
        print(f'  {n * 100.0 / grand:5.1f}%  {n:7d}  {mod:16s} {guest}')
    for t, n in totals.most_common():
        if n < grand * 0.03:
            continue
        print(f'\n== thread "{t}" ({n} samples): top {a.top} ==')
        for (name, mod), k in per_thread[t].most_common(a.top):
            print(f'  {k * 100.0 / n:5.1f}%  {k:7d}  {mod:16s} {name.replace("__imp__", "")}')


if __name__ == '__main__':
    main()
