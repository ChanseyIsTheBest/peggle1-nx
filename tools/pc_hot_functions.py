#!/usr/bin/env python3
"""Hottest functions of a PC profile (logs/pc_profile.log, written with peggle1_pc_profile_s = N; see
app/src/peggle1_pc_profiler.cpp): adds the per-thread self-time samples of the chosen report windows up per function.

  python tools/pc_hot_functions.py out/pc/logs/pc_profile.log [--from-report 3] [--to-report 9] [--top 40]
                                   [--thread GuestThread] [--guest]

--guest keeps only recompiled guest functions (sub_XXXXXXXX), --thread only threads whose name contains the text.
"""
import argparse
import collections
import re

WINDOW = re.compile(r'^== pc profile window ([\d.]+) s')
THREAD = re.compile(r'^thread (\d+) "(.*)": busy ([\d.]+)% \((\d+) samples\)')
LINE = re.compile(r'^\s+[\d.]+%\s+(\S+) x(\d+)$')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--from-report', type=int, default=1, help='first window to count (1-based)')
    ap.add_argument('--to-report', type=int, default=10 ** 9, help='last window to count')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--thread', default='', help='only threads whose name contains this text')
    ap.add_argument('--guest', action='store_true', help='only recompiled guest functions')
    a = ap.parse_args()

    window = 0
    thread = None  # (name) while inside a thread block; None in the "all threads" block
    totals = collections.Counter()
    per_thread = collections.defaultdict(collections.Counter)
    thread_samples = collections.Counter()
    seconds = 0.0
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = WINDOW.match(line)
        if m:
            window += 1
            thread = None
            if a.from_report <= window <= a.to_report:
                seconds += float(m.group(1))
            continue
        if not (a.from_report <= window <= a.to_report):
            continue
        m = THREAD.match(line)
        if m:
            name = f'{m.group(2) or "?"} ({m.group(1)})'
            thread = name if a.thread.lower() in name.lower() else ''
            if thread:
                thread_samples[thread] += int(m.group(4))
            continue
        if line.startswith('== all threads'):
            thread = None
            continue
        m = LINE.match(line)
        if m and thread:
            label, n = m.group(1), int(m.group(2))
            if a.guest and '!sub_' not in label:
                continue
            totals[label] += n
            per_thread[thread][label] += n

    all_samples = sum(thread_samples.values())
    print(f'windows {a.from_report}..{min(window, a.to_report)}: {seconds:.0f} s, {all_samples} busy samples '
          f'(top 30 of each thread only; shares are of all busy samples)')
    for label, n in totals.most_common(a.top):
        print(f'  {n * 100.0 / max(all_samples, 1):5.1f}%  {label} x{n}')
    for name, n in thread_samples.most_common():
        print(f'== {name}: {n} samples')
        for label, k in per_thread[name].most_common(min(a.top, 15)):
            print(f'  {k * 100.0 / max(n, 1):5.1f}%  {label} x{k}')


if __name__ == '__main__':
    main()
