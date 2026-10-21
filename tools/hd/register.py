"""Finds a PS4 sprite's picture inside (or around) a 720 sprite of another cut.

The two releases cut some animations into pieces differently: the same arm is 103x100 design units in one and
103x104 in the other, with another name. Such a 720 sprite has no PS4 sprite of its id, but one whose picture, laid
over it at the right offset, is the same picture. register() looks for that offset.

All images are premultiplied float32 (h, w, 4), 0..255. The 720 sprite is compared at the HD scale (1.5 times, the
PS4's own), so the offset is found in PS4 texels.
"""
import numpy as np

SUPPORT = 8.0  # alpha from which a texel counts as part of a picture


def _corr(fa, b, shape):
    """Cross-correlation of a (given as its transform) with b: out[dy, dx] = sum a[y, x] * b[y - dy, x - dx], with
    negative offsets wrapped to the end of each axis."""
    return np.fft.irfft2(fa * np.conj(np.fft.rfft2(b, shape)), shape)


def best_offset(s_up, c, cover_min=0.9):
    """The offset (dx, dy) of c's corner in s_up at which the two pictures differ least where either has something,
    among the offsets at which c covers at least cover_min of s_up's alpha. Returns (mean square error per channel
    over the texels that carry a picture, dx, dy, share of s_up's alpha covered) or None."""
    sh, sw = s_up.shape[:2]
    ch, cw = c.shape[:2]
    shape = (sh + ch, sw + cw)
    ones_s = np.ones((sh, sw), dtype=np.float32)
    ones_c = np.ones((ch, cw), dtype=np.float32)
    ms = (s_up[:, :, 3] >= SUPPORT).astype(np.float32)
    mc = (c[:, :, 3] >= SUPPORT).astype(np.float32)
    f_ones_s = np.fft.rfft2(ones_s, shape)
    f_ms = np.fft.rfft2(ms, shape)
    # sum of squares of each picture inside the overlap, and the product term, over the four channels
    ssd = _corr(np.fft.rfft2((s_up ** 2).sum(axis=2), shape), ones_c, shape) + \
        _corr(f_ones_s, (c ** 2).sum(axis=2), shape)
    for k in range(4):
        ssd -= 2.0 * _corr(np.fft.rfft2(s_up[:, :, k], shape), c[:, :, k], shape)
    support = _corr(f_ms, ones_c, shape) + _corr(f_ones_s, mc, shape) - _corr(f_ms, mc, shape)
    alpha = s_up[:, :, 3]
    total = float(alpha.sum())
    if total <= 0.0:
        return None
    cover = _corr(np.fft.rfft2(alpha, shape), ones_c, shape) / total
    need = max(16.0, 0.5 * float(ms.sum()))
    ok = (cover >= cover_min - 1e-4) & (support >= need)
    if not ok.any():
        return None
    mse = np.where(ok, np.maximum(ssd, 0.0) / (4.0 * np.maximum(support, 1.0)), np.inf)
    iy, ix = np.unravel_index(int(np.argmin(mse)), mse.shape)
    dy = iy if iy < sh else iy - shape[0]
    dx = ix if ix < sw else ix - shape[1]
    return float(mse[iy, ix]), int(dx), int(dy), float(cover[iy, ix])


def overlay(s_up, c, dx, dy):
    """s_up with c laid over it at (dx, dy): the picture a PS4 sprite of the 720 sprite's own cut would be, as far as
    c reaches; the rest stays the enlarged 720 picture."""
    out = s_up.copy()
    sh, sw = s_up.shape[:2]
    ch, cw = c.shape[:2]
    x0, y0, x1, y1 = max(dx, 0), max(dy, 0), min(dx + cw, sw), min(dy + ch, sh)
    if x1 > x0 and y1 > y0:
        out[y0:y1, x0:x1] = c[y0 - dy:y1 - dy, x0 - dx:x1 - dx]
    return out
