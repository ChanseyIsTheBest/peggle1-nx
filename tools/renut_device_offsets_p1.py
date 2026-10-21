#!/usr/bin/env python3
"""peggle1-nx (2026-10-21): Renut's device offsets, from Peggle 2's values back to Peggle 1's (= N&B's).

peggle2-nx's tools/renut_device_offsets.py rewrote Renut's hard-coded N&B Direct3D device offsets to Peggle 2's XDK
21256 layout. Peggle 1's XDK 7776 device is laid out exactly as N&B's (7645): out/renut_port/device_offsets.md, a
helper agent's harvest from 410 aligned function pairs, 296 of 297 offsets with delta 0. Input:
out/renut_port/device_offset_sites_p1.json, the 291 sites of peggle2-nx's list, each with `code` (the N&B line),
`banjo`, `peggle` (Peggle 2) and `peggle1` values. Each site's line as Peggle 2's tool left it (the N&B code with the
Peggle 2 literals) is found near the recorded line, and the Peggle 2 literals (same spelling) become Peggle 1's.
Plus the six sites peggle2-nx changed by hand (out/renut_port/device_offset_sites_p1_extra.json), done here by their
`code` line and `token`. Idempotent: a line that already holds the Peggle 1 value is skipped.

  python tools/renut_device_offsets_p1.py [--dry-run]
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPU = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu')
SITES = os.path.join(ROOT, 'out', 'renut_port', 'device_offset_sites_p1.json')
EXTRA = os.path.join(ROOT, 'out', 'renut_port', 'device_offset_sites_p1_extra.json')


def spellings(v):
    return ['0x%X' % v, '0x%x' % v, '%d' % v]


def replace_literal(code, old, new):
    """Replace the literal 'old' in the code part of a line (not the comment) with 'new', keeping its spelling."""
    cut = code.find('//')
    head, tail = (code, '') if cut < 0 else (code[:cut], code[cut:])
    for s in spellings(old):
        pat = re.compile(r'(?<![0-9A-Za-z_])%s(?![0-9A-Za-z_])' % re.escape(s))
        if pat.search(head):
            rep = ('0x%X' % new) if s.startswith('0x') else ('%d' % new)
            return pat.sub(rep, head) + tail, True
    return code, False


def values(v):
    if isinstance(v, str):  # '{a, b, c}' (kDeviceOffsetsMinusOne)
        return [int(x) for x in re.findall(r'[0-9]+', v)]
    return v if isinstance(v, list) else [v]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    sites = json.load(open(SITES, encoding='utf-8'))
    files = {}

    def lines_of(rel):
        path = os.path.join(GPU, rel)
        if path not in files:
            raw = open(path, encoding='utf-8', newline='').read()
            files[path] = [raw, raw.replace('\r\n', '\n').split('\n')]
        return files[path][1]

    done = already = same = 0
    problems = []
    # Sites sharing one line are rewritten together.
    groups = {}
    for s in sites:
        groups.setdefault((s.get('p1_tree_site') or s['site'], s['code'].strip()), []).append(s)
    for (site, code), group in groups.items():
        rel, line = site.rsplit(':', 1)
        line = int(line)
        olds_b, olds_p2, news = [], [], []
        for s in group:
            if s.get('peggle1') is None:
                problems.append('%s %s: no Peggle 1 value (left as it is)' % (site, s.get('name') or ''))
                continue
            b, p2 = values(s['banjo']), values(s['peggle'] if s['peggle'] is not None else s['banjo'])
            p1 = values(s['peggle1'])
            olds_b += b
            olds_p2 += p2 if s.get('changed') else b
            news += p1
        if not news:
            continue
        if olds_p2 == news:
            same += 1
            continue
        lines = lines_of(rel)
        # The line now: the N&B code with Peggle 2's literals.
        now = code
        for o, n in zip(olds_b, olds_p2):
            now, _ = replace_literal(now, o, n)
        want = code
        for o, n in zip(olds_b, news):
            want, _ = replace_literal(want, o, n)
        lo, hi = max(0, line - 120), min(len(lines), line + 120)
        cand = [i for i in range(lo, hi) if lines[i].strip() == now]
        if not cand:
            if any(lines[i].strip() == want for i in range(lo, hi)):
                already += 1
            else:
                problems.append('%s: line not found: %s' % (site, now))
            continue
        best = min(cand, key=lambda i: abs(i + 1 - line))
        text = lines[best]
        ok_all = True
        for o, n in zip(olds_p2, news):
            if o == n:
                continue
            text, ok = replace_literal(text, o, n)
            ok_all &= ok
        if not ok_all:
            problems.append('%s: a literal of %s not in: %s' % (site, olds_p2, lines[best].strip()))
            continue
        lines[best] = text
        done += 1
    extra_done = 0
    for e in json.load(open(EXTRA, encoding='utf-8')):
        if e.get('peggle1') is None:
            problems.append('%s: %s has no Peggle 1 value (by hand: %s)' % (e['site'], e['token'], e['evidence'][:90]))
            continue
        rel, line = e['site'].rsplit(':', 1)
        line = int(line)
        lines = lines_of(rel)
        code = e['code'].strip()
        cand = [i for i in range(max(0, line - 120), min(len(lines), line + 120)) if lines[i].strip() == code]
        if not cand:
            problems.append('%s: extra line not found: %s' % (e['site'], code[:100]))
            continue
        best = min(cand, key=lambda i: abs(i + 1 - line))
        tok, new = e['token'], e['peggle1']
        rep = ('0x%X' % new) if tok.lower().startswith('0x') else ('%d' % new)
        pat = re.compile(r'(?<![0-9A-Za-z_])%s(?![0-9A-Za-z_])' % re.escape(tok))
        if not pat.search(lines[best]):
            problems.append('%s: token %s not in the line' % (e['site'], tok))
            continue
        lines[best] = pat.sub(rep, lines[best])
        extra_done += 1
    for path, (raw, lines) in files.items():
        nl = '\r\n' if '\r\n' in raw else '\n'
        out = '\n'.join(lines).replace('\n', nl)
        if out != raw and not a.dry_run:
            open(path, 'w', encoding='utf-8', newline='').write(out)
    print('renut_device_offsets_p1: %d lines rewritten, %d already, %d lines whose value Peggle 1 shares with Peggle 2; '
          '%d of the hand-changed sites rewritten' % (done, already, same, extra_done))
    for p in problems:
        print('  ' + p)


if __name__ == '__main__':
    sys.exit(main())
