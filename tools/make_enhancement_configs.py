#!/usr/bin/env python3
"""Writes the premade Switch configs from the enhancement table (app/src/peggle1_enhancements.cpp, kEnhancements), so
an enhancement is added in one place only. From futari-nx's tool of the same name (the same configs in every port):

  app/switch/peggle1.toml               the default peggle1.toml of the SD card: every enhancement on
  app/configs/peggle1_cpu_on.toml       every CPU and non-CPU enhancement on (the default, named for A/B logs)
  app/configs/peggle1_cpu_off.toml      every CPU enhancement off (CPU master off, each key at its off value),
                                        non-CPU on: the A/B baseline
  app/configs/peggle1_cpu_verify.toml   all on + peggle1_cpu_verify: every fast path's self-check compares on every
                                        use; the "[verify] ... differences: N" log lines must say 0. Slower: test
                                        sessions only
  app/configs/peggle1_profile.toml      all on (named for profiling sessions)

The profiler (switch_profile_stacks: the console CPU sampler; masseffect_gpu_marks_categories: GPU time per pass
category) is ON in the default config and every premade one, OFF only in peggle1_cpu_verify.toml; its keys sit outside
the enhancement table, so a master switch cannot turn it off.

The configs differ only in what their name says, so two logs compare directly. To use one, copy it to
sdmc:/switch/peggle2-nx/peggle1.toml (the game reads only that name). tools/win/package_switch.ps1 puts app/configs/
into the SD folder as configs/.

  python tools/make_enhancement_configs.py
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, 'app', 'src', 'peggle1_enhancements.cpp')

HEADER = """# peggle2-nx settings for the Nintendo Switch - {title}
# Written by tools/make_enhancement_configs.py from app/src/peggle1_enhancements.cpp: edit the table there, not this
# file. To use it: copy it to sdmc:/switch/peggle2-nx/ as peggle1.toml (the game reads only that name). The startup
# log line "[enhancements]" says which enhancements were active.
{extra}
# The full game, not the trial: XBLA titles ask the console for their licence (0 would be the trial version). It
# is also the program's default when the line is missing.
license_mask = 1

# Language: "en", "de", "fr", "es", "it" or "pt" (Brazilian Portuguese). Empty = English.
peggle1_language = ""

# Pictures a second: 30 = as the Xbox 360 game (a picture every second screen refresh), 60 = a picture every refresh.
# The game runs at the same speed either way: it makes 60 logic steps a second and, with 60, draws after each one
# instead of after every second one. A picture that is not ready in time takes two steps, as with 30. With 60 the log
# has a "[fps]" line every 10 s (pictures and logic steps a second).
peggle1_fps = {fps}

# Local multiplayer. A second, third or fourth controller that joins a lobby is signed in as a guest of the profile,
# as on the console (false: only controller 1 can play). With the game data pack that has the PS4 content the
# lobby offers Windy and Jimmy too, in the PS4's layout of eight smaller buttons (peggle1_dlc_mp_masters = false:
# the Xbox 360's lobby of five masters).
xam_guest_signin = true
peggle1_dlc_mp_masters = true

# Single player, "Select a Master" (the screen a beaten level opens with). With the PS4 content: five masters in
# the row, Jimmy, Windy and the random button in a second row as on the PS4, and Y steps through all six costumes
# of a master (the two costume packs are the fifth and the sixth). false: the Xbox 360's screen (one row made for
# five masters, four costumes).
peggle1_dlc_master_select = true

# Controllers (up to four: the handheld Joy-Cons, Pro Controllers, Joy-Con pairs, or single Joy-Cons held sideways).
# Face buttons by position, as on the Xbox 360 pad the game was made for: the bottom button (B on Switch
# controllers) is A, the right one (A) is B, the left one (Y) is X, the top one (X) is Y. false = by letter.
input_xbox_layout = true
# Single Joy-Cons are held sideways, one per player.
input_joycon_sideways = true
# Each player's own layout: which Switch buttons press an Xbox button, as a list of A B X Y L R ZL ZR PLUS MINUS
# LSTICK RSTICK UP DOWN LEFT RIGHT SL SR, or NONE. Keys: input_p1_ to input_p4_ followed by a b x y lb rb lt rt start
# back ls rs up down left right. Empty or missing = the default above. Examples (remove the # to use one):
# input_p1_a = "B, ZR"          # player 1: B or ZR shoots
# input_p2_lb = "L, SL"         # player 2: L or SL is the left bumper
# input_p3_start = "PLUS, MINUS"

{renderer}

{log}
"""
RENDERER_NATIVE = """# Renderer: the native Vulkan renderer (PM4 ring -> NVK). It needs peggle1_shaders.mesp (+ .idx) next to the NRO,
# built from your own game files (tools/build_shader_library.sh). false = ReXGlue's emulated Xenos GPU: the reference
# image, but far too slow on the console. An NRO built without the Vulkan driver (development builds since
# 2026-10-20) ignores these two lines and runs Renut's renderer on deko3d (deko3d_glsl.bin next to the NRO).
masseffect_renderer_native = true
gpu_plugin = "xenos\""""
# 2026-10-20 (the user: "a fully functional deko 3d native renderer with no ring emulation or edram emulation"):
# Renut's Direct3D-level renderer (reNut by masterspike52, as banjo-nx carries it), on deko3d.
RENDERER_RENUT = """# Renderer: Renut's Direct3D-level renderer on deko3d (reNut by masterspike52, through banjo-nx): the game's
# Direct3D calls are drawn directly, with no PM4 ring and no EDRAM model. The game's own shaders come from
# deko3d_glsl.bin next to the NRO (the web installer makes it from your game files); the shader variants it compiles
# are kept in deko3d_shaders.bin there (copy the one that comes with the build to skip the first compiles). It
# follows peggle1_render_height_handheld / _docked and shows the HD atlases as the native renderer does.
# peggle1_renderer = "native" goes back to the native renderer, in an NRO that has the Vulkan driver (development
# builds since 2026-10-20 have none: Renut's renderer always).
peggle1_renderer = "renut"
renut_gpu_backend = "deko3d"
masseffect_renderer_native = false"""
# 2026-10-20: log_to_file too. The NRO's built-in settings (since 1.0.0) have log_to_file = false, so a config that only
# set the level wrote no file (seen in an emulator run of peggle1_renut_deko3d.toml: no logs/ folder at all).
LOG_ON = ('# Logs go to logs/ next to the NRO. Please keep them after a test run.\nlog_level = "info"\n'
          'log_to_file = true')
# Asked for on 2026-10-14 ("a config with no logging"): "off" is a level of the SDK's log_level. stderr stays
# redirected to logs/rex/rex_stderr.log (sdk/src/ui/switch_crash_hooks.c: a few lines of the graphics driver at
# start) and a crash still writes rex_crash.log. Use it to play; for a fault, switch back to a config with logs.
LOG_OFF = ('# No log: the program writes no log lines (log_level = "off"). Only logs/rex/rex_stderr.log (a few lines\n'
           '# of the graphics driver at start) and, after a crash, rex_crash.log are still written. For a fault\n'
           '# report, use a config with log_level = "info" and reproduce it.\nlog_level = "off"')


# The release build (2026-10-14, the user's request for version 1.0.0): "hardcode all optimisations on ... turn
# logging off, enable all DLC and force the 1080p assets always for handheld and docked ... only things I want to show
# up is controller mapping settings, framerate settings make the default cap 30 like 360 version, and language
# setting". The one supported game data is the Xbox 360 game with the PS4 content and the HD pack. As sa2-nx does it:
#   app/peggle1_switch_defaults.toml  compiled into the Switch NRO (src/peggle1_app.h BuiltInConfig): every fixed
#                                     setting, applied before the SD card's peggle1.toml, which may override any key;
#   app/peggle1_switch.toml           the player's peggle1.toml, written next to the NRO at the first start when there
#                                     is none (DefaultConfigFile): frame rate, language and the controllers only.
RELEASE_DEFAULTS_HEAD = """# peggle2-nx: the Switch build's fixed settings, compiled into the NRO (app/CMakeLists.txt, src/peggle1_app.h
# BuiltInConfig). Written by tools/make_enhancement_configs.py: edit that script or the enhancement table
# (app/src/peggle1_enhancements.cpp), not this file. The SD card's peggle1.toml is applied after this and may set any
# of these keys again (a test config from configs/ does).

# The full game (XBLA titles ask the console for their licence).
license_mask = 1

# Renut's Direct3D-level renderer on deko3d (reNut by masterspike52, through banjo-nx): no PM4 ring, no EDRAM model.
# The game's own shaders come from deko3d_glsl.bin beside the NRO (the web installer makes it from the game files).
# The NRO has no Vulkan driver, so this is its only renderer.
peggle1_renderer = "renut"
renut_gpu_backend = "deko3d"
masseffect_renderer_native = false

# 60 pictures a second (the user's choice of 2026-10-20: the deko3d renderer leaves the room); the player's
# peggle1.toml chooses (30 = as the Xbox 360 game).
peggle1_fps = 60

# No log: no log lines, no log file, no logs/ folder (a crash still writes logs/rex/rex_crash.log), and no console
# profile file.
log_level = "off"
log_to_file = false
switch_profile_report = false
switch_profile_stacks = false
masseffect_gpu_marks_categories = false

# All of the PS4 content: Windy and Jimmy in the multiplayer lobby and on Select a Master, guests for controllers 2
# to 4, and the PS4 powers' screen layers and motion.
xam_guest_signin = true
peggle1_dlc_mp_masters = true
peggle1_dlc_master_select = true
peggle1_dlc_overlays = true
peggle1_dlc_motion = true

# 1080p art always (the HD pack, peggle1_hd.pack beside the NRO): 1920x1080 when docked, 1280x720 drawn from the HD
# art in handheld mode.
peggle1_render_height_handheld = 720
peggle1_render_height_docked = 1080
peggle1_hd_atlases = 2

# ---- Enhancement masters ----
peggle1_cpu_enhancements = true
peggle1_noncpu_enhancements = true

# ---- CPU enhancements ({n_cpu}) ----
{cpu}

# ---- Non-CPU enhancements ({n_noncpu}) ----
{noncpu}
"""

RELEASE_PLAYER = """# Peggle 2 for the Nintendo Switch: your settings. The game writes this file when there is none; delete it to get
# the defaults back. Everything else is built into peggle1.nro.

# Pictures a second: 60 = a picture every screen refresh, 30 = as the Xbox 360 game. The game runs at the same speed
# either way (60 logic steps a second); 60 needs more of the console and is not reached everywhere.
peggle1_fps = 60

# Language: "en", "de", "fr", "es", "it" or "pt" (Brazilian Portuguese). Empty = English.
peggle1_language = ""

# Controllers (up to four: the handheld Joy-Cons, Pro Controllers, Joy-Con pairs, or single Joy-Cons held sideways).
# Face buttons by position, as on the Xbox 360 pad the game was made for: the bottom button (B on Switch
# controllers) is A, the right one (A) is B, the left one (Y) is X, the top one (X) is Y. false = by letter.
input_xbox_layout = true
# Single Joy-Cons are held sideways, one per player.
input_joycon_sideways = true
# Each player's own layout: which Switch buttons press an Xbox button, as a list of A B X Y L R ZL ZR PLUS MINUS
# LSTICK RSTICK UP DOWN LEFT RIGHT SL SR, or NONE. Keys: input_p1_ to input_p4_ followed by a b x y lb rb lt rt start
# back ls rs up down left right. Empty or missing = the default above. Examples (remove the # to use one):
# input_p1_a = "B, ZR"          # player 1: B or ZR shoots
# input_p2_lb = "L, SL"         # player 2: L or SL is the left bumper
# input_p3_start = "PLUS, MINUS"
"""


def write_release(app, rows):
    n_cpu = sum(r['cpu'] for r in rows)
    text = RELEASE_DEFAULTS_HEAD.format(n_cpu=n_cpu, n_noncpu=len(rows) - n_cpu, cpu=section(rows, True, True, {}),
                                        noncpu=section(rows, False, True, {}))
    for name, body in (('peggle1_switch_defaults.toml', text), ('peggle1_switch.toml', RELEASE_PLAYER)):
        with open(os.path.join(app, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(body)


def parse_table(path):
    text = open(path, encoding='utf-8').read()
    rows = re.findall(r'\{"([a-z0-9_]+)",\s*Group::(kCpu|kNonCpu),\s*"([a-z0-9_]+)",\s*"([^"]*)",\s*"([^"]*)",\s*'
                      r'"((?:[^"\\]|\\.)*)"(,\s*kExplicitOnly)?\}', text)
    if not rows:
        raise SystemExit('no rows found in kEnhancements of ' + path)
    return [dict(name=n, cpu=g == 'kCpu', key=k, on=on, off=off, what=w, explicit=bool(x))
            for n, g, k, on, off, w, x in rows]


def string_literals(text):
    """The concatenation of the adjacent C string literals at the start of `text` (a macro's description)."""
    out = []
    for m in re.finditer(r'\s*"((?:[^"\\]|\\.)*)"', text):
        if m.start() != (out[-1][0] if out else 0):
            break
        out.append((m.end(), m.group(1)))
    return ''.join(s for _, s in out).replace('\\"', '"')


def parse_registered():
    """The keys registered in the sources rather than in the table: PEGGLE1_NATIVE (native-hook settings) and
    PEGGLE1_RING_ENHANCEMENT (the renderer's CPU enhancements from sa2-nx), CPU group; PEGGLE1_RING_GPU_ENHANCEMENT
    (sa2-nx's non-CPU renderer keys: GPU work, fidelity fixes), non-CPU group; with their code defaults."""
    rows = []
    src = os.path.join(ROOT, 'app', 'src')
    for folder, _, files in os.walk(src):
        for f in sorted(files):
            if not f.endswith('.cpp'):
                continue
            text = open(os.path.join(folder, f), encoding='utf-8').read()
            for m in re.finditer(r'^\s*PEGGLE1_(NATIVE|RING_ENHANCEMENT|RING_GPU_ENHANCEMENT)\((\w+), (\w+),'
                                 r'(?: (true|false),)?', text, re.M):
                kind, _, key, default = m.groups()
                desc = string_literals(text[m.end():])
                on = default or 'true'
                name = re.sub(r'^peggle1_(cpu_|gpu_|native_|api_)', lambda x: 'api_' if x.group(1) == 'api_' else '',
                              key)
                rows.append(dict(name=name, cpu=kind != 'RING_GPU_ENHANCEMENT', key=key, on=on, off='false',
                                 what=desc, explicit=on == 'false'))
    return rows


def parse_natives():
    """The native replacements (CPU group, key default on): the hand-written hooks of peggle1_hot_guest.cpp and the
    localized copies listed in src/cpu/hot/localized.txt."""
    rows = []
    hot = open(os.path.join(ROOT, 'app', 'src', 'cpu', 'peggle1_hot_guest.cpp'), encoding='utf-8').read()
    for addr, key, desc in re.findall(r'^PEGGLE1_HOT_HOOK\((\w+), (\w+), "([^"]*)"', hot, re.M):
        rows.append(dict(name=key.replace('peggle1_cpu_', ''), cpu=True, key=key, on='true', off='false', what=desc,
                         explicit=False))
    for line in open(os.path.join(ROOT, 'app', 'src', 'cpu', 'hot', 'localized.txt'), encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        addr, _, what = line.partition(' ')
        key = f'peggle1_cpu_native_sub_{addr.upper()}'
        rows.append(dict(name=key.replace('peggle1_cpu_', ''), cpu=True, key=key, on='true', off='false',
                         what=f'localized copy of sub_{addr.upper()}: {what.strip()}', explicit=False))
    return rows


def value(v):
    return v if v in ('true', 'false') or re.fullmatch(r'-?\d+', v) else '"%s"' % v


def section(rows, cpu, state, overrides):
    lines = []
    for r in rows:
        if r['cpu'] != cpu:
            continue
        v = overrides.get(r['key'], r['on'] if state and not r['explicit'] else r['off'])
        note = '  (set by this config)' if r['key'] in overrides else ''
        if r['explicit'] and r['key'] not in overrides:
            note += '  (EXPLICIT ONLY: off unless you set it; see the known issues)'
        lines.append(f"{r['key']} = {value(v)}".ljust(56) + f"# {r['name']}: {r['what']}{note}")
    return '\n'.join(lines)


# The profiler (the user's request, relayed by the SA2 session): ON in the default config and every premade config,
# OFF only in the verify config. Plain keys outside the enhancement table, so no master switch turns it off; written
# explicitly into every config, so a config stays as it says even if a code default changes.
PROFILING = {
    'switch_profile_stacks': ('true', 'console CPU sampler: 1 ms stack samples into logs/rex/rex_profile.log'),
    'masseffect_gpu_marks_categories': ('true', 'GPU time per pass category and the idle gap ("GPU per Swap" lines)'),
}
NO_PROFILING = {k: ('false', what) for k, (_, what) in PROFILING.items()}


def profiler_section(profiling):
    on = profiling is PROFILING
    lines = [f"\n# ---- Profiler ({'on' if on else 'OFF in this config'}; not under the enhancement masters) ----"]
    for key, (v, what) in profiling.items():
        lines.append(f'{key} = {v}'.ljust(56) + f'# {what}')
    return '\n'.join(lines) + '\n'


def write(path, title, extra, rows, cpu_on, tail='', overrides=None, profiling=PROFILING):
    overrides = overrides or {}
    n_cpu = sum(r['cpu'] for r in rows)
    text = HEADER.format(title=title, extra=extra, fps=overrides.get('peggle1_fps', '60'),
                         renderer=RENDERER_RENUT if overrides.get('renderer') == 'renut' else RENDERER_NATIVE,
                         log=LOG_OFF if overrides.get('log_level') == 'off' else LOG_ON)
    text += profiler_section(profiling)
    text += f"""
# ---- Enhancement masters (false turns every enhancement of that group off, whatever its own key says) ----
peggle1_cpu_enhancements = {'true' if cpu_on else 'false'}
peggle1_noncpu_enhancements = true

# ---- CPU enhancements ({n_cpu}) ----
{section(rows, True, cpu_on, overrides)}

# ---- Non-CPU enhancements ({len(rows) - n_cpu}) ----
{section(rows, False, True, overrides)}
"""
    text += tail
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


DLC_TAIL = ('\n# ---- The PS4 powers (a feature, no enhancement) ----\npeggle1_dlc_overlays = true\n'
            'peggle1_dlc_motion = true\n')
HD_HEAD = ('\n# ---- Render resolution and HD atlases (heights: multiples of 180; atlases: 0 never, 1 above 720p, '
           '2 always) ----\n')


def main():
    rows = parse_table(TABLE) + parse_registered() + parse_natives()
    app = os.path.join(ROOT, 'app')
    configs = {
        'peggle1_cpu_on.toml': ('ALL CPU ENHANCEMENTS ON', '', True, '', {}, PROFILING),
        # Asked for on 2026-10-13: everything on, with the finished parts of the PS4 content that are still off by
        # default (the powers themselves are on by default). No extra logging, unlike peggle1_test_dlc_look.toml.
        'peggle1_all_on_dlc.toml': (
            'ALL ENHANCEMENTS ON, with the PS4 powers\' screen layers and motion',
            '# For the converted archive (the pack of peggle2-nx-dlc). As peggle1_cpu_on.toml, plus Windy\'s ultraviolet\n'
            '# screen with the holes around her fairies, Jimmy\'s spotlights and ball holes, the fairies\' wobbling\n'
            '# flight and Jimmy\'s look-ahead with bounces. With the stock archive the two keys change nothing.\n',
            True, '\n# ---- The PS4 powers (a feature, no enhancement) ----\npeggle1_dlc_overlays = true\n'
                  'peggle1_dlc_motion = true\n', {}, PROFILING),
        # Asked for on 2026-10-14: "a config with no logging plus 30fps cap plus all optimisations on like the all
        # on build": peggle1_all_on_dlc.toml with log_level = "off" and the profiler off; then "regen it with hd
        # art always": the HD atlases in handheld mode too (peggle1_hd_atlases = 2, as peggle1_hd_always.toml).
        'peggle1_all_on_quiet.toml': (
            'ALL ENHANCEMENTS ON, HD ART ALWAYS, 60 pictures a second, NO LOGGING, no profiler',
            '# To play: as peggle1_all_on_dlc.toml (every enhancement, the PS4 powers\' screen layers and motion),\n'
            '# with 60 pictures a second, no log and no profiler, and the HD atlases always: 1080p with them when\n'
            '# docked, 720p drawn from them (filtered down) in handheld mode. Needs peggle1_hd.pack beside the NRO.\n',
            True, DLC_TAIL + HD_HEAD + 'peggle1_render_height_handheld = 720\npeggle1_render_height_docked = 1080\n'
                  'peggle1_hd_atlases = 2\n', {'peggle1_fps': '60', 'log_level': 'off'}, NO_PROFILING),
        # 2026-10-13: render resolution per console mode and the HD atlas pack (peggle2-nx-hd). All four are
        # peggle1_all_on_dlc.toml plus the settings named; the first writes the defaults out.
        'peggle1_hd_docked.toml': (
            'HD: 1080p when docked with the HD atlases, 720p handheld as before (the defaults, written out)',
            '# Needs peggle1_hd.pack beside the NRO for the HD atlases (without it only the resolution changes).\n',
            True, DLC_TAIL + HD_HEAD + 'peggle1_render_height_handheld = 720\npeggle1_render_height_docked = 1080\n'
                  'peggle1_hd_atlases = 1\n', {}, PROFILING),
        'peggle1_hd_always.toml': (
            'HD: 1080p when docked, and the HD atlases in handheld mode too (filtered down to the 720p picture)',
            '# Needs peggle1_hd.pack beside the NRO. Handheld draws 1280x720 as before, from the larger atlases.\n',
            True, DLC_TAIL + HD_HEAD + 'peggle1_render_height_handheld = 720\npeggle1_render_height_docked = 1080\n'
                  'peggle1_hd_atlases = 2\n', {}, PROFILING),
        'peggle1_hd_handheld_1080.toml': (
            'HD: 1080p in both modes with the HD atlases (handheld: drawn at 1080p and scaled to the 720p screen)',
            '# Needs peggle1_hd.pack beside the NRO. About 2.25 times the pixels to draw in handheld mode: compare\n'
            '# the frame rate with peggle1_hd_always.toml.\n',
            True, DLC_TAIL + HD_HEAD + 'peggle1_render_height_handheld = 1080\npeggle1_render_height_docked = 1080\n'
                  'peggle1_hd_atlases = 1\n', {}, PROFILING),
        'peggle1_hd_docked_720_atlases.toml': (
            'HD TEST: 1080p when docked with the game\'s own 720 atlases (no HD pack used)',
            '# What the resolution alone costs and looks like: compare with peggle1_hd_docked.toml.\n',
            True, DLC_TAIL + HD_HEAD + 'peggle1_render_height_handheld = 720\npeggle1_render_height_docked = 1080\n'
                  'peggle1_hd_atlases = 0\n', {}, PROFILING),
        'peggle1_hd_off.toml': (
            'HD OFF: 1280x720 in both modes with the game\'s own atlases (as every build before 2026-10-13 21:00)',
            '# The baseline for the frame rate and the picture of the HD configs.\n',
            True, DLC_TAIL + HD_HEAD + 'peggle1_render_height_handheld = 720\npeggle1_render_height_docked = 720\n'
                  'peggle1_hd_atlases = 0\n', {}, PROFILING),
        # 2026-10-14: 60 pictures a second (peggle1_fps; the key is part of every config's header, 30 elsewhere).
        'peggle1_60fps.toml': (
            '60 PICTURES A SECOND, otherwise the defaults (1080p with the HD atlases when docked, 720p handheld)',
            '# As peggle1_hd_docked.toml with peggle1_fps = 60. Seen on the PC only: whether the console reaches 60 is\n'
            '# not known. The "[fps]" lines of the log say what it reached; the game\'s speed is right either way.\n'
            '# If docked mode stays well under 60, try peggle1_60fps_720.toml.\n',
            True, DLC_TAIL + HD_HEAD + 'peggle1_render_height_handheld = 720\npeggle1_render_height_docked = 1080\n'
                  'peggle1_hd_atlases = 1\n', {'peggle1_fps': '60'}, PROFILING),
        'peggle1_60fps_720.toml': (
            '60 PICTURES A SECOND at 1280x720 in both modes with the game\'s own atlases (the lightest picture)',
            '# As peggle1_hd_off.toml with peggle1_fps = 60: the least work per picture, to see whether 60 is reached.\n',
            True, DLC_TAIL + HD_HEAD + 'peggle1_render_height_handheld = 720\npeggle1_render_height_docked = 720\n'
                  'peggle1_hd_atlases = 0\n', {'peggle1_fps': '60'}, PROFILING),
        # Asked for on 2026-10-14: the HD pack in both modes, 60 pictures a second, every enhancement on.
        'peggle1_60fps_hd_always.toml': (
            '60 PICTURES A SECOND with the HD atlases in BOTH modes, every enhancement on',
            '# Needs peggle1_hd.pack beside the NRO. Docked: 1920x1080 from the HD atlases. Handheld: 1280x720 (the\n'
            '# screen\'s own size) drawn from the HD atlases, filtered down. peggle1_fps = 60 above. The heaviest of the\n'
            '# premade configs: seen on the PC only. The "[fps]" lines of the log say what the console reaches (the\n'
            '# game\'s speed is right either way); if it stays well under 60, peggle1_60fps.toml (HD atlases only when\n'
            '# docked) and peggle1_60fps_720.toml (no HD atlases, 720p) are lighter.\n',
            True, DLC_TAIL + HD_HEAD + 'peggle1_render_height_handheld = 720\npeggle1_render_height_docked = 1080\n'
                  'peggle1_hd_atlases = 2\n', {'peggle1_fps': '60'}, PROFILING),
        # 2026-10-20: Renut's Direct3D-level renderer on deko3d (no PM4 ring, no EDRAM model), everything else as
        # peggle1_all_on_dlc.toml with the release's picture (720p handheld, 1080p docked, HD atlases always), logs on
        # for the first console runs.
        'peggle1_renut_deko3d.toml': (
            'RENUT\'S RENDERER ON DEKO3D (Direct3D level: no PM4 ring, no EDRAM model), every enhancement on',
            '# The deko3d renderer of 2026-10-20 (docs/peggle1-notes.md, "Renut\'s renderer on deko3d"). 720p handheld,\n'
            '# 1080p docked, the HD atlases always (peggle1_hd.pack beside the NRO), as the release build, at 60 pictures a\n'
            '# second (peggle1_fps = 60; the user\'s choice of 2026-10-20; 30 is the console\'s own rate). Logs on: please\n'
            '# keep logs/ after a run.\n',
            True, DLC_TAIL + HD_HEAD + 'peggle1_render_height_handheld = 720\npeggle1_render_height_docked = 1080\n'
                  'peggle1_hd_atlases = 2\n', {'renderer': 'renut', 'peggle1_fps': '60'}, PROFILING),
        'peggle1_cpu_off.toml': ('ALL CPU ENHANCEMENTS OFF (A/B baseline)', '', False, '', {}, PROFILING),
        'peggle1_cpu_verify.toml': (
            'CPU VERIFY: all on, every fast path checked against the original on every use',
            '# Slower than peggle1_cpu_on.toml (every comparison runs the original path too): for a test session.\n'
            '# Every 10 s the log has "[verify] checks done: ... | differences: N": N must stay 0. A difference also\n'
            '# logs a DIFFERENCE or MISMATCH line and turns that one fast path off for the rest of the session.\n'
            '# The profiler is OFF here (the only config without it).\n',
            True, '\n# ---- Verify mode ----\npeggle1_cpu_verify = true\n', {}, NO_PROFILING),
        'peggle1_profile.toml': (
            'PROFILE: all on (the profiler is on in every config but the verify one)',
            '# rex_profile.log: python tools/switch_hot_functions.py logs/rex/rex_profile.log --map out/nx/peggle1.map\n'
            '# (the map must come from the same build as the NRO).\n',
            True, '', {}, PROFILING),
        # One-key A/B configs of 2026-10-13 (the same NRO, compared with peggle1_cpu_on.toml).
        'peggle1_test_vsync_relaxed_off.toml': (
            'TEST: all on, relaxed vsync OFF (the fixed 1/60 s vblanks)',
            '# Compare the frame rate in a heavy scene with peggle1_cpu_on.toml: with the ring thread or the GPU over\n'
            '# 33 ms per frame the fixed vblanks hold every frame to 50 ms (20 frames a second).\n',
            True, '', {'peggle1_vsync_relaxed': 'false'}, PROFILING),
        'peggle1_test_gpu_boost_off.toml': (
            'TEST: all on, the handheld GPU profile request OFF (the console\'s own GPU clock)',
            '# Compare the "GPU per Swap" lines and the frame rate in handheld mode with peggle1_cpu_on.toml.\n',
            True, '', {'peggle1_gpu_boost': 'false'}, PROFILING),
        # The PS4 release's two powers (app/src/dlc): a feature, not an enhancement; these two configs are for a
        # console run with the converted archive (tools/ps4_dlc).
        'peggle1_test_dlc_log.toml': (
            'TEST: all on, the events of the PS4 powers and the game\'s own error messages in the log',
            '# For a run with the converted archive: every fairy, multiball and style shot is logged ([dlc] lines), and\n'
            '# a resource group the game does not find is named instead of only crashing later.\n',
            True, '\n# ---- The PS4 powers (a feature, no enhancement) ----\npeggle1_dlc_log = true\n', {}, PROFILING),
        'peggle1_test_dlc_powers_off.toml': (
            'TEST: all on, the PS4 powers OFF (the Xbox 360 executable as it shipped)',
            '# With the stock archive nothing changes. With the converted archive the areas of Windy and Jimmy do not\n'
            '# load (no PurplePegs class) or play the early Multiball: only to tell the cost of the module or a fault.\n',
            True, '\n# ---- The PS4 powers (a feature, no enhancement) ----\npeggle1_dlc_powers = false\n', {},
            PROFILING),
        # 2026-10-13: the powers' screen layers and motion, both off by default until a console run has shown their
        # picture and their cost (Jimmy's layer is a mesh of up to 2,514 triangles every frame of his power).
        'peggle1_test_dlc_look.toml': (
            'TEST: all on, the PS4 powers\' screen layers and motion ON (new, off by default)',
            '# For a run with the converted archive. peggle1_dlc_overlays: Windy\'s ultraviolet screen with holes\n'
            '# around the new fairies, Jimmy\'s spotlights and ball holes. peggle1_dlc_motion: fairies fly the PS4\'s\n'
            '# wobbling path, Jimmy\'s slow-motion look-ahead follows bounces. Seen on the PC only; compare the frame\n'
            '# rate during Jimmy\'s power with peggle1_cpu_on.toml.\n',
            True, '\n# ---- The PS4 powers (a feature, no enhancement) ----\npeggle1_dlc_overlays = true\n'
                  'peggle1_dlc_motion = true\npeggle1_dlc_log = true\n', {}, PROFILING),
        'peggle1_test_cpu_rounds_runtime_off.toml': (
            'TEST: all on, the run-time keys of the CPU rounds of 2026-10-13 OFF',
            '# Only what a key can switch: the barrier after a guest compare-and-swap is back, the hooks count in\n'
            '# their shared counter, critical sections look their thread up, every signal notifies. The generated\n'
            '# code of the rounds (constants, locals, promoted calls, float modes) is the NRO itself.\n',
            True, '', {'peggle1_cpu_guest_cas_no_barrier': 'false', 'peggle1_cpu_hook_counter_ctx': 'false',
                       'kernel_critsec_pcr_owner': 'false', 'thread_signal_skip_idle': 'false'}, PROFILING),
        # The console's graphics corruption of 2026-10-13 (large atlases partly garbage, the PC right): two runs
        # that say at which stage the bytes go wrong (tools/texdiag_check.py reads the log and the texdiag/ folder).
        'peggle1_test_texdiag.toml': (
            'TEST: all on, the data checks along the path of a texture ON',
            '# Nothing else changes. The log gets a CRC-32 of every file read of 4 KB or more ("[io] crc"), of every\n'
            '# stream the game inflates ("[texdiag] stream") and of every texture of 1 MB or more the renderer picks up\n'
            '# ("[texdiag] texture"); the first large textures are written to texdiag/ next to the NRO (up to 72 MB).\n'
            '# Run to the first level, then send logs/ and the texdiag/ folder.\n',
            True, '\n# ---- Data checks (diagnostic) ----\npeggle1_diag_textures = true\nio_diag_crc = true\n'
                  'pack_log_fallbacks = true\n', {}, PROFILING),
        'peggle1_test_texdiag_verify.toml': (
            'TEST: CPU VERIFY (every fast path checked against the original) with the data checks ON',
            '# peggle1_cpu_verify.toml plus the keys of peggle1_test_texdiag.toml: one run says whether a fast path\n'
            '# differs from the original ("[verify] ... differences: N", DIFFERENCE or MISMATCH lines) and at which\n'
            '# stage a texture\'s bytes go wrong. Slower than the normal configs. The profiler is OFF here.\n',
            True, '\n# ---- Verify mode ----\npeggle1_cpu_verify = true\n'
                  '\n# ---- Data checks (diagnostic) ----\npeggle1_diag_textures = true\nio_diag_crc = true\n'
                  'pack_log_fallbacks = true\n', {}, NO_PROFILING),
        'peggle1_test_texdiag_safe.toml': (
            'TEST: the data checks ON, and three fast paths for bulk data OFF',
            '# As peggle1_test_texdiag.toml, plus: pack reads go through a host buffer (the file system does not write\n'
            '# guest memory), every texture gets its own GPU allocation (no pool), and the game\'s own memcpy and memset\n'
            '# run instead of the native ones. If the picture is right with this one, one of the three is the cause.\n',
            True, '\n# ---- Data checks (diagnostic) ----\npeggle1_diag_textures = true\nio_diag_crc = true\n'
                  'pack_log_fallbacks = true\n'
                  '\n# ---- Bulk data the slow way ----\npack_direct_reads = false\n'
                  'masseffect_native_textures_pool = false\n',
            {'peggle1_cpu_native_memcpy': 'false', 'peggle1_cpu_native_memset': 'false'}, PROFILING),
    }
    for name, (title, extra, cpu_on, tail, overrides, profiling) in configs.items():
        write(os.path.join(app, 'configs', name), title, extra, rows, cpu_on, tail, overrides, profiling)
    write(os.path.join(app, 'switch', 'peggle1.toml'), 'default settings: every enhancement on', '', rows, True)
    write_release(app, rows)
    print('wrote app/peggle1_switch_defaults.toml (built into the Switch NRO) and app/peggle1_switch.toml (the '
          'player\'s peggle1.toml)')
    n_cpu = sum(r['cpu'] for r in rows)
    print(f'{len(rows)} enhancements ({n_cpu} CPU, {len(rows) - n_cpu} non-CPU); wrote app/switch/peggle1.toml and '
          f'app/configs/{", ".join(configs)}')


if __name__ == '__main__':
    main()
