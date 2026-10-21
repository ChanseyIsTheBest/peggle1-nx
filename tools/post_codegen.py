#!/usr/bin/env python3
"""The post-codegen steps of the optimised build, in one place (from sa2-nx's tool of the same name, 2026-10-13;
docs/peggle1-notes.md, "sa2-nx's three CPU rounds" and "banjo-nx's codegen round"). It replaces
tools/perf_codegen_post.py (futari-nx's three passes in one file): sa2-nx's later steps are written against the text its
own passes produce.

Run after EVERY codegen, by tools/codegen.ps1 and by the build itself: app/CMakeLists.txt appends this script to the
generated peggle1_codegen command, because the PC build re-runs the generator whenever an input changes (the
manifest, a toml, the xex, the generator binary) and that would otherwise drop the steps silently.

  base (as before):
      tools/pch_no_volatile.py  guest loads and stores without volatile            } required
      tools/loop_barriers.py    a compiler barrier at the loop headers that need it } together
                                (since 2026-10-19 the guest window only, not all memory: marathon-nx via banjo-nx)
      tools/direct_calls.py     calls between unhooked functions become direct (inlinable); --redo, so a function
                                hooked since the last run is called through its hook again
  round 1 (sasrt-nx's, through sa2-nx):
      tools/guest_cas.py        compare-and-swap without the legacy builtin's trailing `dmb ish` on AArch64
      tools/flush_mode_flow.py  the flush-mode checks the control flow and the callees' summaries fix (nothing on the
                                Switch; still tested in the PC build), with the entry modes from the image
      tools/vec_byteswap.py     the 16-byte reversal of guest vector loads and stores without a table lookup
      tools/pch_neon_shuffles.py constant word shuffles as one permutation on AArch64
  round 2:
      tools/indirect_calls.py   every `bctrl` tagged with its site (PC census), and the sites of
                                app/indirect_promotions.txt as a guarded direct call. Before loop_barriers.py (a
                                promoted call is no longer an indirect call) and before direct_calls.py.
      tools/pch_cold_traps.py   the pch's trap helper out of line and cold
      tools/ro_constants.py     loads from the image's read-only data as constants. Before float_loads.py.
      tools/float_loads.py      a single-precision load straight into a float register
      tools/frame_traffic.py    (since 2026-10-19) marks the prologue and epilogue statements only (REX_LR_READ,
                                REX_LR_WRITE, ...); its three elisions stay off (not exact). tools/lr_preserve.py reads
                                the marks.
      tools/leaf_locals.py      the ctx registers of leaf functions with loops as locals
  round 3:
      tools/loop_locals.py      the ctx registers a call-free loop uses as locals for the length of the loop. After
                                loop_barriers.py, before leaf_locals.py.
      Both skip the functions of app/config/locals_optout.txt (tools/locals_choice.py: cheaper without on the A57).
  Peggle's game patches (not optimisations): tools/mp_masters.py, tools/fps_updates.py, tools/costume_states.py. Right
      after leaf_locals.py (they match the text the steps up to there leave) and before banjo-nx's analyses, which
      take every function with a patch's `// peggle2-nx:` line for opaque (tools/leaf_dead_stores.py, OPAQUE_MARK).
  banjo-nx's codegen round (2026-10-19; docs/peggle1-notes.md, "banjo-nx's codegen round"), in its order:
      tools/vperm_one_source.py, tools/fp_sign_ops.py, tools/leaf_dead_stores.py, tools/single_floats.py,
      tools/inline_memcalls.py, tools/vec_unaligned_stores.py, tools/ro_constants_cfg.py, tools/dead_ctx_defs.py
      (--abi-indirect), tools/flush_hoist.py; then the localized native copies (tools/localize_native.py --list
      app/src/cpu/hot/localized.txt; made from the text as it stands after those steps, so the copies get them too);
      then tools/lr_preserve.py --rewrite, tools/r1_preserve.py + tools/r1_local.py (only the functions of
      app/config/r1_local_choice.json: everywhere it is a loss), tools/vsel_neon.py and tools/inline_leaves.py (last).
      All of the round's rewrites are taken back out first on a tree processed before (the late ones by their own
      --restore, the others by tools/round_restore.py), so every step reads the text it was written for.
  not run: sa2-nx's frame_traffic elisions (not exact); banjo-nx's hot_layout.py (the release build's PGO already
      gathers the hot functions: -freorder-functions with the profile), switch_goto.py (Banjo's script interpreters;
      Peggle's hot list has none), limiter_lazy_log10.py (XAudio's limiter; Peggle's audio is Wwise), store_cards.py
      (Banjo's renderer).

ro_constants.py, ro_constants_cfg.py, leaf_dead_stores.py, dead_ctx_defs.py and the entry modes of flush_mode_flow.py
need the generator's image dump (out/default_image.bin, written by tools/codegen.ps1 through REXGLUE_DUMP_IMAGE).

Every step is idempotent and the whole sequence is too: a second run on a processed tree changes no file (checked on a
copy: this script twice with --gen <copy> and a hash of the tree after each run).

Environment (tests): PEGGLE1_DEAD_DEFS_OFF=1 takes dead_ctx_defs.py's rewrites back out; PEGGLE1_DEAD_DEFS_HOLD=1
leaves the files as tools/dead_defs_bisect.py set them; PEGGLE1_DEAD_DEFS_NO_ABI=1 runs it without --abi-indirect;
PEGGLE1_R1_LOCAL_ALL=1 gives every function the r1 mirror (for tools/locals_choice.py's measurement on a copy).

  python tools/post_codegen.py [--gen <generated folder>]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated')
IMAGE = os.path.join(ROOT, 'out', 'default_image.bin')
XEX = os.path.join(ROOT, 'assets', 'game_root', 'default.xex')
PROMOTIONS = os.path.join(APP, 'indirect_promotions.txt')
PCH = 'peggle1_pch.h'
CONFIG = os.path.join(APP, 'config')  # the choice lists (not app/: tools/leaf_dead_stores.py reads app/*.txt)


def run(*args):
    result = subprocess.run([sys.executable, *args], cwd=ROOT)
    if result.returncode != 0:
        sys.exit(f'post_codegen: {" ".join(os.path.basename(a) for a in args[:1])} failed ({result.returncode})')


def tool(name):
    return os.path.join(ROOT, 'tools', name)


def main():
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else GEN
    scratch = os.path.abspath(gen) != os.path.abspath(GEN)  # a copy for measurements: no localized copies
    tree = os.path.join(gen, 'default')
    pch = os.path.join(tree, PCH)
    if not os.path.isfile(IMAGE):
        sys.exit('post_codegen: out/default_image.bin is missing (the image dump of the code generator): run '
                 'tools\\codegen.ps1 once, it writes it (REXGLUE_DUMP_IMAGE)')
    if os.path.isfile(XEX) and os.path.getmtime(XEX) > os.path.getmtime(IMAGE):
        sys.exit('post_codegen: out/default_image.bin is older than assets/game_root/default.xex: run '
                 'tools\\codegen.ps1 (it writes the dump again)')
    xex = ['--xex', XEX] if os.path.isfile(XEX) else []
    os.environ.setdefault('REXSDK_DIR', os.path.join(ROOT, 'sdk'))
    proofs = os.path.join(ROOT, 'out', 'codegen_proofs')
    # banjo-nx's late rewrites out first: they change statements the analyses parse (r1l for ctx.r1,
    # REX_LR_WRITE_SAME, hoisted checks, inline copies, REX_VSEL). On a fresh codegen these find nothing.
    run(tool('inline_leaves.py'), '--gen', tree, '--restore')
    run(tool('vsel_neon.py'), '--gen', tree, '--restore')
    run(tool('r1_local.py'), '--gen', tree, '--restore')
    run(tool('lr_preserve.py'), '--gen', tree, '--out', proofs, '--restore')
    run(tool('flush_hoist.py'), '--gen', tree, '--restore')
    # And the rest of the round (peggle2-nx): its earlier rewrites changed what the base steps below decide on a second
    # run (loop barriers, flush-mode proofs, read-only folds). Each undone the way its step wrote it.
    run(tool('round_restore.py'), '--gen', tree)
    run(tool('pch_no_volatile.py'), pch)
    # Before direct_calls.py (a promoted call is a direct call like any other) and BEFORE loop_barriers.py: a loop
    # needs no barrier when every way round it passes an indirect call, and a promoted call is no longer one.
    run(tool('indirect_calls.py'), '--gen', tree, '--promote', PROMOTIONS)
    # REQUIRED with pch_no_volatile.py: without barriers GCC turns guest polling loops into infinite loops.
    run(tool('loop_barriers.py'), tree)
    run(tool('pch_cold_traps.py'), pch)
    run(tool('direct_calls.py'), '--redo', '--gen', tree)
    # After direct_calls.py: the flush-mode proofs depend on which callees are called directly.
    run(tool('guest_cas.py'), '--gen', tree)
    run(tool('flush_mode_flow.py'), '--redo', '--gen', tree, '--image', IMAGE)
    run(tool('vec_byteswap.py'), '--gen', tree)
    run(tool('pch_neon_shuffles.py'), pch)
    # ro_constants before float_loads (it folds the integer form of a float load).
    # --xex: the words of the XEX's import records are written by the loader (kernel variables' addresses): loads
    # of them stay loads.
    run(tool('ro_constants.py'), '--gen', tree, '--image', IMAGE, *xex)
    run(tool('float_loads.py'), '--gen', tree)
    # Marks only (REX_LR_READ, REX_LR_WRITE, REX_DEAD_SAVE, REX_CHAIN_STORE: the statements as before unless one of its
    # opt-in switches is defined, and none is): tools/lr_preserve.py reads the link-register marks.
    run(tool('frame_traffic.py'), '--gen', tree)
    run(tool('loop_locals.py'), '--gen', tree)
    run(tool('leaf_locals.py'), '--gen', tree)
    # peggle1-nx (2026-10-21): Peggle 2's game patches (mp_masters.py, fps_updates.py, costume_states.py) ran here; they
    # patch Peggle 2's functions and are not Peggle 1's.
    # banjo-nx's codegen round (each step with its CMake switch back, PEGGLE1_GEN_*), in banjo-nx's order.
    run(tool('vperm_one_source.py'), '--gen', tree)
    # fp_sign_ops: exact while the game never selects round-up / round-down (the "[census] FPSCR rounding mode" line of
    # peggle1_diag_runtime_census counts the guest's choices; docs/peggle1-notes.md has the count).
    run(tool('fp_sign_ops.py'), '--gen', tree)
    run(tool('leaf_dead_stores.py'), '--gen', tree, '--image', IMAGE)
    run(tool('single_floats.py'), '--gen', tree)
    run(tool('inline_memcalls.py'), '--gen', tree)
    run(tool('vec_unaligned_stores.py'), '--gen', tree)
    run(tool('ro_constants_cfg.py'), '--gen', tree, '--image', IMAGE, *xex)
    if os.environ.get('PEGGLE1_DEAD_DEFS_HOLD'):  # tools/dead_defs_bisect.py has set the files: leave them
        print('post_codegen: dead_ctx_defs.py held (PEGGLE1_DEAD_DEFS_HOLD)')
    elif os.environ.get('PEGGLE1_DEAD_DEFS_OFF'):
        run(tool('dead_ctx_defs.py'), '--gen', tree, '--restore-only')
    else:
        run(tool('dead_ctx_defs.py'), '--gen', tree, '--image', IMAGE,
            *([] if os.environ.get('PEGGLE1_DEAD_DEFS_NO_ABI') else ['--abi-indirect']))
    run(tool('flush_hoist.py'), '--gen', tree)
    if not scratch:
        # The localized native copies, made from the text as it stands after the steps above (the tool reads their
        # forms: tools/localize_native.py, plain() and unleaf()).
        run(tool('localize_native.py'), '--list', os.path.join(APP, 'src', 'cpu', 'hot', 'localized.txt'))
    # Link-register write-backs proven to store the value ctx.lr already holds compile to nothing on the Switch; the
    # PC build checks every one (REX_LR_WRITE_SAME_CHECK). A choice list, when there is one, limits them.
    # STRICT, not banjo-nx's --assume-indirect (2026-10-19): Peggle's functions that end through __restgprlr (the
    # generator drops the helper) never write ctx.lr back, and a call through a pointer or a hook reaches them: with the
    # assumption the PC check found 1,473 of 68 M checked write-backs wrong in 150 s (7,603 sites proven); strictly,
    # 2,642 sites.
    lr_choice = os.path.join(CONFIG, 'lr_write_same_choice.json')
    run(tool('lr_preserve.py'), '--gen', tree, '--out', proofs, '--rewrite',
        *(['--only-file', lr_choice] if os.path.isfile(lr_choice) else []))
    # r1 as a local mirror, only in the functions where it compiles cheaper (everywhere it is a loss, banjo-nx).
    r1_choice = os.path.join(CONFIG, 'r1_local_choice.json')
    r1_all = bool(os.environ.get('PEGGLE1_R1_LOCAL_ALL'))
    if r1_all or os.path.isfile(r1_choice):
        run(tool('r1_preserve.py'), '--gen', tree, '--out', os.path.join(proofs, 'strict'))
        run(tool('r1_preserve.py'), '--gen', tree, '--out', os.path.join(proofs, 'assumed'), '--indirect-preserve')
        run(tool('r1_local.py'), '--gen', tree, '--proof-strict', os.path.join(proofs, 'strict', 'r1_preserve.json'),
            '--proof-assumed', os.path.join(proofs, 'assumed', 'r1_preserve.json'),
            *([] if r1_all else ['--only-file', r1_choice]))
    run(tool('vsel_neon.py'), '--gen', tree)
    # Last of all: inline copies of the tiny leaves each file calls in another.
    run(tool('inline_leaves.py'), '--gen', tree)
    if scratch:
        print(f'post_codegen: optimised codegen steps done on {gen}')
        return
    keep_unchanged_times(GEN)
    print('post_codegen: optimised codegen steps done')


def keep_unchanged_times(gen):
    """From banjo-nx (2026-10-16, through sa2-nx): a CMake re-run runs the code generator again, which writes every
    generated file anew; the steps above then give most of them exactly the text they had. A file whose text is the
    one of the last run gets its old modification time back, so ninja does not compile it again. The times and hashes
    of the last run: app/generated/codegen.times.json."""
    import hashlib
    import json
    record = os.path.join(gen, 'codegen.times.json')
    try:
        old = json.load(open(record, encoding='utf-8'))
    except (OSError, ValueError):
        old = {}
    new = {}
    kept = 0
    # The inputs of the generator's build step besides the manifest: never give an output a time older than these
    # (ninja would then run the step on every build).
    inputs = [os.path.join(APP, n) for n in ('overrides.toml', 'perf_overrides.toml')] + [XEX]
    sdk_out = os.path.join(ROOT, 'sdk', 'out')
    if os.path.isdir(sdk_out):
        for d, _, fs in os.walk(sdk_out):
            inputs += [os.path.join(d, f) for f in fs if f in ('rexglue.exe', 'rexgluerd.exe', 'rexglued.exe')]
    floor = max([os.stat(p).st_mtime_ns for p in inputs if os.path.isfile(p)] or [0])
    paths = []
    for folder in (os.path.join(gen, 'default'), os.path.join(gen, 'hot')):
        if os.path.isdir(folder):
            paths += [os.path.join(folder, n) for n in sorted(os.listdir(folder)) if n.endswith(('.cpp', '.h', '.inc'))]
    for path in paths:
        rel = os.path.relpath(path, gen).replace(os.sep, '/')
        digest = hashlib.blake2b(open(path, 'rb').read(), digest_size=16).hexdigest()
        entry = old.get(rel)
        if entry and entry[0] == digest:
            when = max(entry[1], floor)
            os.utime(path, ns=(when, when))
            kept += 1
            new[rel] = [digest, when]
        else:
            new[rel] = [digest, os.stat(path).st_mtime_ns]
    # The generator writes its manifest again on every run (the same text); a manifest newer than the outputs kept
    # above would make every build run the generator again. It gets the time of the oldest output.
    manifest = os.path.join(APP, 'peggle1_manifest.toml')
    if new and os.path.isfile(manifest):
        oldest = min(os.stat(os.path.join(gen, rel)).st_mtime_ns for rel in new)
        if os.stat(manifest).st_mtime_ns > oldest:
            os.utime(manifest, ns=(oldest, oldest))
    with open(record, 'w', encoding='utf-8') as f:
        json.dump(new, f)
    print(f'post_codegen: {kept} of {len(new)} generated files unchanged since the last run (their old times kept)')


if __name__ == '__main__':
    main()
