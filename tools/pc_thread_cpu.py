#!/usr/bin/env python3
"""CPU seconds per thread from a PC profile (logs/pc_profile.log, peggle1_pc_profile_s = 10), added up over a range of
its report windows, so two runs of the same scripted route compare by CPU time rather than wall time (the PC is shared
with other builds; a thread's CPU time is far less affected by that than its wall-clock share).

  python tools/pc_thread_cpu.py out/pc/logs/pc_profile.log [--from-report 6] [--to-report 12] [--vs other.log]
"""
import argparse
import collections
import re


def load(path, first, last):
    window = 0
    total = collections.Counter()
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('== pc profile window'):
            window += 1
            continue
        m = re.match(r'^thread \d+ "(.*)": busy [\d.]+% \(\d+ samples\), cpu ([\d.]+) s', line)
        if m and first <= window <= last:
            name = re.sub(r'XThread[0-9A-F]+ ', 'XThread ', m.group(1))
            total[name] += float(m.group(2))
    return total, min(window, last) - first + 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--from-report', type=int, default=6)
    ap.add_argument('--to-report', type=int, default=12)
    ap.add_argument('--vs', help='a second profile of the same route: printed side by side (this one first)')
    a = ap.parse_args()
    t, n = load(a.log, a.from_report, a.to_report)
    o, m = load(a.vs, a.from_report, a.to_report) if a.vs else (None, 0)
    names = sorted(set(t) | set(o or {}), key=lambda k: -(t.get(k, 0) + (o or {}).get(k, 0)))
    print(f'windows {a.from_report}..{a.to_report} ({n} in {a.log}' + (f', {m} in {a.vs})' if a.vs else ')'))
    for k in names[:14]:
        line = f'{t.get(k, 0):8.2f} s'
        if o is not None:
            b = o.get(k, 0)
            line += f'  vs {b:8.2f} s  ({100 * (t.get(k, 0) - b) / b:+6.1f} %)' if b else f'  vs {b:8.2f} s'
        print(f'{line}  {k}')
    total = sum(t.values())
    line = f'{total:8.2f} s'
    if o is not None:
        tb = sum(o.values())
        line += f'  vs {tb:8.2f} s  ({100 * (total - tb) / tb:+6.1f} %)'
    print(f'{line}  ALL THREADS')


if __name__ == '__main__':
    main()
