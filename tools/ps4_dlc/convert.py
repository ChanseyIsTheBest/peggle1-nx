"""PS4 resource groups as Xbox 360 content: the 1080 atlases cut into sprites, scaled to 720 and packed again, the
plain files carried over, the manifest entries rewritten. Used by build_mod.py; results are cached on disk because the
texture work is the slow part.
"""
import hashlib
import os
import pickle

import numpy as np
from PIL import Image

import ptx
import rton
from rton import Num, Obj, Str

SCALE = 2.0 / 3.0  # 1080 art -> 720 art
GAP = 2            # transparent pixels between two sprites of an atlas
MAX_SIDE = 4096
VERSION = 5        # bump to drop the cache


def num(v):
    """An integer the way the manifest stores it."""
    return Num(0x21, 0) if v == 0 else Num(0x24, v) if v > 0 else Num(0x25, v)


def s(text):
    return Str(0x90, text)


class Skyline:
    """Bottom-left skyline packing into one bin."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.line = [(0, 0, w)]  # (x, y, width) segments, left to right

    def _fit(self, i, w, h):
        x = self.line[i][0]
        if x + w > self.w:
            return None
        left, y, k = w, 0, i
        while left > 0:
            if k >= len(self.line):
                return None
            y = max(y, self.line[k][1])
            if y + h > self.h:
                return None
            left -= self.line[k][2]
            k += 1
        return y

    def add(self, w, h):
        best = None
        for i in range(len(self.line)):
            y = self._fit(i, w, h)
            if y is not None and (best is None or (y + h, self.line[i][2]) < (best[0], best[1])):
                best = (y + h, self.line[i][2], i, y)
        if best is None:
            return None
        _, _, i, y = best
        x = self.line[i][0]
        new = (x, y + h, w)
        # Cut the segments the rectangle covers.
        end = x + w
        k = i
        rest = []
        while k < len(self.line) and self.line[k][0] < end:
            sx, sy, sw = self.line[k]
            if sx + sw > end:
                rest.append((end, sy, sx + sw - end))
            k += 1
        self.line[i:k] = [new] + rest
        # Merge neighbours of the same height.
        j = 0
        while j + 1 < len(self.line):
            a, b = self.line[j], self.line[j + 1]
            if a[1] == b[1]:
                self.line[j:j + 2] = [(a[0], a[1], a[2] + b[2])]
            else:
                j += 1
        return x, y


def pack(sizes):
    """[(w, h)] -> [(atlas index, x, y)], [(atlas width, atlas height)]: as few atlases as possible, each the
    smallest power-of-two rectangle that holds its sprites."""
    order = sorted(range(len(sizes)), key=lambda i: (-max(sizes[i]), -sizes[i][0] * sizes[i][1]))
    # Power-of-two rectangles no more than twice as long as wide, the smallest first (the 360 atlases' shapes).
    shapes = sorted({(1 << a, 1 << b) for a in range(6, 13) for b in range(6, 13)
                     if abs(a - b) <= 1 and max(1 << a, 1 << b) <= MAX_SIDE}, key=lambda t: (t[0] * t[1], t[0]))

    def fill(shape, items):
        """Places as many of `items` (in order) as fit: ({index: (x, y)}, the rest)."""
        sky = Skyline(*shape)
        pos, rest = {}, []
        for i in items:
            q = sky.add(sizes[i][0] + GAP, sizes[i][1] + GAP)
            if q is None:
                rest.append(i)
            else:
                pos[i] = q
        return pos, rest

    def solve(items, depth):
        """The atlases with the least total area for `items`: [(shape, {index: (x, y)})]."""
        single = None
        for shape in shapes:
            if shape[0] * shape[1] < sum((sizes[i][0] + GAP) * (sizes[i][1] + GAP) for i in items):
                continue
            pos, rest = fill(shape, items)
            if not rest:
                single = [(shape, pos)]
                break
        best = single
        best_area = single[0][0][0] * single[0][0][1] if single else None
        if depth < 3:
            # One atlas filled and the rest in further ones, when that takes less area than the single atlas.
            for shape in reversed(shapes):
                area = shape[0] * shape[1]
                if best_area is not None and area >= best_area:
                    continue
                if area < 1024 * 1024 and best is not None:
                    break
                pos, rest = fill(shape, items)
                if not pos or not rest:
                    continue
                more = solve(rest, depth + 1)
                if more is None:
                    continue
                total = area + sum(sh[0] * sh[1] for sh, _ in more)
                if best_area is None or total < best_area:
                    best, best_area = [(shape, pos)] + more, total
        return best

    result = solve(order, 0)
    assert result, 'a sprite is larger than the largest atlas'
    placed = [None] * len(sizes)
    atlases = []
    for shape, pos in result:
        for i, (x, y) in pos.items():
            placed[i] = (len(atlases), x + GAP // 2, y + GAP // 2)
        atlases.append(shape)
    return placed, atlases


def _rename_res(parts):
    return [s('720') if p.s == '1080' else rton.clone(p) for p in parts]


def read_atlases(ps4, packet):
    """The decoded atlases of a PS4 texture subgroup: {NAME (the file name, upper case): RGBA array}."""
    part1 = ps4.part(packet, 1)
    atlases = {}
    for path, e in ps4.packet_files(packet):
        if e['type'] == 1:
            key = os.path.splitext(path.split('\\')[-1])[0].upper()
            atlases[key] = ptx.decode_bc3(ptx.ps4_untile(part1[e['offset']:e['offset'] + e['size']], e['w'], e['h']))
    return atlases


def convert_textures(ps4, packet, group, new_name, previews=None):
    """A PS4 1080 atlas subgroup -> (textures [(path, bytes, w, h)], manifest resources) for the 720 subgroup
    `new_name` (e.g. Masters_Bjorn_Costumes_Garden_720)."""
    return pack_sprites(group['resources'], read_atlases(ps4, packet), new_name, previews)


def pack_sprites(resources, atlases, new_name, previews=None, sizes=None):
    """Manifest entries of PS4 sprites (and their atlases' entries, used for the time stamp only) with the decoded
    atlases they are cut from -> (textures, manifest resources, info) of the 720 subgroup `new_name`.
    sizes: {sprite id: (w, h)}: the whole picture scaled to that size instead of by SCALE (a picture whose canvas
    the 360 draws at another size than the PS4)."""
    sprites, atlas_entries, others = [], [], []
    for e in resources:
        if e.get('atlas') is True:
            atlas_entries.append(e)
        elif 'parent' in e:
            src = atlases[e['parent'].s[len('ATLASIMAGE_ATLAS_'):].upper()]
            ax, ay, aw, ah = e['ax'].v, e['ay'].v, e['aw'].v, e['ah'].v
            crop = src[ay:ay + ah, ax:ax + aw]
            # Sprite sheets (cols / rows) keep cell sizes whole: each cell is scaled, so the size is a multiple.
            cols = e['cols'].v if 'cols' in e else 1
            rows = e['rows'].v if 'rows' in e else 1
            w = max(1, round(aw / cols * SCALE)) * cols
            h = max(1, round(ah / rows * SCALE)) * rows
            if sizes and e['id'].s in sizes:
                w, h = sizes[e['id'].s]
            sprites.append((e, ptx.resize_rgba(crop, w, h) if crop.size else np.zeros((h, w, 4), np.uint8)))
        else:
            others.append(e)
    assert not others, [o['id'].s for o in others][:3]
    placed, shapes = pack([(im.shape[1], im.shape[0]) for _, im in sprites])
    canvases = [np.zeros((ah, aw, 4), np.uint8) for aw, ah in shapes]
    for (e, im), (a, x, y) in zip(sprites, placed):
        h, w = im.shape[:2]
        c = canvases[a]
        # The sprite with its border pixels repeated once around it (bilinear sampling at its edge).
        padded = np.pad(im, ((1, 1), (1, 1), (0, 0)), mode='edge')
        y0, x0 = max(y - 1, 0), max(x - 1, 0)
        y1, x1 = min(y + h + 1, c.shape[0]), min(x + w + 1, c.shape[1])
        c[y0:y1, x0:x1] = padded[y0 - (y - 1):y1 - (y - 1), x0 - (x - 1):x1 - (x - 1)]
    textures, res = [], []
    time = rton.clone(atlas_entries[0]['time']) if atlas_entries and 'time' in atlas_entries[0] else s('0')
    ids = []
    for a, (aw, ah) in enumerate(shapes):
        tex_name = f'{new_name}_{a:02d}'
        atlas_id = f'ATLASIMAGE_ATLAS_{tex_name.upper()}'
        ids.append(atlas_id)
        textures.append((f'ATLASES\\{tex_name.upper()}.PTX', ptx.encode_bc3(canvases[a]), aw, ah))
        res.append(Obj([(s('type'), s('Image')), (s('slot'), None), (s('id'), s(atlas_id)),
                        (s('path'), [s('atlases'), s(tex_name)]), (s('atlas'), True), (s('runtime'), True),
                        (s('width'), num(aw)), (s('height'), num(ah)), (s('time'), rton.clone(time))]))
        if previews:
            os.makedirs(previews, exist_ok=True)
            Image.fromarray(canvases[a], 'RGBA').save(os.path.join(previews, tex_name + '.png'))
    for (e, im), (a, x, y) in zip(sprites, placed):
        n = rton.clone(e)
        n.set('slot', None)
        n.set('path', _rename_res(n['path']))
        n.set('parent', s(ids[a]))
        n.set('ax', num(x))
        n.set('ay', num(y))
        n.set('aw', num(im.shape[1]))
        n.set('ah', num(im.shape[0]))
        for k in ('x', 'y'):
            if k in n:
                n.set(k, num(round(n[k].v * SCALE)))
        res.append(n)
    info = (f'{len(sprites)} sprites, {"+".join(f"{v.shape[1]}x{v.shape[0]}" for v in atlases.values())} -> '
            f'{"+".join(f"{w}x{h}" for w, h in shapes)}')
    return textures, res, info


def convert_plain(ps4, packet, group, only=None):
    """A PS4 subgroup of plain files -> (files [(path, bytes)], manifest resources), the resolution folder renamed.
    only: a set of resource ids; the rest is left out."""
    part0 = ps4.part(packet, 0)
    wanted = None
    if only is not None:
        wanted = {'\\'.join(x.s for x in e['path']).upper() for e in group['resources'] if e['id'].s in only}
    files = []
    for path, e in sorted(ps4.packet_files(packet), key=lambda x: x[1]['offset']):
        if wanted is not None and path.upper() not in wanted:
            continue
        assert e['type'] == 0, path
        files.append((path.replace('\\1080\\', '\\720\\'), part0[e['offset']:e['offset'] + e['size']]))
    res = []
    for e in group['resources']:
        if only is not None and e['id'].s not in only:
            continue
        n = rton.clone(e)
        n.set('slot', None)
        n.set('path', _rename_res(n['path']))
        res.append(n)
    return files, res


def convert_composite(ps4, ps4_groups, name, cache_dir=None, previews=None, skip_files=None):
    """A whole PS4 composite as 360 content.
    Returns dict(name, packets=[dict(name, res, lang, textures, files)], groups=[manifest groups], info).
    skip_files(path) -> True leaves a plain file out (PS4-only formats)."""
    comp = ps4.comp_by_name[name.upper()]
    key = None
    if cache_dir:
        h = hashlib.sha1(repr((VERSION, comp['name'], [(ps4.rsgs[i]['name'], ps4.rsgs[i]['length'], r, l)
                                                        for i, r, l in comp['subs']], SCALE, GAP)).encode())
        key = os.path.join(cache_dir, f'{comp["name"]}_{h.hexdigest()[:12]}.pkl')
        if os.path.exists(key) and not skip_files:
            return pickle.load(open(key, 'rb'))
    packets, groups, infos, subrefs = [], [], [], []
    for idx, res, lang in comp['subs']:
        g = ps4.rsgs[idx]
        mg = ps4_groups[g['name']]
        if res == 1080:
            new_name = g['name'].replace('_1080', '_720')
            textures, resources, info = convert_textures(ps4, g, mg, new_name, previews)
            packets.append(dict(name=new_name, res=720, lang=lang, textures=textures, files=[]))
            infos.append(info)
        else:
            assert res == 0, (g['name'], res)
            new_name = g['name']
            files, resources = convert_plain(ps4, g, mg)
            if skip_files:
                keep = [not skip_files(p) for p, _ in files]
                paths = {p.upper() for (p, _), k in zip(files, keep) if not k}
                files = [f for f, k in zip(files, keep) if k]
                resources = [r for r in resources
                             if '\\'.join(x.s for x in r['path']).upper() not in paths]
            packets.append(dict(name=new_name, res=0, lang=lang, textures=[], files=files))
            infos.append(f'{len(files)} file(s), {sum(len(d) for _, d in files)} bytes')
        items = [(s('type'), s('simple')), (s('id'), s(new_name))]
        ref = [(s('id'), s(new_name))]
        if res == 1080:
            items.append((s('res'), s('720')))
            ref.append((s('res'), s('720')))
        if 'loc' in mg:
            items.append((s('loc'), rton.clone(mg['loc'])))
            ref.append((s('loc'), rton.clone(mg['loc'])))
        items += [(s('parent'), s(comp['name'])), (s('resources'), resources)]
        groups.append(Obj(items))
        subrefs.append(Obj(ref))
    groups.insert(0, Obj([(s('type'), s('composite')), (s('id'), s(comp['name'])), (s('subgroups'), subrefs)]))
    out = dict(name=comp['name'], packets=packets, groups=groups, info=f'{comp["name"]}: ' + '; '.join(infos))
    if key and not skip_files:
        os.makedirs(cache_dir, exist_ok=True)
        pickle.dump(out, open(key, 'wb'), protocol=4)
    return out


def convert_selection(ps4, ps4_groups, name, images, plain, cache_dir=None, previews=None, sizes=None):
    """A new 360-style composite `name` (subgroups name_720 and name_Common) from chosen resources of PS4 groups, for
    content the two releases group differently (the 360 keeps a master's cannon art in Masters_X_Shared, the PS4 in
    BOARD). images: {PS4 atlas subgroup: [sprite id, ...]}; plain: {PS4 plain subgroup: [resource id, ...]}.
    Returns the same dict as convert_composite."""
    key = None
    if cache_dir:
        h = hashlib.sha1(repr((VERSION, name, sorted((g, sorted(v)) for g, v in images.items()),
                               sorted((g, sorted(v)) for g, v in plain.items()),
                               [ps4.by_name[g.upper()]['length'] for g in list(images) + list(plain)],
                               SCALE, GAP, sorted((sizes or {}).items()))).encode())
        key = os.path.join(cache_dir, f'{name}_sel_{h.hexdigest()[:12]}.pkl')
        if os.path.exists(key):
            return pickle.load(open(key, 'rb'))
    packets, groups, infos, subrefs = [], [], [], []
    if images:
        # Every chosen sprite of every source subgroup into one set of atlases.
        new_name = name + '_720'
        chosen, sheets = [], {}
        for gname, ids in images.items():
            mg = ps4_groups[gname]
            sheets.update(read_atlases(ps4, ps4.by_name[gname.upper()]))
            found = [e for e in mg['resources'] if e['id'].s in set(ids) and 'parent' in e]
            missing = set(ids) - {e['id'].s for e in found}
            assert not missing, (gname, sorted(missing))
            chosen += found
        textures, resources, info = pack_sprites(chosen, sheets, new_name, previews, sizes)
        packets.append(dict(name=new_name, res=720, lang='', textures=textures, files=[]))
        groups.append(Obj([(s('type'), s('simple')), (s('id'), s(new_name)), (s('res'), s('720')),
                           (s('parent'), s(name)), (s('resources'), resources)]))
        subrefs.append(Obj([(s('id'), s(new_name)), (s('res'), s('720'))]))
        infos.append(info)
    if plain:
        new_name = name + '_Common'
        files, resources = [], []
        for gname, ids in plain.items():
            f, r = convert_plain(ps4, ps4.by_name[gname.upper()], ps4_groups[gname], set(ids))
            missing = set(ids) - {e['id'].s for e in r}
            assert not missing, (gname, sorted(missing))
            files += f
            resources += r
        packets.append(dict(name=new_name, res=0, lang='', textures=[], files=files))
        groups.append(Obj([(s('type'), s('simple')), (s('id'), s(new_name)), (s('parent'), s(name)),
                           (s('resources'), resources)]))
        subrefs.append(Obj([(s('id'), s(new_name))]))
        infos.append(f'{len(files)} file(s), {sum(len(d) for _, d in files)} bytes')
    groups.insert(0, Obj([(s('type'), s('composite')), (s('id'), s(name)), (s('subgroups'), subrefs)]))
    out = dict(name=name, packets=packets, groups=groups, info=f'{name} (selection): ' + '; '.join(infos))
    if key:
        os.makedirs(cache_dir, exist_ok=True)
        pickle.dump(out, open(key, 'wb'), protocol=4)
    return out


def assign_slots(groups, slots, next_slot):
    """Gives every resource of the new groups its slot: the one its id already has, else the next free one."""
    for g in groups:
        for e in g.get('resources') or []:
            rid = e['id'].s
            if rid not in slots:
                slots[rid] = next_slot
                next_slot += 1
            e.set('slot', num(slots[rid]))
    return next_slot
