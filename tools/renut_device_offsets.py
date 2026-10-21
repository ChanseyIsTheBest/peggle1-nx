#!/usr/bin/env python3
"""peggle2-nx (2026-10-19): Renut's hard-coded N&B Direct3D device offsets, rewritten to Peggle 2's (XDK 21256).

Renut (reNut by masterspike52, as banjo-nx carries it) was written against Banjo-Kazooie: Nuts & Bolts' Direct3D, whose
private device layout differs from Peggle 2's (public part 0..0x2A70 unchanged; private fields move by +128..+412, see
out/renut_port/device_offsets.md). Input: out/renut_port/device_offset_sites.json (every site: file:line, the line's
code, the N&B value, the Peggle value). Each changed site's line is found by its code (near the recorded line), and the
N&B literal in it (same spelling: hex or decimal) becomes Peggle's. Sites without a Peggle value (code only the
Direct3D functions Peggle does not link reach: tiling, command-buffer recording) are left alone and listed.
Idempotent: a line that already holds the Peggle value is skipped.

  python tools/renut_device_offsets.py [--dry-run]
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPU = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu')
SITES = os.path.join(ROOT, 'out', 'renut_port', 'device_offset_sites.json')


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    sites = json.load(open(SITES, encoding='utf-8'))
    files = {}
    done = skipped = already = 0
    missing = []
    # sites sharing a line (two literals in one statement) are rewritten together
    groups = {}
    for s in sites:
        if s['changed'] and s['peggle'] is not None:
            groups.setdefault((s['site'], s['code'].strip()), []).append(s)
    merged = []
    for s in sites:
        if not s['changed'] or s['peggle'] is None:
            merged.append(s)
            continue
        g = groups.pop((s['site'], s['code'].strip()), None)
        if g is None:
            continue
        olds, news = [], []
        for x in g:
            o, n = x['banjo'], x['peggle']
            if isinstance(o, str):  # '{a, b, c}' (kDeviceOffsetsMinusOne)
                o, n = [[int(v) for v in re.findall(r'[0-9]+', y)] for y in (o, n)]
            olds += o if isinstance(o, list) else [o]
            news += n if isinstance(n, list) else [n]
        merged.append(dict(g[0], banjo=olds, peggle=news))
    for s in merged:
        if not s['changed']:
            continue
        rel, line = s['site'].rsplit(':', 1)
        line = int(line)
        if s['peggle'] is None:
            missing.append('%s %s (N&B %s): no Peggle counterpart' % (s['site'], s['name'] or '', s['banjo']))
            skipped += 1
            continue
        path = os.path.join(GPU, rel)
        if path not in files:
            raw = open(path, encoding='utf-8', newline='').read()
            files[path] = [raw, raw.replace('\r\n', '\n').split('\n')]
        lines = files[path][1]
        code = s['code'].strip()
        olds = s['banjo'] if isinstance(s['banjo'], list) else [s['banjo']]
        news = s['peggle'] if isinstance(s['peggle'], list) else [s['peggle']]
        # the line: its code (as recorded, or already rewritten), nearest to the recorded line
        best = None
        for i in range(max(0, line - 80), min(len(lines), line + 80)):
            t = lines[i].strip()
            if t == code:
                if best is None or abs(i + 1 - line) < abs(best + 1 - line):
                    best = i
        if best is None:
            # already rewritten?
            want = code
            for o, n in zip(olds, news):
                want, _ = replace_literal(want, o, n)
            if any(lines[i].strip() == want for i in range(max(0, line - 80), min(len(lines), line + 80))):
                already += 1
                continue
            missing.append('%s: code not found: %s' % (s['site'], code))
            continue
        text = lines[best]
        ok_all = True
        for o, n in zip(olds, news):
            text, ok = replace_literal(text, o, n)
            ok_all &= ok
        if not ok_all:
            missing.append('%s: literal %s not in: %s' % (s['site'], olds, lines[best].strip()))
            continue
        lines[best] = text
        done += 1
    for path, (raw, lines) in files.items():
        nl = '\r\n' if '\r\n' in raw else '\n'
        out = '\n'.join(lines).replace('\n', nl)
        if out != raw and not a.dry_run:
            open(path, 'w', encoding='utf-8', newline='').write(out)
    print('renut_device_offsets: %d sites rewritten, %d already, %d without a Peggle value' % (done, already, skipped))
    for m in missing:
        print('  ' + m)


if __name__ == '__main__':
    sys.exit(main())
