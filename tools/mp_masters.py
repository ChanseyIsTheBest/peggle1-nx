#!/usr/bin/env python3
"""Post-codegen step: the multiplayer lobby asks the port about masters beyond the fifth (app/src/dlc/
peggle1_dlc_lobby.cpp, key peggle1_dlc_mp_masters; docs/peggle1-notes.md, "Windy and Jimmy in the multiplayer
lobby"). Two places of the recompiled lobby get a call; with the key off both calls change nothing.

1. sub_82206AC0 (MultiplayerLobbyLayout) makes one button per master in a loop that ends with

       // cmpwi cr6,r11,5
       cr6.compare<int32_t>(r11.s32, 5, xer);
       // blt cr6,0x82206e6c
       if (cr6.lt) goto loc_82206E6C;

   In front of the compare: `if (peggle1_mp_more_masters(base, r11.u32)) goto loc_82206E6C;`.

2. sub_82207658 places the buttons: it fills a map from button number to the layout's placeholder (numbers 0 to 4
   and -1, the layout's entries at +20 ... +84 and +100) and then looks every button up in it. After the last of its
   own entries: the same for numbers 5 and 6 with the placeholders peggle1_mp_placeholder returns (0 = none).

  python tools/mp_masters.py [--gen app/generated/default] [--check]
Idempotent. In tools/post_codegen.py right after leaf_locals.py (it reads the text the steps up to there leave)
and before banjo-nx's analyses: they take a function with its `// peggle2-nx:` line for opaque
(tools/leaf_dead_stores.py, OPAQUE_MARK), so this step must write that line in every function it changes.
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LOOP_OLD = '\tcr6.compare<int32_t>(r11.s32, 5, xer);\n'
LOOP_AFTER = '\tif (cr6.lt) goto loc_82206E6C;\n'
LOOP_MARKER = '\t// peggle2-nx: more masters than five in the lobby (tools/mp_masters.py)\n'
LOOP_NEW = (LOOP_MARKER + '\t{ bool peggle1_mp_more_masters(uint8_t*, uint32_t); '
            'if (peggle1_mp_more_masters(base, r11.u32)) goto loc_82206E6C; }\n' + LOOP_OLD)

PLACE_BEFORE = '\tctx.r9.s64 = r29.s64 + 100;\n'
PLACE_OLD = '\tREX_STORE_D_U32(ctx.r3.u32, 0, ctx.r9.u32);\n'
PLACE_MARKER = '\t// peggle2-nx: placeholders for a sixth and a seventh master button (tools/mp_masters.py)\n'
PLACE_NEW = (PLACE_OLD + PLACE_MARKER +
             '\t{ uint32_t peggle1_mp_placeholder(PPCContext*, uint8_t*, uint32_t, uint32_t);\n'
             '\t  for (uint32_t peggle1_k = 5; peggle1_k < 7; ++peggle1_k) {\n'
             '\t    const uint32_t peggle1_p = peggle1_mp_placeholder(&ctx, base, r29.u32, peggle1_k);\n'
             '\t    if (!peggle1_p) break;\n'
             '\t    REX_STORE_D_U32(ctx.r1.u32, 80, peggle1_k);\n'
             '\t    ctx.r4.s64 = ctx.r1.s64 + 80;\n'
             '\t    ctx.r3.s64 = ctx.r1.s64 + 112;\n'
             '\t    __imp__sub_82209548(ctx, base);\n'
             '\t    REX_STORE_D_U32(ctx.r3.u32, 0, peggle1_p);\n'
             '\t  } }\n')


def patch_loop(body, path):
    at = body.find(LOOP_OLD)
    if body.count(LOOP_OLD) != 1 or LOOP_AFTER not in body[at:at + 200]:
        sys.exit('ERROR (post-codegen patch): mp_masters: the button loop of sub_82206AC0 is not as expected in %s'
                 % path)
    return body.replace(LOOP_OLD, LOOP_NEW, 1)


def patch_place(body, path):
    at = body.find(PLACE_BEFORE)
    store = body.find(PLACE_OLD, at)
    # the store must be the one that follows `r9 = r29 + 100` (one comment line between them) and be preceded by
    # the map call of the placeholder numbered -1
    if body.count(PLACE_BEFORE) != 1 or store < 0 or store - at > 120 or \
            '__imp__sub_82209548(ctx, base);' not in body[at - 400:at]:
        sys.exit('ERROR (post-codegen patch): mp_masters: the placeholder map of sub_82207658 is not as expected in '
                 '%s' % path)
    return body[:store] + PLACE_NEW + body[store + len(PLACE_OLD):]


PATCHES = (('DEFINE_REX_FUNC(sub_82206AC0)', LOOP_MARKER, patch_loop, 'the button loop asks peggle1_mp_more_masters'),
           ('DEFINE_REX_FUNC(sub_82207658)', PLACE_MARKER, patch_place, 'the placeholder map asks peggle1_mp_placeholder'))


def main():
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else os.path.join(ROOT, 'app', 'generated', 'default')
    check = '--check' in sys.argv
    todo = {p[0]: p for p in PATCHES}
    for path in sorted(glob.glob(os.path.join(gen, 'peggle1_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        changed = False
        for function in list(todo):
            start = text.find(function)
            if start < 0:
                continue
            _, marker, patch, what = todo.pop(function)
            end = text.find('\n}\n', start)
            body = text[start:end]
            if marker in body:
                print('mp_masters: already in %s (%s)' % (os.path.basename(path), function[16:-1]))
                continue
            text = text[:start] + patch(body, path) + text[end:]
            changed = True
            print('mp_masters: %s in %s%s' % (what, os.path.basename(path), ' (check only)' if check else ''))
        if changed and not check:
            open(path, 'w', encoding='utf-8', newline='\n').write(text)
    if todo:
        sys.exit('ERROR (post-codegen patch): mp_masters: not found under %s: %s' % (gen, ', '.join(todo)))


if __name__ == '__main__':
    main()
