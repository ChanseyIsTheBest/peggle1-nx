#!/usr/bin/env python3
"""Post-codegen step: the game's frame step asks the port how many logic updates the frame gets (app/src/
peggle1_frame_hooks.cpp, key peggle1_fps; docs/peggle1-notes.md, "60 frames a second").

The framework's per-frame step (sub_8242F8A8) runs, in the mode Peggle 2 uses, two logic updates and then draws:

        __imp__sub_8242F2C8(ctx, base);      // the first update (1/60 s of game time)
        ...                                  // and its fractional update of the widgets
    loc_8242FA04:
        ctx.r3.u64 = r31.u64;
        __imp__sub_8242F2C8(ctx, base);      // the second update
        ...
    loc_8242FA34:                            // draw, present

so the game's speed is tied to 30 presented frames a second. After the label loc_8242FA04:

    if (peggle1_fps_updates() < 2) goto loc_8242FA34;

With peggle1_fps = 30 (the default) the port answers 2 and nothing changes.

  python tools/fps_updates.py [--gen app/generated/default] [--check]
Idempotent. In tools/post_codegen.py right after leaf_locals.py (it reads the text the steps up to there leave)
and before banjo-nx's analyses: they take a function with its `// peggle2-nx:` line for opaque
(tools/leaf_dead_stores.py, OPAQUE_MARK), so this step must write that line in every function it changes.
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FUNCTION = 'DEFINE_REX_FUNC(sub_8242F8A8)'
LABEL = 'loc_8242FA04:\n'
AFTER = '\tctx.r3.u64 = r31.u64;\n'
CALL = '__imp__sub_8242F2C8(ctx, base);'
TARGET = 'loc_8242FA34:\n'
MARKER = '\t// peggle2-nx: one logic update for a frame of one vblank (tools/fps_updates.py)\n'
NEW = (LABEL + MARKER + '\t{ int peggle1_fps_updates(void); if (peggle1_fps_updates() < 2) goto loc_8242FA34; }\n')


def main():
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else os.path.join(ROOT, 'app', 'generated', 'default')
    check = '--check' in sys.argv
    for path in sorted(glob.glob(os.path.join(gen, 'peggle1_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        start = text.find(FUNCTION)
        if start < 0:
            continue
        end = text.find('\n}\n', start)
        body = text[start:end]
        if MARKER in body:
            print('fps_updates: already in %s' % os.path.basename(path))
            return
        at = body.find(LABEL)
        # the label once, the second update's call right after it (comment lines apart), and the draw's label later
        tail = body[at + len(LABEL):at + len(LABEL) + 400]
        code = [l for l in tail.split('\n') if not l.strip().startswith('//')]
        if body.count(LABEL) != 1 or body.count(CALL) != 3 or body.count(TARGET) != 1 or \
                len(code) < 2 or code[0] + '\n' != AFTER or code[1].strip() != CALL or body.find(TARGET) < at:
            sys.exit('ERROR (post-codegen patch): fps_updates: the frame step sub_8242F8A8 is not as expected in %s'
                     % path)
        body = body.replace(LABEL, NEW, 1)
        print('fps_updates: the frame step asks peggle1_fps_updates in %s%s'
              % (os.path.basename(path), ' (check only)' if check else ''))
        if not check:
            open(path, 'w', encoding='utf-8', newline='\n').write(text[:start] + body + text[end:])
        return
    sys.exit('ERROR (post-codegen patch): fps_updates: sub_8242F8A8 not found under %s' % gen)


if __name__ == '__main__':
    main()
