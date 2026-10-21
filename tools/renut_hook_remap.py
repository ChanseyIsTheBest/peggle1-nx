#!/usr/bin/env python3
"""Renut's renderer hooks (taken from banjo-nx through peggle2-nx, N&B's addresses noted) at Peggle 1's addresses.

peggle2-nx wrote this tool (2026-10-19) to move banjo-nx's hooks to Peggle 2. In peggle1-nx (2026-10-21) the same tree
holds Peggle 2's addresses, and every hook keeps N&B's address in its `// N&B sub_X` note. This rewrites each hook from
that note to the address of the same XDK function in Peggle 1 (out/renut_port/hook_map.json: banjo_addr ->
peggle_addr, a helper agent's matching of N&B's and Peggle 1's executables, out/renut_port/hook_map.md), in
src/renut_engine/gpu:
  - a hook whose function Peggle 1 does not link (peggle_addr null) is left out (#if 0 ... #endif around its macro
    and body; RAW hooks have a body that follows the macro);
  - a hook peggle2-nx left out ("#if 0  // peggle2-nx: N&B sub_X (...) is not linked in Peggle 2") whose function
    Peggle 1 links is put back (the #if 0 line and its #endif go);
  - a hook at an address Peggle 1's own code hooks too (the frame hooks: SHARED below) becomes the _SHARED form
    (renut_native_sub_X), which Peggle 1's hook calls first when Renut's renderer is chosen; every other hook is a
    plain one;
  - every other `sub_<address>` in code is left for the port to fix by hand.
Writes src/renut_engine/gpu/banjo_shared_hooks.h again with Peggle 1's shared addresses. Idempotent.

  python tools/renut_hook_remap.py [--map out/renut_port/hook_map.json] [--check]
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPU = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu')
# Peggle 1's own hooks at Direct3D functions (src/peggle1_frame_hooks.cpp: D3DDevice_Swap): where Renut hooks the same
# address, Peggle 1's hook hands over to Renut's (_SHARED).
PEGGLE_HOOKED = {
    '824430C8',
}
DISABLED = re.compile(r'^#if 0  // peggle2-nx: N&B sub_([0-9A-F]{8}) \(.*\) is not linked in Peggle 2')
HOOK = re.compile(r'BANJO_RENUT_HOOK(_RAW)?(_SHARED)?\(sub_([0-9A-F]{8})')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--map', default=os.path.join(ROOT, 'out', 'renut_port', 'hook_map.json'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    entries = json.load(open(a.map, encoding='utf-8'))
    if isinstance(entries, dict):
        entries = entries.get('hooks', list(entries.values()))
    mapping = {}
    for e in entries:
        b = e['banjo_addr'].upper().replace('0X', '').replace('SUB_', '')
        p = e.get('peggle_addr')
        p = p.upper().replace('0X', '').replace('SUB_', '') if p else None
        mapping[b] = (p, e.get('confidence', ''), e.get('name', ''))
    shared = set()
    missing, absent, done, restored = set(), [], 0, []
    for d, _, fs in os.walk(GPU):
        for f in fs:
            if not f.endswith(('.cpp', '.h')) or f == 'banjo_shared_hooks.h' or f == 'banjo_renut_select.h':
                continue
            path = os.path.join(d, f)
            text = open(path, encoding='utf-8', newline='').read()
            nl = '\r\n' if '\r\n' in text else '\n'
            lines = text.split(nl)
            out = []
            i = 0
            changed = False
            while i < len(lines):
                line = lines[i]
                dm = DISABLED.match(line)
                if dm and mapping.get(dm.group(1), (None,))[0]:
                    # Put the hook back: drop this line and the #endif that closes it.
                    depth = 1
                    j = i + 1
                    while j < len(lines):
                        s = lines[j].lstrip()
                        if s.startswith('#if'):
                            depth += 1
                        elif s.startswith('#endif'):
                            depth -= 1
                            if depth == 0:
                                break
                        j += 1
                    del lines[j]
                    restored.append((dm.group(1), f))
                    changed = True
                    i += 1
                    continue
                m = HOOK.search(line)
                if not m or line.lstrip().startswith('//') or '#define' in line:
                    out.append(line)
                    i += 1
                    continue
                raw, banjo = bool(m.group(1)), m.group(3)
                note = re.search(r'// N&B sub_([0-9A-F]{8})', line)
                if note:  # already rewritten: the N&B address is in the note
                    banjo = note.group(1)
                if banjo not in mapping:
                    missing.add(banjo)
                    out.append(line)
                    i += 1
                    continue
                peggle, conf, name = mapping[banjo]
                if not peggle:
                    # Left out: the macro line, and for RAW hooks the body up to its closing brace at column 0.
                    absent.append((banjo, name, f))
                    block = [line]
                    if raw and '{' in line:
                        depth = line.count('{') - line.count('}')
                        while depth > 0 and i + 1 < len(lines):
                            i += 1
                            block.append(lines[i])
                            depth += lines[i].count('{') - lines[i].count('}')
                    out.append('#if 0  // peggle1-nx: N&B sub_%s (%s) is not linked in Peggle 1' % (banjo, name))
                    out.extend(block)
                    out.append('#endif')
                    i += 1
                    changed = True
                    continue
                form = 'BANJO_RENUT_HOOK' + ('_RAW' if raw else '') + ('_SHARED' if peggle in PEGGLE_HOOKED else '')
                if peggle in PEGGLE_HOOKED:
                    shared.add(peggle)
                new = HOOK.sub(lambda _: '%s(sub_%s' % (form, peggle), line, count=1)
                if not note:
                    new += '  // N&B sub_%s' % banjo
                if new != line:
                    changed = True
                    done += 1
                out.append(new)
                i += 1
            if changed and not a.check:
                open(path, 'w', encoding='utf-8', newline='').write(nl.join(out))
    hdr = os.path.join(GPU, 'banjo_shared_hooks.h')
    body = ['// peggle1-nx (tools/renut_hook_remap.py): the addresses both Renut\'s renderer and Peggle 1\'s own code hook.',
            '// Peggle 1\'s hook calls renut_native_sub_X first when Renut\'s renderer is chosen (BANJO_RENUT_YIELD_SHARED).',
            '#pragma once', '', '#include <cstdint>', '', '#include <rex/ppc.h>', '']
    body += ['extern "C" void renut_native_sub_%s(PPCContext& ctx, uint8_t* base);' % s for s in sorted(shared)]
    if not a.check:
        open(hdr, 'w', encoding='utf-8', newline='\n').write('\n'.join(body) + '\n')
    print('renut_hook_remap: %d hooks at Peggle 1 addresses, %d put back, %d left out (not linked in Peggle 1), %d '
          'shared with Peggle 1\'s own hooks (%s); N&B addresses with no entry in the map: %s' % (
              done, len(restored), len(absent), len(shared), ', '.join(sorted(shared)),
              ', '.join(sorted(missing)) or 'none'))
    for b, f in restored:
        print('  put back: sub_%s (%s)' % (b, f))
    for b, n, f in absent:
        print('  left out: sub_%s %s (%s)' % (b, n, f))


if __name__ == '__main__':
    sys.exit(main())
