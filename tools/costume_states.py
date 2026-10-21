#!/usr/bin/env python3
"""Post-codegen step: the Select a Master screen shows the PS4's six costumes of a master (app/src/dlc/
peggle1_dlc_master_select.cpp, key peggle1_dlc_master_select; docs/peggle1-notes.md, "Select a Master with seven
masters").

The screen's side picture of a master is one animation (POPANIM_<group>_MASTER_SELECT_COSTUMES) driven by a state
machine: per costume n (1, 2, ...) the labels n_in, n_idle, n_out and n_locked_in, n_locked_idle, n_locked_out as
states 6n-5 ... 6n, state 0 = "stop". The Xbox 360 executable makes them for four costumes (a loop counted down from
4 in sub_821F8390), and its state machine holds 32 states (std::bitset<32>: every state number is checked against
32). The PS4's makes six times six (its machine has a bitset<64>), which does not fit here: 37 states.

Here: costumes five and six (the PS4's two costume packs; nothing locks them but their master) get their three
unlocked states only, at 25, 26, 27 and 28, 29, 30. Three functions change:

1. sub_821F8390 (the states): the loop count comes from peggle1_costume_count (4, or 6 when the archive has the
   PS4's animations); rounds after the fourth leave the locked states out and advance by 3.
2. sub_821F9618 (the chosen costume's picture on) and
3. sub_821F95A0 (a costume's picture off) take a costume's idle state as 6c + 2 and its locked one as that + 3:
   26 and 32 (costumes five and six) become 26 and 29, and their locked state is costume one's (state 5: the
   silhouette of a master that is locked).

  python tools/costume_states.py [--gen app/generated/default] [--check]
Idempotent. In tools/post_codegen.py right after leaf_locals.py (it reads the text the steps up to there leave)
and before banjo-nx's analyses: they take a function with its `// peggle2-nx:` line for opaque
(tools/leaf_dead_stores.py, OPAQUE_MARK), so this step must write that line in every function it changes.
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARKER = '\t// peggle2-nx: six costumes in the 32 states of the Select a Master picture (tools/costume_states.py)\n'


def once(body, old, new, what, path):
    if body.count(old) != 1:
        sys.exit('ERROR (post-codegen patch): costume_states: %s is not as expected in %s (%d matches)'
                 % (what, path, body.count(old)))
    return body.replace(old, new, 1)


def patch_states(body, path):
    body = once(body, '\tr14.s64 = 4;\n',
                MARKER + '\t{ uint32_t peggle1_costume_count(PPCContext*, uint8_t*); '
                'r14.s64 = peggle1_costume_count(&ctx, base); }\n', 'the loop count of sub_821F8390', path)
    body = once(body, '\tr28.s64 = r26.s64 + 2;\n',
                '\tif (r25.u32 > 4) goto peggle1_costume_unlocked_only;\n\tr28.s64 = r26.s64 + 2;\n',
                'the first locked state of sub_821F8390', path)
    # The end of a round: the six label strings are released, the first one read is the one at r1 + 192.
    at = body.find('\tif (r25.u32 > 4) goto peggle1_costume_unlocked_only;\n')
    release = '\tr11.u64 = REX_LOAD_D_U32(ctx.r1.u32, 212);\n'
    end = body.find(release, at)
    if end < 0 or body.count(release) != 1 or body.count('__imp__sub_8222A818(ctx, base);', at, end) != 2:
        sys.exit('ERROR (post-codegen patch): costume_states: the end of a round of sub_821F8390 is not as expected '
                 'in %s' % path)
    body = body[:end] + 'peggle1_costume_unlocked_only:\n' + body[end:]
    return once(body, '\tr26.s64 = r26.s64 + 6;\n', '\tr26.s64 = r26.s64 + (r25.u32 > 4 ? 3 : 6);\n',
                'the state step of sub_821F8390', path)


def patch_use(name):
    def patch(body, path):
        body = once(body, '\tr30.s64 = r11.s64 + 2;\n',
                    '\tr30.s64 = r11.s64 + 2;\n' + MARKER +
                    '\tif (r30.u32 >= 26) r30.s64 = r30.u32 == 26 ? 26 : 29;\n', 'the idle state of ' + name, path)
        return once(body, '\tctx.r4.s64 = r30.s64 + 3;\n', '\tctx.r4.s64 = r30.u32 >= 26 ? 5 : r30.s64 + 3;\n',
                    'the locked state of ' + name, path)
    return patch


PATCHES = (('DEFINE_REX_FUNC(sub_821F8390)', patch_states, 'states for six costumes'),
           ('DEFINE_REX_FUNC(sub_821F9618)', patch_use('sub_821F9618'), 'the states of costumes five and six'),
           ('DEFINE_REX_FUNC(sub_821F95A0)', patch_use('sub_821F95A0'), 'the states of costumes five and six'))


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
            _, patch, what = todo.pop(function)
            end = text.find('\n}\n', start)
            body = text[start:end]
            if MARKER in body:
                print('costume_states: already in %s (%s)' % (os.path.basename(path), function[16:-1]))
                continue
            text = text[:start] + patch(body, path) + text[end:]
            changed = True
            print('costume_states: %s: %s in %s%s' % (function[16:-1], what, os.path.basename(path),
                                                       ' (check only)' if check else ''))
        if changed and not check:
            open(path, 'w', encoding='utf-8', newline='\n').write(text)
    if todo:
        sys.exit('ERROR (post-codegen patch): costume_states: not found under %s: %s' % (gen, ', '.join(todo)))


if __name__ == '__main__':
    main()
