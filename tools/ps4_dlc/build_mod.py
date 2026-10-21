"""Builds a copy of the Xbox 360 main.rsb with content taken from the PS4 release of Peggle 2 (the user's own dumps of
both). Nothing is written next to the originals; the output goes where --out says.

  python -P -E tools/ps4_dlc/build_mod.py --x360 assets/game_root/main.rsb --ps4 <ps4 main.rsb>
      --out out/ps4_dlc/game_root_mod/main.rsb [--content costumes,jimmy,...] [--cache DIR] [--previews DIR]
      [--test-unlock-all] [--test-first-costume Bjorn=Garden]

Content (see docs/peggle1-notes.md, "PS4 content in the Xbox 360 build"):
  costumes   the Garden and Spooky costume packs of the five launch masters
  jimmy      Jimmy Lightning: his six costumes' art and animations and his MASTERS.JSON entry
  windy      Windy the Fairy: the same
  levels     the two areas of the PS4's second story (Windy's and Jimmy's levels and trials) as areas 7 and 8
  strings    the PS4 string tables' entries the 360's lack (names of the new masters, powers, style shots, levels)
  sound      the PS4's sound objects merged into the 360's structure bank and the media of its sounds (voices,
             effects, ambience; not the music tracks) as PCM (tools/ps4_dlc/sound.py); needs jimmy and levels
  multiplayer  the two new masters' multiplayer art (lobby button, Peg Party leader); with the port's
             peggle1_dlc_mp_masters the lobby then offers them (needs jimmy and windy)
  music      Windy's and Jimmy's music: the PS4's two branches of the music tree merged into the 360's structure
             bank and their media as streamed PCM files (tools/ps4_dlc/music.py); without it Windy's levels have
             no music and Jimmy's play another master's
  select     the single-player Select a Master screen's art as the PS4 has it: the two new masters' buttons and
             costume pictures, and for the five launch masters the costume animation with six costumes (the
             360's has four, so the Garden and Spooky costumes had no picture there); needs jimmy, windy, costumes
  videos     the two new masters' intro videos WITH their audio track (the 360 archive has both files, 1920x1080,
             without one: they play silent), and the two masters' trial intro videos and subtitles it lacks
The delivered archive has all of them (one pack: costumes,jimmy,windy,levels,strings,multiplayer,select,videos,
sound,music).
The test options change the game's rules for exploring on the PC and are never for a delivered archive.
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert
import rsb as rsblib
import rton

BS = chr(92)
PACKS = ['Garden', 'Spooky']
# MASTERS.JSON entries of the two PS4 masters, in the 360 file's shape. The power classes are the PS4's names; the
# 360 executable has an early MultiBall and no PurplePegs (the port supplies both: app/src/dlc).
# The 360 file also names a background group and a sound group per master (the PS4 file dropped both keys); the 360
# executable reads them:
#   backgroundResGroup  the side art of the master's levels: built here from the PS4's animation (the PS4 keeps it
#                       in Animations) under a name in the 360's pattern
#   "<group>_Shared"    the cannon's power art and the power's own effects and sprites (on the PS4 in BOARD,
#                       Animations and Characters): `shared` lists them; what they reference comes along
#   soundResGroup       STAND-IN: the 360's own WendySounds bank for both (the PS4 banks are in another format)
# Jimmy's music is STAND-IN set A (set G's events are not in the 360 banks).
NEW_MASTERS = {
    'Fairy': dict(displayNameId='PEG_MASTER_FAIRY', power='PurplePegs', group='Masters_Wendy',
                  costumes=['Costumes_Default', 'Costumes_Yellow', 'Costumes_Aviator', 'Costumes_Pug',
                            'Costumes_Garden', 'Costumes_Spooky'],
                  music='Play_Music_Set_F',
                  background='backgrounds_fairyarea_fairyarea_sides',
                  backgroundResGroup='FairyareaBackground',
                  soundResGroup='WendySounds',
                  shared=['POPANIM_BOARD_BOARD_GUN_POWERS_BASE_PURPLEPOWER', 'POPANIM_MASTERS_WENDY_BUTTER_FLY',
                          'IMAGE_MASTERS_WENDY_ULTRAVIOLET_SCREEN', 'PIEFFECT_PI_FAIRY_SPARKLES']),
    'Jimmy': dict(displayNameId='PEG_MASTER_JIMMY', power='MultiBall', group='Masters_Jimmy',
                  costumes=['Costumes_Default', 'Costumes_Jared', 'Costumes_Clown', 'Costumes_Capture',
                            'Costumes_Garden', 'Costumes_Spooky'],
                  music='Play_Music_Set_A',                                    # STAND-IN (Play_Music_Set_G)
                  background='backgrounds_desert_desert_sides',
                  backgroundResGroup='DesertBackground',
                  soundResGroup='WendySounds',                                 # STAND-IN (his own bank)
                  # His lightning: the PS4's four images. The 360 has images of the same ids in Gnorman's group
                  # (his power's own bolt), so these get ids of their own (app/src/dlc/peggle1_dlc_jimmy.cpp).
                  shared=['PIEFFECT_PI_JIMMY_POWER', 'IMAGE_LIGHTNING1', 'IMAGE_LIGHTNING2', 'IMAGE_LIGHTNING3',
                          'IMAGE_LIGHTNING4'],
                  rename={f'IMAGE_LIGHTNING{i}': f'IMAGE_JIMMY_LIGHTNING{i}' for i in range(1, 5)}),
}

# Keys of the PS4 power data files that the port's classes do not declare as properties.
POWER_KEYS_DROPPED = {
    'MultiBall': ('m_FrenzyStyleName', 'm_FeverStyleName', 'm_SlowdownAudioEventName', 'm_cDimShade',
                  'm_fSlowdownRange', 'm_FrenzyStylePoints', 'm_FeverStylePoints'),
}


class Mod:
    def __init__(self, a):
        self.a = a
        self.x360, self.ps4 = rsblib.Rsb(a.x360), rsblib.Rsb(a.ps4)
        assert self.x360.E == '>' and self.ps4.E == '<'
        self.manifest, self.version = rton.decode(self.x360.read('__MANIFESTGROUP__', 'PROPERTIES' + BS + 'RESOURCES.RTON'))
        ps4_manifest, _ = rton.decode(self.ps4.read('__MANIFESTGROUP__', 'PROPERTIES' + BS + 'RESOURCES.RTON'))
        self.ps4_groups = {g['id'].s: g for g in ps4_manifest['groups']}
        self.have = {g['id'].s for g in self.manifest['groups']}
        # Every PS4 resource: id -> (its subgroup, its manifest entry).
        self.ps4_res = {}
        for g in ps4_manifest['groups']:
            for e in g.get('resources') or []:
                self.ps4_res[e['id'].s] = (g['id'].s, e)
        self.slots = {}
        for g in self.manifest['groups']:
            for e in g.get('resources') or []:
                self.slots[e['id'].s] = e['slot'].v
        self.first_slot = self.next_slot = self.manifest['slot_count'].v
        self.w = rsblib.Writer(self.x360)
        self.init_files = {}  # replaced plain files of Init_Common: path -> text
        self.init_raw = {}    # the same for binary files: path -> bytes
        self.group_files = {}  # files added to or replaced in other packets: group -> {path: bytes}
        self.log = []
        self.area_sound_groups = {}  # master power of a new area -> its sound group (set with --content sound)

    # ---- helpers ----
    def text(self, path):
        """A text file of Init_Common (as edited so far)."""
        if path not in self.init_files:
            self.init_files[path] = self.x360.read(self.home(path), path).decode('utf-8-sig')
        return self.init_files[path]

    def home(self, path):
        """The 360 packet a plain file is in."""
        return self.x360.rsgs[dict(self.x360.file_list)[path.upper()]]['name']

    def set_text(self, path, text):
        self.init_files[path] = text

    def add_composite(self, name):
        """A whole PS4 composite, converted. False when the 360 archive has it already or the PS4 does not."""
        if name in self.have:
            self.log.append(f'{name}: already in the 360 archive, left alone')
            return False
        if name.upper() not in self.ps4.comp_by_name:
            self.log.append(f'{name}: not in the PS4 archive, skipped')
            return False
        return self.add_converted(convert.convert_composite(self.ps4, self.ps4_groups, name, self.a.cache,
                                                            self.a.previews))

    def selection(self, ids):
        """PS4 resource ids -> the (images, plain) arguments of convert.convert_selection: the subgroup each one is
        in, plus the sprites its animations name (a PAM lists them as "file|IMAGE_ID") and the sprites of its
        particle effects (IMAGE_<effect>_<n>_<name>)."""
        images, plain, seen, todo = {}, {}, set(), list(ids)
        while todo:
            rid = todo.pop()
            if rid in seen:
                continue
            seen.add(rid)
            gname, e = self.ps4_res[rid]
            if e['type'].s == 'Image':
                images.setdefault(gname, []).append(rid)
                continue
            plain.setdefault(gname, []).append(rid)
            if e['type'].s == 'PopAnim':
                data = self.ps4.read(gname, BS.join(x.s for x in e['path']))
                found = pam_images(data)
                missing = [i for i in found if i not in self.ps4_res]
                assert not missing, (rid, missing[:3])
                todo += found
            elif e['type'].s == 'PIEffect':
                rx = re.compile('IMAGE_' + re.escape(rid[len('PIEFFECT_'):]) + r'_[0-9]+_')
                todo += [k for k in self.ps4_res if rx.match(k)]
        return images, plain

    def add_selection(self, name, ids, rename=None):
        """A new 360-style composite `name` from PS4 resources (and what they reference). rename: {PS4 id: the id
        it gets here}, for a PS4 resource whose id the 360 archive already uses for something else."""
        images, plain = self.selection(ids)
        c = convert.convert_selection(self.ps4, self.ps4_groups, name, images, plain, self.a.cache, self.a.previews)
        if rename:
            c = dict(c, groups=rton.clone(c['groups']))
            for g in c['groups']:
                for e in g.get('resources') or []:
                    if e['id'].s in rename:
                        e.set('id', convert.s(rename[e['id'].s]))
        return self.add_converted(c)

    # ---- the DLC areas ----
    def json_file(self, path, ps4=False):
        """A JSON file of Init_Common / LoadingThread as an object (key order kept)."""
        if ps4:
            for group in ('Init_Common', 'LoadingThread_Common'):
                try:
                    return json.loads(self.ps4.read(group, path).decode('utf-8-sig'))
                except KeyError:
                    pass
            raise KeyError(path)
        return json.loads(self.text(path))

    def add_files(self, group, files):
        """Plain files into an existing 360 packet, with their manifest entries (type File, as the 360's own).
        files: {path: bytes}; a path the packet has already is replaced and keeps its entry."""
        mg = next(g for g in self.manifest['groups'] if g['id'].s == group)
        have = {BS.join(x.s for x in e['path']).upper(): e for e in mg['resources']}
        template = next(e for e in mg['resources'] if e['type'].s == 'File')
        for path in files:
            if path.upper() in have:
                continue
            parts = path.split(BS)
            rid = 'RESFILE_' + re.sub('[^A-Z0-9]', '_', os.path.splitext(path.upper())[0])
            e = rton.clone(template)
            e.set('id', convert.s(rid))
            e.set('path', [convert.s(x) for x in parts])
            assert rid not in self.slots, rid
            self.slots[rid] = self.next_slot
            e.set('slot', convert.num(self.next_slot))
            self.next_slot += 1
            mg['resources'].append(e)
        self.group_files.setdefault(group, {}).update(files)

    def levels(self):
        """The two areas of the PS4's second story (Windy's and Jimmy's, ten levels and ten trials each) as the 7th
        and 8th area of the 360's one story: the 360 executable has no paging between stories (the PS4's
        "focusAction" areas), and its area list scrolls.
        Taken from the PS4 files: the areas with their stages, their progression entries, the backgrounds,
        challenges and dialogs they name, their level files and scripts, and the level backgrounds' art."""
        levels_path = 'LEVELS' + BS + 'LEVELS.JSON'
        ours, theirs = self.json_file(levels_path), self.json_file(levels_path, ps4=True)
        story = ours['stories']['STORY_NAME_ADVENTURE']
        used_levels = {s['level']['filename'].upper() for a in story['areas'].values() for s in a['stages'].values()}
        used_scripts = {s['script'].upper() for a in story['areas'].values() for s in a['stages'].values()
                        if 'script' in s} | {a['script'].upper() for a in story['areas'].values()}
        new_areas = {k: v for k, v in theirs['stories']['DLC001']['areas'].items() if 'focusAction' not in v}
        files, backgrounds, challenges, dialogs = {}, set(), set(), set()
        ps4_files = {p.upper(): (g, p) for g in self.ps4.rsgs if g['name'] == 'LoadingThread_Common'
                     for p, e in self.ps4.packet_files(g)}

        def take(path, shared):
            """The PS4's version of a level file or script, unless a stock 360 stage uses a file of that name."""
            key = path.upper()
            if key not in ps4_files:
                self.log.append(f'levels: {path} is not in the PS4 archive')
                return
            if shared:
                return  # a stock level plays this file: the 360's own version stays
            files[key] = self.ps4.read('LoadingThread_Common', ps4_files[key][1])

        # Level thumbnails: the 360 archive has Windy's under the PS4's ids already, early pictures at another size
        # (large 286x254 and small 88x78 where every other area's frames are made for 378x333 and 88x72: the user
        # saw them overflow their frames). The PS4's come in under ids of their own (<thumbnail>_PS4) and the
        # stages name those.
        thumbs, thumb_rename = [], {}
        for name, area in new_areas.items():
            area = json.loads(json.dumps(area))
            # The area's sound group (its ambience bank). With --content sound: the two groups sound.py makes for
            # the PS4's FIREFIELD.BNK and DESERT.BNK, named as the PS4's area groups are.
            # Without: STAND-IN, Bjorn's area group (the PS4's groups hold banks in the PS4's format).
            area['resGroup'] = self.area_sound_groups.get(area['masterPower'], 'pegglegrounds_common')
            take('LEVELS' + BS + 'SCRIPTS' + BS + area['script'], area['script'].upper() in used_scripts)
            for stage in area['stages'].values():
                level = stage['level']
                ps4_ids = [f'IMAGE_LEVEL_THUMBNAILS_{size}_{level["thumbnail"].upper()}' for size in ('LARGE', 'SMALL')]
                thumbs += ps4_ids
                if any(i in self.slots for i in ps4_ids) and all(i in self.ps4_res for i in ps4_ids):
                    level['thumbnail'] += '_PS4'
                    for i in ps4_ids:
                        thumb_rename[i] = i + '_PS4'
                take('LEVELS' + BS + level['filename'], level['filename'].upper() in used_levels)
                if 'script' in stage:
                    take('LEVELS' + BS + 'SCRIPTS' + BS + stage['script'], stage['script'].upper() in used_scripts)
                backgrounds.add(level['background'])
                if 'challenge' in level:
                    challenges.add(level['challenge'])
            story['areas'][name] = area
            challenges.add(area['challengesId'])
        for key, value in theirs['progression'].items():
            if key not in ours['progression'] and value.get('stage') in {s for a in new_areas.values()
                                                                        for s in a['stages']}:
                ours['progression'][key] = value
        self.set_text(levels_path, json.dumps(ours, indent=1, ensure_ascii=False))

        def merge(path, wanted, what):
            a, b = self.json_file(path), self.json_file(path, ps4=True)
            added = []
            for key in b:
                if key in wanted and a.get(key) != b[key]:
                    a[key] = b[key]
                    added.append(key)
            missing = sorted(k for k in wanted if k not in b)
            self.set_text(path, json.dumps(a, indent=1, ensure_ascii=False))
            self.log.append(f'levels: {what}: {len(added)} entries from the PS4 file' +
                            (f'; not there: {missing}' if missing else ''))
            return b

        merge('PROPERTIES' + BS + 'BACKGROUNDS.JSON', backgrounds, 'backgrounds')
        ps4_challenges = merge('PROPERTIES' + BS + 'CHALLENGES.JSON', challenges, 'challenges')
        # The dialogs the new scripts show (lines "Dialog <name>" etc.): every PS4 entry the 360 file lacks.
        d_path = 'PROPERTIES' + BS + 'DIALOGS.JSON'
        a, b = self.json_file(d_path), self.json_file(d_path, ps4=True)
        new_dialogs = [k for k in b if k not in a]
        for k in new_dialogs:
            a[k] = b[k]
        self.set_text(d_path, json.dumps(a, indent=1, ensure_ascii=False))
        self.add_files('LoadingThread', {k: v for k, v in files.items()})
        # The menus' art for the new areas: the area list's portraits and the level thumbnails (the 360 archive has
        # Windy's already, a leftover of her planned 360 release; Jimmy's come from the PS4).
        self.extend_composite('InterfaceAreaSelect', [a['selectorMasterAnim'] for a in new_areas.values()])
        self.extend_composite('LevelThumbnails', [i for i in thumbs if i in self.ps4_res], rename=thumb_rename)
        if thumb_rename:
            self.log.append(f'levels: {len(thumb_rename)} thumbnails of the PS4 in place of the 360 archive\'s early '
                            f'ones of the same ids (as ..._PS4)')
        done = 0
        for name in sorted(backgrounds):
            if self.add_composite('BG_' + name.upper()) or ('BG_' + name.upper()) in {h.upper() for h in self.have}:
                done += 1
        self.log.append(f'levels: areas {list(new_areas)} ({sum(len(a["stages"]) for a in new_areas.values())} '
                        f'stages), {len(files)} level and script files, {len(new_dialogs)} dialogs, '
                        f'{done} of {len(backgrounds)} level backgrounds')

    def extend_composite(self, composite, ids, rename=None, before=None, sizes=None):
        """PS4 resources (and what they reference) as further subgroups of an existing 360 composite: they load and
        unload with it. For art of a 360 screen that the PS4 has more of (a new area's portrait, new thumbnails):
        the 360's own packets stay as they are. before: the composite's subgroup the new ones are put in front of
        (a composite loads its subgroups in order: pictures that an animation of that subgroup names must come
        first); the default is the end."""
        rename = rename or {}
        ids = [i for i in ids if rename.get(i, i) not in self.slots]
        if not ids:
            return
        images, plain = self.selection(ids)
        for store in (images, plain):  # what the 360 composite's own subgroups have already is not taken again
            for g in list(store):
                store[g] = [i for i in store[g] if rename.get(i, i) not in self.slots]
                if not store[g]:
                    del store[g]
        # One further set of subgroups per call: <composite>_Dlc, then _Dlc2, _Dlc3 (a group name twice in the
        # manifest stops the game at start: "Resource already defined").
        name, n = composite + '_Dlc', 2
        while any(g.upper() == name.upper() or g.upper().startswith(name.upper() + '_') for g in self.have):
            name, n = f'{composite}_Dlc{n}', n + 1
        c = convert.convert_selection(self.ps4, self.ps4_groups, name, images, plain, self.a.cache, self.a.previews,
                                      sizes)
        parent = next(g for g in self.manifest['groups'] if g['id'].s.upper() == composite.upper())
        index = self.w.comp_index(parent['id'].s)
        groups = rton.clone(c['groups'][1:])
        at = None
        if before is not None:
            at = next(i for i, s in enumerate(parent['subgroups']) if s['id'].s.upper() == before.upper())
            names = [self.w.packets[s[0]]['name'].upper() for s in self.w.composites[index]['subs']]
            assert names[at] == before.upper(), (names, at, before)
        for packet, group in zip(c['packets'], groups):
            sub = self.w.add_packet(packet['name'], files=packet['files'], textures=packet['textures'])
            group.set('parent', convert.s(parent['id'].s))
            ref = [(convert.s('id'), convert.s(packet['name']))]
            if packet['res']:
                ref.append((convert.s('res'), convert.s(str(packet['res']))))
            if at is None:
                self.w.composites[index]['subs'].append((sub, packet['res'], packet['lang']))
                parent['subgroups'].append(rton.Obj(ref))
            else:
                self.w.composites[index]['subs'].insert(at, (sub, packet['res'], packet['lang']))
                parent['subgroups'].insert(at, rton.Obj(ref))
                at += 1
            for e in group.get('resources') or []:
                if rename and e['id'].s in rename:
                    e.set('id', convert.s(rename[e['id'].s]))
        self.next_slot = convert.assign_slots(groups, self.slots, self.next_slot)
        self.manifest['groups'].extend(groups)
        self.have.update(g['id'].s for g in groups)
        self.log.append(c['info'].replace(name + ' (selection)', f'{parent["id"].s} + {name}'))

    def multiplayer(self):
        """The two new masters' multiplayer art into the 360 composites that hold the five masters' own: their lobby
        buttons (Characters) and their Peg Party leader animations (MultiplayerInGameCharacters). The 360's own
        packets stay as they are."""
        # The lobby's big picture of a player's master (IMAGE_MASTERS_<M>_MULTIPLAYER_LOBBY_COSTUME<n>): the 360 draws
        # it from a 270x282 canvas, the PS4 from 668x698 (445x465 at 720): taken as it is, the new masters' pictures
        # were 1.6 times too big and reached down and right out of the player's panel (the user's console run,
        # 2026-10-14). They are scaled whole to the 360's canvas, the size of Bjorn's picture there (2.47 PS4
        # texels per texel; the launch masters' own pictures sit at 2.42 to 2.89 against the PS4's).
        ref = next(e for g in self.manifest['groups'] for e in g.get('resources') or []
                   if e['id'].s == 'IMAGE_MASTERS_BJORN_MULTIPLAYER_LOBBY_COSTUME1')
        canvas = (ref['aw'].v, ref['ah'].v)
        for composite, word in (('Characters', 'MULTIPLAYER_LOBBY'), ('MultiplayerInGameCharacters', 'MULTIPLAYER_PEGPARTY')):
            ids = sorted(i for i in self.ps4_res
                         if word in i.upper() and ('_WENDY_' in i.upper() or '_JIMMY_' in i.upper()))
            sizes = {i: canvas for i in ids if re.fullmatch(r'IMAGE_MASTERS_(WENDY|JIMMY)_MULTIPLAYER_LOBBY_COSTUME[0-9]+', i)}
            self.extend_composite(composite, ids, sizes=sizes)
            if sizes:
                self.log.append(f'multiplayer: {len(sizes)} lobby pictures of the new masters scaled to the 360 '
                                f'{canvas[0]}x{canvas[1]} canvas')

    def select(self):
        """The Select a Master screen (it opens for a level that was beaten). Its buttons and its side picture are
        animations by name: POPANIM_<master's group>_MASTER_SELECT_BUTTON and ..._MASTER_SELECT_COSTUMES (a master
        without one gets Bjorn's button and no side picture)."""
        new = sorted(i for i in self.ps4_res
                     if re.fullmatch(r'POPANIM_MASTERS_(WENDY|JIMMY)_MASTER_SELECT_(BUTTON|COSTUMES)', i))
        self.extend_composite('Characters', new)
        # The five launch masters: the PS4's costume animation shows six costumes, the 360's four. Its file takes
        # the place of the 360's (same id, same packet), and the pictures the 360 lacks come as a further subgroup.
        mg = {e['id'].s: (g['id'].s, e) for g in self.manifest['groups'] for e in g.get('resources') or []}

        def ps4_file(rid):
            """The PS4's file of an animation in the place of the 360's (same id, same packet) -> the pictures it
            names that the 360 archive lacks."""
            if rid not in mg or rid not in self.ps4_res:
                self.log.append(f'select: {rid} not in both archives, left alone')
                return None
            group, entry = mg[rid]
            pgroup, pentry = self.ps4_res[rid]
            data = self.ps4.read(pgroup, BS.join(x.s for x in pentry['path']))
            found = pam_images(data)
            missing = [i for i in found if i not in self.ps4_res]
            assert not missing, (rid, missing[:3])
            self.group_files.setdefault(group, {})[BS.join(x.s for x in entry['path'])] = bytes(data)
            return [i for i in found if i not in self.slots]

        images, replaced = [], 0
        for master in ('BJORN', 'JEFFREY', 'BERG', 'GNORMAN', 'LUNA'):
            more = ps4_file(f'POPANIM_MASTERS_{master}_MASTER_SELECT_COSTUMES')
            if more is not None:
                images += more
                replaced += 1
        # In front of the 360's animations: Characters_Common holds the replaced files, and the game looks for a
        # picture that is not loaded yet as a file (images\\720\\...\\costumes_1024x2304_.png) and stops.
        self.extend_composite('Characters', sorted(set(images)), before='Characters_Common')
        # The panel with the master's name and power beside the buttons ("MasterSelectBlades"): one animation with
        # labels by master name (bjorn_in, ..., random_in); the PS4's has fairy_* and jimmy_* and two more pictures.
        panel = ps4_file('POPANIM_FRONT_END_UI_LEVEL_MASTER_SELECT_SLIDERS_LEVEL_MASTER_SELECT_MAINFULE2_SLIDERS')
        if panel:
            self.extend_composite('InterfaceMasterSelect', sorted(set(panel)), before='InterfaceMasterSelect_Common')
        self.log.append(f'select: {len(new)} animations of the new masters, {replaced} costume animations replaced '
                        f'by the PS4\'s, {len(set(images))} pictures they name added; the name panel\'s animation '
                        f'replaced by the PS4\'s, {len(set(panel or []))} pictures added')

    def videos(self):
        """Bink videos of the PS4 archive into the 360's Videos packet (plain files, stored as they are: the game
        plays a video from its offset in the archive). The two intro videos replace the 360's silent copies."""
        files, subs = {}, {}
        for name in ('WENDY_INTRO', 'JIMMY_INTRO', 'CHALLENGE_INTRO_WENDY', 'CHALLENGE_INTRO_JIMMY'):
            data = self.ps4.read('Videos', 'VIDEO' + BS + name + '.BK2')
            if data is None:
                self.log.append(f'videos: the PS4 archive has no {name}.BK2')
                continue
            tracks = int.from_bytes(bytes(data[40:44]), 'little')
            assert bytes(data[:3]) == b'KB2' and tracks >= 1, (name, bytes(data[:4]), tracks)
            files['VIDEO' + BS + name + '.BK2'] = bytes(data)
            text = self.ps4.read('Subtitles', 'SUBTITLES' + BS + name + '.JSON')
            if text is not None:
                subs['SUBTITLES' + BS + name + '.JSON'] = bytes(text)
        self.add_files('Videos', files)
        if subs:
            self.add_files('Subtitles', subs)
        self.log.append(f'videos: {len(files)} videos with an audio track ({sum(len(v) for v in files.values())} '
                        f'bytes), {len(subs)} subtitle files')

    def unlocks(self, content):
        """UNLOCKS.JSON: the PS4 file's rules for what this build adds. The store requirements are left out (the PS4
        gates each pack with "requiredPurchase_ps4"; the content here is the user's own, all five packs), the rules
        of play stay: a level needs the one before it, the trials need the area's ten levels, a master's second to
        fourth costume need the area finished, its trials finished and 25 objectives."""
        path = 'PROPERTIES' + BS + 'UNLOCKS.JSON'
        ours, theirs = self.json_file(path), self.json_file(path, ps4=True)
        masters = json.loads(self.text('PROPERTIES' + BS + 'MASTERS.JSON'))
        levels = json.loads(self.text('LEVELS' + BS + 'LEVELS.JSON'))
        areas = levels['stories']['STORY_NAME_ADVENTURE']['areas']
        stages = {s for a in areas.values() for s in a['stages']}
        wanted = {
            'levels': lambda k: k in stages,
            'areas': lambda k: k in areas,
            'challenges': lambda k: k in {a['challengesId'] for a in areas.values()},
            'masters': lambda k: k in masters,
            # "Masters_<group>_Costumes_<name>": the costume must be in its master's list
            'costumes': lambda k: any(k.startswith(info['group'] + '_') and k[len(info['group']) + 1:] in
                                      info['costumes'] for info in masters.values()),
        }
        added = {}
        for section, keep in wanted.items():
            for key, rule in theirs.get(section, {}).items():
                if key in ours.setdefault(section, {}) or not keep(key):
                    continue
                rule = {k: v for k, v in rule.items() if not k.startswith('requiredPurchase')}
                if not any(k.startswith('required') for k in rule):
                    rule = {}  # nothing but the purchase gated it: open
                ours[section][key] = rule
                added[section] = added.get(section, 0) + 1
        # The 360 file gates what it already knew of Windy (the master, her area) with its own marketplace offer
        # ("requiredPurchase": 15): the same content, opened the same way.
        opened = []
        for section, keep in wanted.items():
            for key, rule in ours.get(section, {}).items():
                if keep(key) and any(k.startswith('requiredPurchase') for k in rule):
                    rule = {k: v for k, v in rule.items() if not k.startswith('requiredPurchase')}
                    ours[section][key] = rule if any(k.startswith('required') for k in rule) else {}
                    opened.append(key)
        self.set_text(path, json.dumps(ours, indent=1, ensure_ascii=False))
        self.log.append('unlocks: ' + ', '.join(f'{k} +{v}' for k, v in added.items()) +
                        f'; purchase requirement removed from {opened}')

    def strings(self):
        """The PS4 string tables' entries the 360's lack (the new masters, levels, style shots, trials), per
        language: added to the 360 file; a 360 entry that is empty takes the PS4's text. Nothing else changes."""
        rx = re.compile(r'[ \t]*<string id="([^"]+)">(.*?)</string>[ \t]*\r?\n', re.S)
        total = []
        for g in self.x360.rsgs:
            for path, info in self.x360.packet_files(g):
                if not path.upper().endswith('STRINGS' + BS + 'STRINGS.XML') or info['type'] != 0:
                    continue
                if g['name'].upper() not in self.ps4.by_name:
                    continue
                try:
                    theirs = self.ps4.read(g['name'], path).decode('utf-8-sig')
                except KeyError:
                    continue
                ours = self.x360.read(g['name'], path).decode('utf-8-sig')
                have = {m.group(1): m.group(2) for m in rx.finditer(ours)}
                new = [(m.group(1), m.group(2)) for m in rx.finditer(theirs)
                       if m.group(1) not in have or (not have[m.group(1)].strip() and m.group(2).strip())]
                filled = [k for k, _ in new if k in have]
                for k, v in new:
                    if k in have:
                        ours = ours.replace(f'<string id="{k}">{have[k]}</string>', f'<string id="{k}">{v}</string>', 1)
                added = ''.join(f'  <string id="{k}">{v}</string>\r\n' for k, v in new if k not in have)
                end = ours.rindex('</strings>')
                ours = ours[:end] + added + ours[end:]
                self.w.replace_files(g['name'], {path: b'\xef\xbb\xbf' + ours.encode('utf-8')})
                total.append(f'{g["name"]} +{len(new) - len(filled)} (and {len(filled)} filled)')
        self.log.append('strings: ' + ', '.join(total))

    def add_converted(self, c):
        """A converted composite (convert.convert_composite / convert_selection) into the archive and the manifest."""
        if c['name'] in self.have:
            self.log.append(f'{c["name"]}: already in the 360 archive, left alone')
            return False
        subs = []
        for p in c['packets']:
            index = self.w.add_packet(p['name'], files=p['files'], textures=p['textures'])
            subs.append((index, p['res'], p['lang']))
        self.w.add_composite(c['name'], subs)
        groups = rton.clone(c['groups'])
        self.next_slot = convert.assign_slots(groups, self.slots, self.next_slot)
        self.manifest['groups'].extend(groups)
        self.have.update(g['id'].s for g in groups)
        self.log.append(c['info'])
        return True

    # ---- content ----
    def costumes(self):
        masters = json.loads(self.text('PROPERTIES' + BS + 'MASTERS.JSON'))
        add = {}
        for master, info in masters.items():
            for c in PACKS:
                if self.add_composite(f'{info["group"]}_Costumes_{c}'):
                    add.setdefault(master, []).append(f'Costumes_{c}')
        self.set_text('PROPERTIES' + BS + 'MASTERS.JSON',
                      append_costumes(self.text('PROPERTIES' + BS + 'MASTERS.JSON'), add))

    def power_package(self, power):
        """The PS4's data file of a power class in place of the 360's (the values the PS4 game plays with). Only
        the keys the class declares are kept: the port's MultiBall (app/src/dlc/peggle1_dlc_jimmy.cpp) holds the
        PS4 file's three strings and its colour itself, and three more keys are stale on the PS4 too."""
        path = 'PACKAGES' + BS + power.upper() + '.RTON'
        data, version = rton.decode(self.ps4.read('Init_Common', path))
        obj = data['objects'][0]['objdata']
        drop = POWER_KEYS_DROPPED.get(power, ())
        missing = [k for k in drop if k not in obj]
        assert not missing, (power, missing)
        obj.items = [(k, v) for k, v in obj.items if k.s not in drop]
        self.init_raw[path] = rton.encode(data, version)
        self.log.append(f'{path}: the PS4 file ({len(obj.items)} keys{", " + str(len(drop)) + " dropped" if drop else ""})')

    def master(self, key):
        """A PS4 master: every costume composite, its shared and background groups, its power's data file and the
        MASTERS.JSON entry."""
        info = NEW_MASTERS[key]
        self.power_package(info['power'])
        for c in info['costumes']:
            self.add_composite(f'{info["group"]}_{c}')
        # The 360's level loader wants a "<group>_Shared" group per master (the cannon's power art and the power's
        # own effects; a missing one is a null entry in its load list and a crash). The PS4 keeps that art in BOARD
        # and uses the name for the master's sound bank, so the group is built from chosen PS4 resources.
        self.add_selection(info['group'] + '_Shared', info['shared'], info.get('rename'))
        # The side art of the master's levels: the 360 loads the group MASTERS.JSON names (the PS4 has the animation
        # in Animations and no such key).
        self.add_selection(info['backgroundResGroup'], ['POPANIM_' + info['background'].upper()])
        path = 'PROPERTIES' + BS + 'MASTERS.JSON'
        text = self.text(path)
        if f'"{key}"' in json.loads(text):
            return
        entry = (f',\n    "{key}":\n    {{\n        "displayNameId" : "{info["displayNameId"]}",\n'
                 f'        "power" : "{info["power"]}",\n'
                 f'\t\t"costumes"\t: [{", ".join(chr(34) + c + chr(34) for c in info["costumes"])}],\n'
                 f'\t\t"music" : "{info["music"]}",\n\t\t"group" : "{info["group"]}",\n'
                 f'\t\t"background" : "{info["background"]}",\n'
                 f'\t\t"backgroundResGroup" : "{info["backgroundResGroup"]}",\n'
                 f'\t\t"soundResGroup"\t\t: "{info["soundResGroup"]}",\n'
                 '\t\t"maxHeadTrackAngle" : 20.0,\n\t\t"minHeadTrackAngle" : -7.0,\n'
                 '\t\t"maxFaceTrackAngle" : 22.0,\n\t\t"minFaceTrackAngle" : -9.0\n    }\n')
        end = text.rstrip().rfind('}')
        body = text[:end].rstrip()
        new = body + entry + '}\n'
        json.loads(new)
        self.set_text(path, new)

    # ---- test options ----
    def test_unlock_all(self):
        path = 'PROPERTIES' + BS + 'UNLOCKS.JSON'
        data = json.loads(self.text(path))
        self.set_text(path, json.dumps({k: {} for k in data}, indent=1))
        self.log.append(f'TEST: UNLOCKS.JSON emptied ({", ".join(f"{k} {len(v)}" for k, v in data.items())})')

    def test_area_power(self, spec):
        """e.g. "Peggle Institute=MultiBall": that area's master power (and so its master) is replaced."""
        path = 'LEVELS' + BS + 'LEVELS.JSON'
        text = self.text(path)
        for item in [x for x in spec.split(',') if x]:
            area, power = item.split('=')
            i = text.index(f'"{area}"')
            m = re.compile(r'("masterPower"\s*:\s*")([^"]*)(")').search(text, i)
            text = text[:m.start(2)] + power + text[m.end(2):]
            self.log.append(f'TEST: area {area} uses the power {power} (was {m.group(2)})')
        json.loads(text)
        self.set_text(path, text)

    def test_first_costume(self, spec):
        path = 'PROPERTIES' + BS + 'MASTERS.JSON'
        text = self.text(path)
        masters = list(json.loads(text).keys())
        for item in [x for x in spec.split(',') if x]:
            master, costume = item.split('=')
            m = list(re.finditer(r'("costumes"\s*:\s*\[)([^\]]*)(\])', text))[masters.index(master)]
            names = [x.strip() for x in m.group(2).split(',')]
            want = f'"Costumes_{costume}"'
            assert want in names, (want, names)
            names.remove(want)
            text = text[:m.start(2)] + ', '.join([want] + names) + text[m.end(2):]
            json.loads(text)
            self.log.append(f'TEST: {master} wears {costume} first')
        self.set_text(path, text)

    def write(self):
        self.manifest.set('slot_count', convert.num(self.next_slot))
        self.w.replace_files('__MANIFESTGROUP__',
                             {'PROPERTIES' + BS + 'RESOURCES.RTON': rton.encode(self.manifest, self.version)})
        changes = {}
        for p, text in self.init_files.items():
            changes.setdefault(self.home(p), {})[p] = text.encode('utf-8')
        changes.setdefault('Init_Common', {}).update(self.init_raw)
        for group, files in self.group_files.items():
            changes.setdefault(group, {}).update(files)
        for group, files in changes.items():
            if files:
                self.w.replace_files(group, files)
        os.makedirs(os.path.dirname(os.path.abspath(self.a.out)), exist_ok=True)
        size = self.w.write(self.a.out)
        for line in self.log:
            print(line)
        print(f'manifest: {len(self.manifest["groups"])} groups, slots {self.first_slot} -> {self.next_slot}; wrote '
              f'{self.a.out} ({size} bytes, {len(self.w.packets)} packets, {len(self.w.composites)} composites, '
              f'{len(self.w.ptx)} textures)')


def pam_images(data):
    """The image ids a PopAnim file names: its strings "file|IMAGE_ID". A string has its 16-bit length in front; the
    length is what ends the id (the byte after it can be a letter too)."""
    out = []
    for m in re.finditer(rb'[|]IMAGE_[A-Z0-9_]+', data):
        bar, s = m.start(), m.start()
        while s > 2 and 32 <= data[s - 1] < 127 and bar - s < 200:
            s -= 1
            end = s + int.from_bytes(data[s - 2:s], 'little')
            if bar + 7 < end <= m.end():
                out.append(data[bar + 1:end].decode())
                break
    return sorted(set(out))


def append_costumes(text, add):
    """Appends costume names to the masters' "costumes" lists. add: {master key: [costume, ...]}."""
    order = list(json.loads(text).keys())
    matches = list(re.finditer(r'("costumes"\s*:\s*\[)([^\]]*)(\])', text))
    assert len(matches) == len(order), (len(matches), order)
    out, last = [], 0
    for master, m in zip(order, matches):
        out.append(text[last:m.end(2)])
        for c in add.get(master, []):
            if f'"{c}"' not in m.group(2):
                out.append(f', "{c}"')
        last = m.end(2)
    out.append(text[last:])
    new = ''.join(out)
    json.loads(new)
    return new


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--x360', required=True)
    ap.add_argument('--ps4', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--content', default='costumes')
    ap.add_argument('--cache', default=os.path.join('out', 'ps4_dlc', 'cache'))
    ap.add_argument('--previews', help='folder for PNG previews of the new atlases')
    ap.add_argument('--test-unlock-all', action='store_true', help='TEST ONLY: UNLOCKS.JSON without any requirement')
    ap.add_argument('--test-first-costume', default='', help='TEST ONLY, e.g. Bjorn=Garden: listed first')
    ap.add_argument('--test-area-power', default='', help='TEST ONLY, e.g. "Peggle Institute=MultiBall"')
    a = ap.parse_args()
    mod = Mod(a)
    content = [c for c in a.content.split(',') if c]
    if 'sound' in content:
        NEW_MASTERS['Jimmy']['soundResGroup'] = 'JimmySounds'
        mod.area_sound_groups = {'PurplePegs': 'fairyarea_common', 'MultiBall': 'desert_common'}
    if 'costumes' in content:
        mod.costumes()
    # The PS4's order: Windy ("Fairy") before Jimmy. MASTERS.JSON keeps its keys in the order they are added, and the
    # game lists the masters in that order (the multiplayer lobby's row, the Select a Master screen's second row).
    # Until 2026-10-14 Jimmy came first here; the user saw the two in each other's places in the lobby.
    if 'windy' in content:
        mod.master('Fairy')
    if 'jimmy' in content:
        mod.master('Jimmy')
    if 'levels' in content:
        mod.levels()
    if 'strings' in content:
        mod.strings()
    if 'multiplayer' in content:
        mod.multiplayer()
    if 'select' in content:
        mod.select()
    if 'videos' in content:
        mod.videos()
    if 'sound' in content:
        import sound
        sound.add(mod)
    if 'music' in content:
        import music
        music.add(mod)
    mod.unlocks(content)
    if a.test_unlock_all:
        mod.test_unlock_all()
    if a.test_first_costume:
        mod.test_first_costume(a.test_first_costume)
    if a.test_area_power:
        mod.test_area_power(a.test_area_power)
    mod.write()


if __name__ == '__main__':
    main()
