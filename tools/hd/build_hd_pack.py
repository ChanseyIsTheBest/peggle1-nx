"""Builds the HD atlas pack (peggle1_hd.pack) for the port's renderer from the PS4 release's 1080 art.

The Xbox 360 game only knows its 720 groups (the group names are in its executable), so the 1080 art cannot be loaded
by the game itself. Instead the renderer swaps a texture for a larger one (app/src/me/peggle1_hd_pack.h): when the
bytes of a texture the game hands to the GPU have a fingerprint (XXH3-64) listed in the pack, it uploads the pack's
image, 1.5 times the size in each axis with the SAME layout, so the game's texture coordinates stay right.

For every 720 atlas of the archive (the stock one or the modded one with the PS4 content, tools/ps4_dlc/build_mod.py):
  - the base of the HD image is the 720 atlas scaled 1.5 times (Lanczos, on colours multiplied by alpha);
  - every sprite that has a PS4 sprite of the same id is replaced by that one, its whole rectangle resampled onto the
    1.5 times rectangle of the 720 sprite, with a ring of its edge around it as the 720 atlases have;
  - the two releases number the pieces of an animation that have the same size differently (BJORN_121X231_3 here is
    BJORN_121X230_3 there), so a PS4 sprite whose picture, scaled down, does not look like the 720 sprite is replaced
    by the PS4 sprite of the same size in the same group that does;
  - they also store some pieces at other sizes or on other canvases, under other ids (tools/hd/matching.py): a PS4
    sprite of the id whose picture lies elsewhere on its canvas is cut to the 720 sprite's canvas by content, and a
    sprite with no PS4 sprite of its id (or one of another picture) gets the PS4 sprite of the group that shows the
    same picture, whatever its size; a sprite with no such match is left alone (the 720 picture enlarged);
  - transparent texels get the colour of the nearest opaque ones (the GPU's filter mixes colours before alpha);
  - the result is encoded as BC3 again (tools/hd/bc3.py) and stored in LZ4 blocks of 256 KiB.

  python -P -E tools/hd/build_hd_pack.py --x360 <main.rsb> --ps4 <ps4 main.rsb> --out <folder>
      [--groups NAME,NAME...] [--jobs N] [--stats] [--previews <folder>] [--min-psnr 20] [--sure-psnr 30]

Nothing of the game is written outside --out / --previews (keep both under out/).
"""
import argparse
import multiprocessing
import os
import struct
import sys
import time

import lz4.block
import numpy as np
import xxhash
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ps4_dlc'))
import bc3  # noqa: E402
import matching  # noqa: E402
import ptx  # noqa: E402
import rsb as rsblib  # noqa: E402
import rton  # noqa: E402

BS = chr(92)
MORTON = (0, 3, 1, 4, 2, 5)  # the PS4 tile order (x0 y0 x1 y1 x2 y2): passed explicitly, ptx.ps4_order() can guess wrong
CHUNK = 256 * 1024
RING = 1.5                   # HD texels of edge extrusion around a sprite (the 720 atlases have 1)
KNOWN = 8                    # alpha (of 255) from which a resampled texel's colour is taken as its own
MAGIC, VERSION = 0x44483250, 1  # 'P2HD'
COUNTS = ('replaced', 'other_name', 'recut', 'by_picture', 'kept_under', 'no_twin', 'mismatch', 'bad_twin')
PREFIX = 'ATLASIMAGE_ATLAS_'

_X = _P = None


def manifest(a):
    m, _ = rton.decode(a.read('__MANIFESTGROUP__', 'PROPERTIES' + BS + 'RESOURCES.RTON'))
    return m


def atlas_name(parent):
    s = parent.upper()
    return s[len(PREFIX):] if s.startswith(PREFIX) else s


def make_jobs(x, p, only):
    """One job per 720 group: its sprites with their PS4 twins, as plain tuples."""
    mx, mp = manifest(x), manifest(p)
    twins, by_group = {}, {}
    for g in mp['groups']:
        res = g.get('res')
        if res is None or res.s != '1080':
            continue
        for e in g.get('resources') or []:
            if 'parent' in e:
                t = (g['id'].s, atlas_name(e['parent'].s), e['ax'].v, e['ay'].v, e['aw'].v, e['ah'].v)
                twins.setdefault(e['id'].s, []).append(t)
                by_group.setdefault(g['id'].s, []).append(t[1:])
    upper = {g.upper(): g for g in by_group}
    jobs = []
    for g in mx['groups']:
        res = g.get('res')
        name = g['id'].s
        if res is None or res.s != '720' or name.upper() not in x.by_name:
            continue
        if only and name.upper() not in only:
            continue
        sprites = {}
        for e in g.get('resources') or []:
            if 'parent' not in e:
                continue
            cands = twins.get(e['id'].s) or []
            want = name.upper().replace('_720', '_1080')
            twin = next((c for c in cands if c[0].upper() == want), cands[0] if cands else None)
            sprites.setdefault(atlas_name(e['parent'].s), []).append(
                (e['id'].s, e['ax'].v, e['ay'].v, e['aw'].v, e['ah'].v, twin))
        if x.by_name[name.upper()]['ptx_count']:
            used = {s[5][0] for group in sprites.values() for s in group if s[5]}
            own = upper.get(name.upper().replace('_720', '_1080'))
            if own:
                used.add(own)
            jobs.append((name, sprites, {g: by_group[g] for g in used}, own))
    return jobs


def textures_of(arch, packet):
    pk = arch.by_name[packet.upper()]
    part = None
    out = {}
    for path, e in arch.packet_files(pk):
        if e['type'] == 1:
            if part is None:
                part = arch.part(pk, 1)
            out[os.path.splitext(path.split(BS)[-1])[0].upper()] = (part[e['offset']:e['offset'] + e['size']], e['w'], e['h'])
    return out


def premul(rgba):
    """(h, w, 4) uint8 -> float32 with the colours multiplied by alpha (0..255)."""
    f = rgba.astype(np.float32)
    f[:, :, :3] *= f[:, :, 3:4] * (1.0 / 255.0)
    return f


def resize(pm, size, box=None):
    """Premultiplied float (h, w, 4) -> (size[1], size[0], 4), Lanczos. Premultiplied because a texel's colour only
    counts as far as it is opaque: the atlases repeat a sprite's colours one texel out under its transparent edge,
    but further out the colours are whatever the encoder left, and resampled on their own they came back as
    coloured fringes."""
    return np.stack([np.asarray(Image.fromarray(pm[:, :, c], 'F').resize(size, Image.LANCZOS, box=box))
                     for c in range(4)], axis=2)


def unpremul(pm):
    """Premultiplied float -> uint8 RGBA. Where alpha is under KNOWN the colour is left 0: fill_colours writes it."""
    a = np.clip(pm[:, :, 3], 0.0, 255.0)
    rgb = pm[:, :, :3] * (255.0 / np.maximum(a, 1.0))[:, :, None]
    out = np.empty(pm.shape, dtype=np.uint8)
    out[:, :, :3] = np.where((a >= KNOWN)[:, :, None], np.clip(rgb + 0.5, 0.0, 255.0), 0.0)
    out[:, :, 3] = a + 0.5
    return out


def fill_colours(rgba):
    """The colours of the texels with alpha under KNOWN, taken from the nearest texels that have one (pull-push over
    halved copies), in place. The GPU's filter mixes the colours of neighbouring texels before alpha is applied, so
    a transparent texel beside an edge must carry the edge's colour; the block encoder also picks better endpoints
    when a block's transparent texels do not hold another colour."""
    known = rgba[:, :, 3] >= KNOWN
    if known.all() or not known.any():
        return
    levels = []
    c = (rgba[:, :, :3] * known[:, :, None]).astype(np.float32)
    w = known.astype(np.float32)
    while c.shape[0] > 1 or c.shape[1] > 1:
        hh, ww = (c.shape[0] + 1) // 2, (c.shape[1] + 1) // 2
        cp = np.zeros((hh * 2, ww * 2, 3), dtype=np.float32)
        cp[:c.shape[0], :c.shape[1]] = c
        wp = np.zeros((hh * 2, ww * 2), dtype=np.float32)
        wp[:w.shape[0], :w.shape[1]] = w
        c, w = cp.reshape(hh, 2, ww, 2, 3).sum(axis=(1, 3)), wp.reshape(hh, 2, ww, 2).sum(axis=(1, 3))
        levels.append((c, w))
    col = levels[-1][0] / np.maximum(levels[-1][1], 1e-6)[:, :, None]
    for c, w in reversed(levels[:-1]):
        up = np.repeat(np.repeat(col, 2, axis=0), 2, axis=1)[:c.shape[0], :c.shape[1]]
        col = np.where((w > 0)[:, :, None], c / np.maximum(w, 1e-6)[:, :, None], up)
    up = np.repeat(np.repeat(np.clip(col + 0.5, 0.0, 255.0).astype(np.uint8), 2, axis=0), 2, axis=1)
    rgba[:, :, :3] = np.where(known[:, :, None], rgba[:, :, :3], up[:rgba.shape[0], :rgba.shape[1]])


def psnr(a, b):
    """Of two premultiplied images (0..255)."""
    mse = float(((a - b) ** 2).mean()) / (255.0 * 255.0)
    return 99.0 if mse <= 1e-10 else 10.0 * np.log10(1.0 / mse)


def place(src, ax, ay, aw, ah, hw, hh):
    """The PS4 sprite `src` (premultiplied) resampled for the HD atlas: its whole rectangle onto [1.5 ax,
    1.5 (ax + aw)), with RING texels of its edge around it. Returns (patch as RGBA, x0, y0, interior rectangle)."""
    bh, bw = src.shape[:2]
    tx0, ty0, tx1, ty1 = 1.5 * ax, 1.5 * ay, 1.5 * (ax + aw), 1.5 * (ay + ah)
    x0, y0 = max(int(np.floor(tx0 - RING)), 0), max(int(np.floor(ty0 - RING)), 0)
    x1, y1 = min(int(np.ceil(tx1 + RING)), hw), min(int(np.ceil(ty1 + RING)), hh)
    sx, sy = bw / (tx1 - tx0), bh / (ty1 - ty0)  # source texels per HD texel (about 1)
    pad = int(np.ceil(RING * max(sx, sy))) + 4
    padded = np.pad(src, ((pad, pad), (pad, pad), (0, 0)), mode='edge')
    box = ((x0 - tx0) * sx + pad, (y0 - ty0) * sy + pad, (x1 - tx0) * sx + pad, (y1 - ty0) * sy + pad)
    if sx == 1.0 and sy == 1.0 and all(float(v).is_integer() for v in box):
        bx, by = int(box[0]), int(box[1])
        patch = unpremul(padded[by:by + (y1 - y0), bx:bx + (x1 - x0)])  # texel for texel
    else:
        patch = unpremul(resize(padded, (x1 - x0, y1 - y0), box))
    inner = (max(int(np.floor(tx0)), 0), max(int(np.floor(ty0)), 0), min(int(np.ceil(tx1)), hw), min(int(np.ceil(ty1)), hh))
    return patch, x0, y0, inner


def pair_preview(folder, tag, small, src):
    """<folder>/<tag>.png: the 720 sprite enlarged 1.5 times beside the PS4 picture at the same size, on grey."""
    ah, aw = small.shape[:2]
    wv, hv = (3 * aw + 1) // 2, (3 * ah + 1) // 2
    pad = 4
    sheet = np.full((hv + 2 * pad, wv * 2 + 3 * pad, 3), 96, dtype=np.uint8)
    for x0, pm in ((pad, resize(small, (wv, hv))), (wv + 2 * pad, resize(src, (wv, hv)))):
        im = unpremul(pm)
        a = im[:, :, 3:4].astype(np.float32) / 255.0
        region = sheet[pad:pad + hv, x0:x0 + wv]
        region[:] = (im[:, :, :3] * a + region * (1 - a)).astype(np.uint8)
    k = 3 if wv < 200 else 2 if wv < 400 else 1
    os.makedirs(folder, exist_ok=True)
    Image.fromarray(sheet, 'RGB').resize((sheet.shape[1] * k, sheet.shape[0] * k), Image.NEAREST).save(
        os.path.join(folder, tag[:150] + '.png'))


def init(x_path, p_path):
    global _X, _P
    _X, _P = rsblib.Rsb(x_path), rsblib.Rsb(p_path)


def build_group(args):
    (name, sprites, alts, own), opt = args
    started = time.time()
    out, stats, rows = [], dict.fromkeys(COUNTS, 0), []
    ps4_tex, ps4_img, boxes = {}, {}, {}

    def ps4_atlas(group, atlas):
        if group not in ps4_tex:
            ps4_tex[group] = textures_of(_P, group) if group.upper() in _P.by_name else {}
        if (group, atlas) not in ps4_img:
            t = ps4_tex[group].get(atlas)
            ps4_img[(group, atlas)] = None if t is None else ptx.decode_bc3(ptx.ps4_untile(t[0], t[1], t[2], MORTON))
        return ps4_img[(group, atlas)]

    def find_by_picture(group, small, skip):
        """matching.by_picture among the PS4 sprites of `group`."""
        def crop(cand):
            oa, ox, oy, ow, oh = cand
            img = ps4_atlas(group, oa)
            if img is None or oy + oh > img.shape[0] or ox + ow > img.shape[1]:
                return None
            return img[oy:oy + oh, ox:ox + ow]

        def cand_box(cand):
            if (group, cand) not in boxes:
                c = crop(cand)
                # the alpha channel alone: premultiplying leaves it as it is
                boxes[(group, cand)] = None if c is None else matching.content_box(c.astype(np.float32))
            return boxes[(group, cand)]

        def cand_picture(cand):
            c = crop(cand)
            return None if c is None else premul(c)
        return matching.by_picture(small, alts.get(group, ()), cand_box, cand_picture, resize, psnr, skip)

    for atlas, (raw, w, h) in sorted(textures_of(_X, name).items()):
        size = (w // 4) * (h // 4) * 16
        if w % 8 or h % 8 or len(raw) < size:
            rows.append(f'{name}/{atlas}: skipped ({w}x{h}, {len(raw)} bytes)')
            continue
        raw = bytes(raw[:size])
        key = xxhash.xxh3_64_intdigest(raw)
        rgba = ptx.decode_bc3(ptx.x360_blocks(raw, w, h))
        base = premul(rgba)
        hw, hh = w * 3 // 2, h * 3 // 2
        patches, n, recipe = [], dict.fromkeys(COUNTS, 0), []
        for sid, ax, ay, aw, ah, twin in sprites.get(atlas, []):
            if not aw or not ah or ax + aw > w or ay + ah > h:
                continue
            small = base[ay:ay + ah, ax:ax + aw]
            src, q, note, pg, bw, bh, skip = None, -1.0, '', own, 0, 0, None
            # for the recipe (tools/hd/make_recipe.py): the PS4 sprite src is made of (group, atlas, x, y, w, h), the
            # rectangle of it recut() cut (None: the sprite as it is) and whether the 720 sprite is kept under it
            whence, rect, under = None, None, False
            if twin is not None:
                pg, pa, bx, by, bw, bh = twin
                img = ps4_atlas(pg, pa)
                if img is None or not bw or not bh or by + bh > img.shape[0] or bx + bw > img.shape[1]:
                    n['bad_twin'] += 1
                    continue
                skip = (pa, bx, by, bw, bh)
                src = premul(img[by:by + bh, bx:bx + bw])
                whence = twin
                q = psnr(resize(src, (aw, ah)), small)
                if q < opt['sure_psnr'] and min(aw, ah) >= 6:
                    # Not clearly the same picture: the PS4 sprites of the same size in that group, in case the
                    # numbering of same-sized pieces differs between the two releases.
                    for oa, ox, oy, ow, oh in alts.get(pg, ()):
                        if abs(ow - bw) > 2 or abs(oh - bh) > 2 or (oa, ox, oy) == (pa, bx, by):
                            continue
                        other = ps4_atlas(pg, oa)
                        if other is None or oy + oh > other.shape[0] or ox + ow > other.shape[1]:
                            continue
                        cand = premul(other[oy:oy + oh, ox:ox + ow])
                        oq = psnr(resize(cand, (aw, ah)), small)
                        if oq > q + 3.0:
                            q, src, note = oq, cand, f' (another sprite of the size, at {ox},{oy})'
                            whence = (pg, oa, ox, oy, ow, oh)
                    if note:
                        n['other_name'] += 1
                    elif abs(bw - 1.5 * aw) > 2 or abs(bh - 1.5 * ah) > 2:
                        # A canvas of another size (a master's lobby picture is 270x282 here and 668x698 there):
                        # the same picture at another place of it, cut to this sprite's canvas by content. Not for
                        # a sprite of 1.5 times the size: that is the same cut, and laying it by content made the
                        # bricks 3 per cent narrower than the 720 ones (their glow moves the content box).
                        again = matching.refit(small, src, resize, psnr)
                        if again is not None and again[0] > q + 2.0:
                            q, src, rect = again[0], again[1], again[3]
                            note = f' (cut to the canvas by content, {again[2]:.3f} texels per texel)'
                            n['recut'] += 1
                            if opt['match_previews']:
                                pair_preview(opt['match_previews'], f'cut_q{q:04.1f}_{name[:28]}_{sid[-40:]}', small, src)
            found = None
            if (src is None or q < opt['min_psnr']) and pg and min(aw, ah) >= matching.MIN_SIDE:
                # No PS4 sprite of the id, or one of another picture: the group's sprite that shows this picture.
                found, near = find_by_picture(pg, small, skip)
                if found is not None:
                    q, lp, cd, src, cand, k, rect = found
                    bw, bh = cand[3], cand[4]
                    whence = (pg,) + tuple(cand)
                    note = (f' (by picture: low-pass {lp:.1f}, colour {cd:.1f}, {k:.3f} texels per texel, '
                            f'at {cand[1]},{cand[2]})')
                    n['by_picture'] += 1
                    if opt['match_previews']:
                        pair_preview(opt['match_previews'],
                                     f'pic_lp{lp:04.1f}_q{q:04.1f}_c{cd:04.1f}_{name[:28]}_{sid[-40:]}', small, src)
                elif near is not None and opt['match_previews'] and near[1] >= 20.0:
                    pair_preview(opt['match_previews'],
                                 f'not_lp{near[1]:04.1f}_q{near[0]:04.1f}_c{near[2]:04.1f}_{name[:28]}_{sid[-40:]}',
                                 small, near[3])
                if found is None and src is None and near is not None:
                    note = f' (no PS4 sprite of the id; nearest picture {near[0]:.1f} dB, low-pass {near[1]:.1f}, colour {near[2]:.1f})'
            if src is None:
                n['no_twin'] += 1
                rows.append(f'  none {name}/{atlas} {sid} {aw}x{ah}{note or " (no PS4 sprite of the id)"}')
                continue
            if q >= opt['min_psnr'] and (found is not None or 'cut to the canvas' in note):
                # A picture from another canvas or another sprite: what the 720 sprite shows and it lacks (the
                # shadow under a master's lobby picture) stays, from the 720 sprite, under it.
                extra = matching.extras(small, src, resize)
                if extra is not None:
                    src = matching.with_extras(small, src, extra[0], resize)
                    under = True
                    note += f' (with {100.0 * extra[1]:.1f} % of the 720 sprite kept under it)'
                    n['kept_under'] += 1
                    if opt['match_previews']:
                        pair_preview(opt['match_previews'], f'und_{100.0 * extra[1]:04.1f}_{name[:28]}_{sid[-40:]}',
                                     small, src)
            rows.append(f'{q:6.2f} {name}/{atlas} {sid} {aw}x{ah} <- {pg} {bw}x{bh}{note}')
            if q < opt['min_psnr']:
                n['mismatch'] += 1
                continue
            n['replaced'] += 1
            if opt.get('recipe'):
                if rect is not None and matching.whole(whence[4:6], rect):
                    rect = None  # recut() returned the sprite as it is
                recipe.append((ax, ay, aw, ah) + tuple(whence) +
                              (None if rect is None else tuple(map(float, rect)), under))
            if not opt['stats']:
                patches.append(place(src, ax, ay, aw, ah, hw, hh))
        for k in stats:
            stats[k] += n[k]
        if opt['stats']:
            if opt.get('recipe'):
                out.append(dict(key=key, name=f'{name}/{atlas}', gw=w, gh=h, hw=hw, hh=hh, patches=recipe, **n))
            continue
        canvas = unpremul(resize(base, (hw, hh)))
        del base
        for patch, x0, y0, _ in patches:  # the rings first: an interior is never overwritten by a neighbour's ring
            canvas[y0:y0 + patch.shape[0], x0:x0 + patch.shape[1]] = patch
        for patch, x0, y0, (ix0, iy0, ix1, iy1) in patches:
            canvas[iy0:iy1, ix0:ix1] = patch[iy0 - y0:iy1 - y0, ix0 - x0:ix1 - x0]
        del patches
        fill_colours(canvas)
        if opt['previews']:
            os.makedirs(opt['previews'], exist_ok=True)
            Image.fromarray(canvas, 'RGBA').save(os.path.join(opt['previews'], f'{name}_{atlas}_hd.png'))
            Image.fromarray(rgba, 'RGBA').save(os.path.join(opt['previews'], f'{name}_{atlas}_720.png'))
        blocks = bc3.encode(canvas)
        # What the encoding cost: the atlas as the game will decode it against the picture it was made from, colour
        # over the texels that show (alpha of 8 and more) and alpha over all.
        back = ptx.decode_bc3(np.frombuffer(blocks, dtype=np.uint8).reshape(hh // 4, hw // 4, 16))
        shown = canvas[:, :, 3] >= KNOWN
        cd = (back[:, :, :3].astype(np.float32) - canvas[:, :, :3].astype(np.float32)) ** 2
        ad = (back[:, :, 3].astype(np.float32) - canvas[:, :, 3].astype(np.float32)) ** 2
        cm = float(cd[shown].mean()) if shown.any() else 0.0
        q_colour = 99.0 if cm <= 1e-9 else 10.0 * np.log10(255.0 * 255.0 / cm)
        am = float(ad.mean())
        q_alpha = 99.0 if am <= 1e-9 else 10.0 * np.log10(255.0 * 255.0 / am)
        del back, cd, ad, shown
        chunks = []
        for i in range(0, len(blocks), CHUNK):
            piece = blocks[i:i + CHUNK]
            z = lz4.block.compress(piece, mode='high_compression', compression=10, store_size=False)
            chunks.append(z if len(z) < len(piece) else piece)
        out.append(dict(key=key, name=f'{name}/{atlas}', gw=w, gh=h, hw=hw, hh=hh, raw=len(blocks),
                        hash=xxhash.xxh3_64_intdigest(blocks), chunks=chunks, q_colour=q_colour, q_alpha=q_alpha,
                        **n))
    return name, out, stats, rows, time.time() - started


def write_pack(folder, entries):
    entries.sort(key=lambda e: e['key'])
    head = 16 + 40 * len(entries)
    offset = head
    index = bytearray(struct.pack('<4I', MAGIC, VERSION, len(entries), CHUNK))
    for e in entries:
        index += struct.pack('<QQQII4H', e['key'], offset, e['hash'], e['raw'], len(e['chunks']), e['gw'], e['gh'],
                             e['hw'], e['hh'])
        offset += 4 * len(e['chunks']) + sum(len(c) for c in e['chunks'])
    path = os.path.join(folder, 'peggle1_hd.pack')
    with open(path, 'wb') as f:
        f.write(index)
        for e in entries:
            f.write(struct.pack('<%dI' % len(e['chunks']), *[len(c) for c in e['chunks']]))
            for c in e['chunks']:
                f.write(c)
    with open(os.path.join(folder, 'peggle1_hd.txt'), 'w', encoding='utf-8') as f:
        for e in entries:
            f.write('%016X %-64s %dx%d -> %dx%d, %d sprites from the PS4 (%d of them found by picture, %d cut to the '
                    'canvas), %d without one, %d of different art, %d KB, encoded at %.1f dB colour, %.1f dB alpha\n' % (
                        e['key'], e['name'], e['gw'], e['gh'], e['hw'], e['hh'], e['replaced'], e['by_picture'],
                        e['recut'], e['no_twin'] + e['bad_twin'], e['mismatch'],
                        (4 * len(e['chunks']) + sum(len(c) for c in e['chunks'])) // 1024, e['q_colour'], e['q_alpha']))
    return path, offset


def verify(path):
    """Reads the pack back the way the game does and checks every image against its stored hash."""
    with open(path, 'rb') as f:
        magic, version, count, chunk = struct.unpack('<4I', f.read(16))
        assert magic == MAGIC and version == VERSION and chunk == CHUNK
        index = [struct.unpack('<QQQII4H', f.read(40)) for _ in range(count)]
        assert all(index[i][0] < index[i + 1][0] for i in range(count - 1)), 'keys not sorted or repeated'
        for key, offset, digest, raw, chunks, gw, gh, hw, hh in index:
            assert raw == (hw // 4) * (hh // 4) * 16 and chunks == (raw + CHUNK - 1) // CHUNK
            f.seek(offset)
            sizes = struct.unpack('<%dI' % chunks, f.read(4 * chunks))
            data = bytearray()
            for i, s in enumerate(sizes):
                want = min(CHUNK, raw - i * CHUNK)
                z = f.read(s)
                data += z if s == want else lz4.block.decompress(z, uncompressed_size=want)
            assert len(data) == raw and xxhash.xxh3_64_intdigest(bytes(data)) == digest, 'image %016X' % key
    return count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--x360', required=True)
    ap.add_argument('--ps4', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--groups', default='')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--stats', action='store_true', help='only compare the sprite pairs (no pack)')
    ap.add_argument('--previews', default='')
    ap.add_argument('--match-previews', default='', help='folder for a picture of every match made by picture')
    ap.add_argument('--min-psnr', type=float, default=20.0, help='below this a PS4 sprite is not used')
    ap.add_argument('--sure-psnr', type=float, default=30.0, help='below this other sprites of the size are tried')
    a = ap.parse_args()
    started = time.time()
    x, p = rsblib.Rsb(a.x360), rsblib.Rsb(a.ps4)
    only = {g.strip().upper() for g in a.groups.split(',') if g.strip()}
    jobs = make_jobs(x, p, only)
    print(f'{len(jobs)} groups ({time.time() - started:.0f} s for the manifests)', flush=True)
    os.makedirs(a.out, exist_ok=True)
    opt = dict(stats=a.stats, previews=a.previews, min_psnr=a.min_psnr, sure_psnr=a.sure_psnr,
               match_previews=a.match_previews)
    entries, seen, total = [], set(), dict.fromkeys(COUNTS, 0)
    log = open(os.path.join(a.out, 'sprites.txt'), 'w', encoding='utf-8')
    with multiprocessing.Pool(a.jobs, init, (a.x360, a.ps4)) as pool:
        for done, (name, out, stats, rows, took) in enumerate(pool.imap_unordered(build_group, [(j, opt) for j in jobs])):
            for r in rows:
                log.write(r + '\n')
            for k in total:
                total[k] += stats[k]
            for e in out:
                if e['key'] not in seen:  # the same texture in two groups: once
                    seen.add(e['key'])
                    entries.append(e)
            print(f'[{done + 1}/{len(jobs)}] {name}: {len(out)} textures, {stats} ({took:.0f} s)', flush=True)
    log.close()
    print('sprites:', total)
    if not a.stats:
        path, size = write_pack(a.out, entries)
        print(f'{path}: {len(entries)} textures, {size} bytes '
              f'({sum(e["raw"] for e in entries)} of BC3), verified {verify(path)} images')
        qs = sorted(e['q_colour'] for e in entries)
        print('encoding, colour over the texels that show: lowest %.1f dB, a tenth under %.1f, median %.1f, '
              'highest %.1f; alpha lowest %.1f dB' % (qs[0], qs[len(qs) // 10], qs[len(qs) // 2], qs[-1],
                                                      min(e['q_alpha'] for e in entries)))
    print(f'{time.time() - started:.0f} s')


if __name__ == '__main__':
    main()
