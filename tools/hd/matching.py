"""Finds the PS4 sprite that shows the same picture as a 720 sprite when no PS4 sprite of its id does.

The two releases store some pieces of an animation at different sizes: the same shield is 93x82 design units in the
Xbox 360 animation and 83x74 or 111x99 in the PS4's (which then draws it scaled), so the ids, which carry the size,
differ, and the ratio of the two sprites' texels is not the 1.5 of the atlases. Other pictures have the same id but
another canvas: a master's lobby picture is 270x282 here and 668x698 there, the figure at another place in it.

A PS4 sprite may lie on a 720 sprite in two ways (ways()):
  - 'whole': sprite on sprite, when their proportions agree. Both releases trim a piece to its picture the same way,
    so this is the exact one for a piece stored at another size, and the only one used for a sprite of the same id
    and 1.5 times the size (the same cut);
  - 'content': the box around the solid texels of the one on that of the other (content_box(): alpha of a half
    and more, so a soft shadow or glow that one release draws and the other does not stays out of it), with
    recut() cutting or padding the PS4 picture to what the 720 sprite's whole canvas then covers. The boxes are
    known to a texel, so nudge() moves such a fit by half texels, and changes its size by up to 2 per cent when
    that is clearly better. What was tried first and taken back: a size search for every sprite. PSNR preferred the
    bricks 3 per cent narrower than the 720 ones (their glow), which the game's picture then showed.

Three measures decide whether it is the same picture, all on the PS4 picture scaled DOWN to the 720 sprite's size
(premultiplied colours, 0..255):
  - the PSNR of the two pictures;
  - the PSNR of both averaged over 3x3 texels ("low-pass"): the two releases filter their edges differently, which
    costs plain PSNR but averages out, while a picture that differs in a part of its area stays low;
  - the distance of their mean colours: the same shape in another colour (a pink and a blue burst of one effect,
    the 360's blue sparkle and the PS4's white one, the green A button and the PS4 archive's black one) passes
    the first two better than some true pairs.
The limits were set by eye on the borderline cases (the builder's --match-previews writes every match as a picture
named by its scores: of the 70 accepted pairs nearest the limits in the whole archive none was another picture; the
rejected ones nearest were recoloured pictures and a few true pairs of 10 texels).

A matched picture can lack something the 720 sprite has: the masters' lobby pictures stand on a shadow here and on
nothing there. extras() finds such an area (alpha the 720 sprite has and the PS4 picture does not, over more than a
fringe) and with_extras() keeps it from the 720 sprite, enlarged, under the PS4 picture.
"""
import numpy as np
from PIL import Image

SUPPORT = 8.0         # alpha from which a texel counts as part of a picture
MIN_SIDE = 6          # smaller sprites are not matched by picture: too few texels to tell pictures apart
MIN_PSNR = 22.0
MIN_LOWPASS = 26.0
MAX_COLOUR = 10.0     # of 255, the mean colour over the picture's alpha
TEXELS = (1.0, 3.2)   # PS4 texels per 720 texel: never a PS4 picture with less detail than the 720 one
LARGER_WITHIN = 3.0   # dB: a larger PS4 picture is preferred over the best match when it is this close to it


SOLID = 128.0         # alpha from which a texel is solid: what the content box is laid around


def box_of(pm, level=SUPPORT):
    """The box around the texels of a premultiplied picture with alpha of `level` and more: (x0, y0, x1, y1) or
    None."""
    m = pm[:, :, 3] >= level
    if not m.any():
        return None
    ys, xs = np.flatnonzero(m.any(axis=1)), np.flatnonzero(m.any(axis=0))
    return int(xs[0]), int(ys[0]), int(xs[-1]) + 1, int(ys[-1]) + 1


def content_box(pm):
    """The box two pictures of the same thing are laid on each other by: around the solid texels (a soft shadow or
    glow one release has and the other lacks stays out of it), or around everything when hardly anything is solid."""
    solid = box_of(pm, SOLID)
    if solid is not None and min(solid[2] - solid[0], solid[3] - solid[1]) >= MIN_SIDE:
        return solid
    return box_of(pm)


def fit(small_box, small_size, cand_box):
    """How a PS4 picture lies on a 720 sprite when their content boxes are laid on each other: (k, x0, y0, x1, y1),
    k = PS4 texels per 720 texel and the rectangle of the PS4 picture that the 720 sprite's canvas covers. None
    when the boxes' proportions differ (not the same picture)."""
    aw, ah = small_size
    sw, sh = small_box[2] - small_box[0], small_box[3] - small_box[1]
    cw, ch = cand_box[2] - cand_box[0], cand_box[3] - cand_box[1]
    kx, ky = cw / sw, ch / sh
    # Each box edge is known to a texel in either picture: more slack for small content.
    if abs(np.log(kx / ky)) > 0.04 + 1.5 / min(sw, sh):
        return None
    k = float(np.sqrt(kx * ky))
    scx, scy = (small_box[0] + small_box[2]) / 2.0, (small_box[1] + small_box[3]) / 2.0
    ccx, ccy = (cand_box[0] + cand_box[2]) / 2.0, (cand_box[1] + cand_box[3]) / 2.0
    return k, ccx - scx * k, ccy - scy * k, ccx + (aw - scx) * k, ccy + (ah - scy) * k


def whole(size, rect):
    """Whether the rectangle `rect` of a picture of `size` (w, h) is the whole picture (recut() returns it as it is)."""
    cw, ch = size
    x0, y0, x1, y1 = rect
    return abs(x0) < 0.26 and abs(y0) < 0.26 and abs(x1 - cw) < 0.26 and abs(y1 - ch) < 0.26


def recut(cand, rect, small_size):
    """The rectangle `rect` (x0, y0, x1, y1, in its texels, may reach outside) of the PS4 picture `cand` as a
    picture of its own, transparent where the rectangle is outside. The whole picture is returned as it is."""
    ch, cw = cand.shape[:2]
    x0, y0, x1, y1 = rect
    if whole((cw, ch), rect):
        return cand
    w, h = max(int(round(x1 - x0)), 1), max(int(round(y1 - y0)), 1)
    return np.stack([np.asarray(Image.fromarray(np.ascontiguousarray(cand[:, :, c]), 'F').transform(
        (w, h), Image.EXTENT, (x0, y0, x1, y1), resample=Image.BICUBIC)) for c in range(4)], axis=2)


def _box_filter(pm, size):
    return np.stack([np.asarray(Image.fromarray(np.ascontiguousarray(pm[:, :, k]), 'F').resize(size, Image.BOX))
                     for k in range(4)], axis=2)


def lowpass_psnr(a, c, psnr):
    h, w = a.shape[:2]
    size = (max(w // 3, 1), max(h // 3, 1))
    return psnr(_box_filter(a, size), _box_filter(c, size))


def colour_distance(a, c):
    """Distance of the mean colours of two premultiplied pictures (each weighted by its own alpha), 0..255 scale."""
    wa, wc = float(a[:, :, 3].sum()), float(c[:, :, 3].sum())
    if wa <= 0.0 or wc <= 0.0:
        return 0.0 if wa == wc else 255.0
    ma = a[:, :, :3].sum(axis=(0, 1)) * (255.0 / wa)
    mc = c[:, :, :3].sum(axis=(0, 1)) * (255.0 / wc)
    return float(np.sqrt(((ma - mc) ** 2).sum()))


def measures(small, src, resize, psnr):
    """(PSNR, low-pass PSNR, colour distance) of a PS4 picture cut to the 720 sprite's canvas against the sprite."""
    ah, aw = small.shape[:2]
    down = resize(src, (aw, ah))
    return psnr(down, small), lowpass_psnr(down, small, psnr), colour_distance(down, small)


def accepted(q, lp, cd):
    return q >= MIN_PSNR and lp >= MIN_LOWPASS and cd <= MAX_COLOUR


def ways(sb, size, cand_size, cb):
    """The ways a PS4 sprite may lie on a 720 sprite, as (kind, k, rectangle of the PS4 sprite the 720 canvas covers):
    'whole' = sprite on sprite (both trimmed to the picture the same way: the exact one when it applies), 'content' =
    content box on content box (the picture sits elsewhere on its canvas; each box edge is known to a texel only)."""
    aw, ah = size
    ow, oh = cand_size
    out = []
    kx, ky = ow / aw, oh / ah
    if abs(np.log(kx / ky)) <= 0.04 + 1.5 / min(aw, ah):
        out.append(('whole', float(np.sqrt(kx * ky)), (0.0, 0.0, float(ow), float(oh))))
    f = fit(sb, size, cb) if cb is not None else None
    if f is not None and not (out and all(abs(u - v) < 0.6 * f[0] for u, v in zip(f[1:], out[0][2]))):
        out.append(('content', f[0], f[1:]))
    return [w for w in out if TEXELS[0] <= w[1] <= TEXELS[1]]


def nudge(small, cand, k, rect, q, resize, psnr):
    """A content fit moved to where the two pictures agree best: by up to a 720 texel in half-texel steps (the boxes
    it came from are known to a texel), then, only when that gains 0.75 dB or more, with its size changed by up to
    2 per cent around its centre (the boxes give the size to about a per cent) and moved once more. Only pictures
    on a canvas of another size come here: a sprite of the same cut keeps the size its rectangle gives it.
    Returns (psnr, picture or None when nothing was better, rectangle)."""
    size = (small.shape[1], small.shape[0])
    best = [q, None, rect]

    def attempt(r, margin=0.0):
        src = recut(cand, r, size)
        got = psnr(resize(src, size), small)
        if got > best[0] + margin:
            best[0], best[1], best[2] = got, src, r
            return True
        return False

    def moves():
        base = best[2]
        for dy in (-1.0, -0.5, 0.0, 0.5, 1.0):
            for dx in (-1.0, -0.5, 0.0, 0.5, 1.0):
                if dx or dy:
                    attempt((base[0] + dx * k, base[1] + dy * k, base[2] + dx * k, base[3] + dy * k))

    moves()
    base, sized = best[2], False
    cx, cy = (base[0] + base[2]) / 2.0, (base[1] + base[3]) / 2.0
    hw, hh = (base[2] - base[0]) / 2.0, (base[3] - base[1]) / 2.0
    for s in (0.98, 0.99, 1.01, 1.02):
        sized |= attempt((cx - hw * s, cy - hh * s, cx + hw * s, cy + hh * s), 0.75)
    if sized:
        moves()
    return tuple(best)


def _near(mask, grow):
    """A mask grown (grow=True) or shrunk by one texel in the eight directions."""
    p = np.pad(mask, 1, mode='constant', constant_values=not grow)
    out = mask.copy()
    h, w = mask.shape
    for dy in (0, 1, 2):
        for dx in (0, 1, 2):
            part = p[dy:dy + h, dx:dx + w]
            out = (out | part) if grow else (out & part)
    return out


def extras(small, src, resize):
    """Where the 720 sprite shows something the PS4 picture laid on it does not: a mask at the 720 sprite's size and
    the share of the sprite's alpha in it, or None when there is nothing of the kind.

    The 720 sprite's alpha must exceed the PS4 picture's by 40 of 255 over an area that survives shrinking by a
    texel (so the fringes along edges the two releases filter differently do not count); the area is then grown by
    two texels to take the soft rim of a shadow with it."""
    ah, aw = small.shape[:2]
    down = resize(src, (aw, ah))
    more = (small[:, :, 3] - down[:, :, 3]) > 40.0
    core = _near(_near(more, False), True)
    if int(core.sum()) < 12:
        return None
    total = float(small[:, :, 3].sum())
    share = float(small[:, :, 3][core].sum()) / total if total > 0 else 0.0
    if share < 0.01:
        return None
    return _near(_near(core, True), True), share


def with_extras(small, src, mask, resize):
    """The PS4 picture `src` over the part `mask` of the 720 sprite, enlarged to src's size (premultiplied over)."""
    sh, sw = src.shape[:2]
    part = small * mask[:, :, None].astype(np.float32)
    under = resize(part, (sw, sh))
    return src + under * (1.0 - np.clip(src[:, :, 3:4], 0.0, 255.0) / 255.0)


def refit(small, cand, resize, psnr):
    """The PS4 picture `cand` cut to the canvas of the 720 sprite by content, with its PSNR: (psnr, picture, k,
    rectangle of `cand` the cut covers), or None when the contents' proportions differ or the cut is the whole
    picture (nothing new)."""
    sb, cb = content_box(small), content_box(cand)
    if sb is None or cb is None:
        return None
    size = (small.shape[1], small.shape[0])
    f = fit(sb, size, cb)
    if f is None or not (TEXELS[0] <= f[0] <= TEXELS[1]):
        return None
    src = recut(cand, f[1:], size)
    if src is cand:
        return None
    q = psnr(resize(src, size), small)
    moved = nudge(small, cand, f[0], f[1:], q, resize, psnr)
    return (moved[0], moved[1], f[0], moved[2]) if moved[1] is not None else (q, src, f[0], f[1:])


def by_picture(small, cands, cand_box, cand_picture, resize, psnr, skip=None):
    """The PS4 sprite of `cands` ((atlas, x, y, w, h), ...) whose picture is that of the 720 sprite `small`.

    cand_box(cand) returns a candidate's content box (or None), cand_picture(cand) its premultiplied picture.
    Returns (found, near): found = (psnr, low-pass, colour distance, picture cut to the sprite's canvas, candidate,
    k, rectangle of the candidate the cut covers) of the chosen sprite or None; near = the same for the best
    candidate when none was accepted (for reports)."""
    ah, aw = small.shape[:2]
    sb = content_box(small)
    if sb is None or min(sb[2] - sb[0], sb[3] - sb[1]) < MIN_SIDE:
        return None, None
    entries = []
    for cand in cands:
        cand = tuple(cand[:5])
        if cand == skip or not cand[3] or not cand[4]:
            continue
        # by the sprites' sizes first: the content is never larger than the sprite
        if cand[3] < TEXELS[0] * (sb[2] - sb[0]) - 2 or cand[4] < TEXELS[0] * (sb[3] - sb[1]) - 2:
            continue
        todo = ways(sb, (aw, ah), (cand[3], cand[4]), cand_box(cand))
        if not todo:
            continue
        c = cand_picture(cand)
        if c is None:
            continue
        mine = None
        for kind, k, rect in todo:
            src = recut(c, rect, (aw, ah))
            q = psnr(resize(src, (aw, ah)), small)
            if mine is None or q > mine[0]:
                mine = [q, src, cand, k, kind, rect, c]
        entries.append(mine)
    if not entries:
        return None, None
    entries.sort(key=lambda e: -e[0])
    # The two best content fits get their fine position (a whole fit is exact already).
    for e in entries[:2]:
        if e[4] == 'content' and e[0] >= 15.0:
            moved = nudge(small, e[6], e[3], e[5], e[0], resize, psnr)
            if moved[1] is not None:
                e[0], e[1], e[5] = moved
    good, best = [], None
    for q, src, cand, k, kind, rect, c in entries[:6]:
        down = resize(src, (aw, ah))
        entry = (q, lowpass_psnr(down, small, psnr), colour_distance(down, small), src, cand, k, rect)
        if accepted(*entry[:3]):
            good.append(entry)
        if best is None or q > best[0]:
            best = entry
    if not good:
        return None, best
    top = max(good, key=lambda e: e[0])
    # Among the accepted ones about as good as the best, the one stored largest: the same picture with more detail.
    close = [e for e in good if e[0] >= top[0] - LARGER_WITHIN]
    return max(close, key=lambda e: (e[5], e[0])), None
