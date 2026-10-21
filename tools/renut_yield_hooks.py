#!/usr/bin/env python3
"""peggle2-nx (2026-10-19): Peggle 2's own Direct3D hooks hand over when Renut's renderer is chosen.

The native renderer's hooks (src/me/peggle1_me_hooks.cpp) and the frame hooks (src/peggle1_frame_hooks.cpp) sit at
Direct3D functions. With peggle1_renderer = "renut" each must do nothing of the native renderer's: at an address Renut
hooks too (src/renut_engine/gpu/banjo_shared_hooks.h, written by tools/renut_hook_remap.py) it calls Renut's side
(BANJO_RENUT_YIELD_SHARED), elsewhere the game's original (BANJO_RENUT_YIELD). D3DDevice_Swap keeps its frame count
(the input script and the video recorder read it) and calls Renut's Swap instead of the original.
Idempotent (a hook that already yields is left alone).

  python tools/renut_yield_hooks.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHARED_H = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu', 'banjo_shared_hooks.h')
FILES = [os.path.join(ROOT, 'app', 'src', 'me', 'peggle1_me_hooks.cpp')]
SWAP_FILE = os.path.join(ROOT, 'app', 'src', 'peggle1_frame_hooks.cpp')
INCLUDE = ('#if PEGGLE1_RENUT_RENDERER\n#include "renut_engine/gpu/banjo_renut_select.h"\n'
           '#include "renut_engine/gpu/banjo_shared_hooks.h"\n#endif\n')


def shared():
    text = open(SHARED_H, encoding='utf-8').read()
    return set(re.findall(r'renut_native_sub_([0-9A-F]{8})', text))


def add_include(t):
    if 'renut_engine/gpu/banjo_renut_select.h' in t:
        return t
    # after the last #include line
    lines = t.split('\n')
    last = max(i for i, l in enumerate(lines) if l.startswith('#include'))
    lines.insert(last + 1, INCLUDE.rstrip('\n'))
    return '\n'.join(lines)


def main():
    sh = shared()
    for path in FILES:
        raw = open(path, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in raw else '\n'
        t = raw.replace('\r\n', '\n')
        out = []
        n = 0
        lines = t.split('\n')
        for i, line in enumerate(lines):
            out.append(line)
            m = re.match(r'^REX_HOOK_RAW\(sub_([0-9A-F]{8})\) \{', line)
            if not m:
                continue
            addr = m.group(1)
            if i + 1 < len(lines) and 'BANJO_RENUT_YIELD' in lines[i + 1] + (lines[i + 2] if i + 2 < len(lines) else ''):
                continue
            macro = 'BANJO_RENUT_YIELD_SHARED' if addr in sh else 'BANJO_RENUT_YIELD'
            out += ['#if PEGGLE1_RENUT_RENDERER', '  %s(sub_%s)  // Renut\'s renderer chosen: not the native renderer\'s' % (
                macro, addr), '#endif']
            n += 1
        t = add_include('\n'.join(out))
        open(path, 'w', encoding='utf-8', newline='').write(t.replace('\n', nl))
        print('renut_yield_hooks: %d hooks in %s' % (n, os.path.basename(path)))
    raw = open(SWAP_FILE, encoding='utf-8', newline='').read()
    nl = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    old = '  __imp__sub_8261FE50(ctx, base);\n  g_peggle1_game_frames.fetch_add(1, std::memory_order_relaxed);\n'
    if 'renut_native_sub_8261FE50' not in t:
        if t.count(old) != 1:
            sys.exit('renut_yield_hooks: the Swap hook is not as expected')
        if '8261FE50' in sh:
            new = ('#if PEGGLE1_RENUT_RENDERER\n  if (::banjo::renut_gpu::Active()) {\n    renut_native_sub_8261FE50(ctx, base);  '
                   '// Renut\'s Swap (presents)\n    g_peggle1_game_frames.fetch_add(1, std::memory_order_relaxed);\n'
                   '    return;\n  }\n#endif\n' + old)
        else:
            new = old
        t = add_include(t.replace(old, new))
        open(SWAP_FILE, 'w', encoding='utf-8', newline='').write(t.replace('\n', nl))
        print('renut_yield_hooks: Swap hook hands over' if '8261FE50' in sh else 'renut_yield_hooks: Swap not shared')


if __name__ == '__main__':
    sys.exit(main())
