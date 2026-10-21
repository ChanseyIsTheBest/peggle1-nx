"""BC3 (DXT5) encoding for the HD atlases: both halves of each block are made here. Pillow's encoder was used
first and lost too much: its alpha 10 dB on soft edges, and its colour half (endpoints from the block's extremes)
left dark squares in smooth gradients, plain to see on the pegs and the cannon at 1080p. Measure both with
python tools/hd/bc3.py <rgba png>.

Colour half (8 bytes): two 5:6:5 colours and sixteen 2-bit indices into them and the two thirds between them.
Per block: the line through its colours (their principal axis, texels weighted by alpha), its ends as the first
endpoints; then three times: the nearest of the four colours for every texel, and the endpoints that fit those
indices best (least squares); the best of the four tries is kept. colour0 > colour1 always (a block of one colour:
colour0 = colour1 with every index 0), so a decoder that takes the BC1 rule for colour0 <= colour1 reads the same.

Alpha half (8 bytes): two endpoints and sixteen 3-bit indices. Two layouts, chosen per block by squared error:
  A  a0 > a1: the eight values a0, a1 and six steps between them (endpoints = the block's maximum and minimum);
  B  a0 <= a1: a0, a1, four steps between them, 0 and 255 (endpoints = the smallest and largest value that is neither
     0 nor 255): exact for the fully transparent and fully opaque texels of a sprite's edge.
"""
import io
import struct

import numpy as np
from PIL import Image

_SLAB = 1 << 17  # blocks per pass (16 texels x 8 palette entries of int32 each)


def _blocks(plane):
    h, w = plane.shape
    return plane.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3).reshape(-1, 16)


def encode_alpha(alpha):
    """(h, w) uint8, both multiples of 4 -> (h / 4 * w / 4, 8) uint8, the alpha halves in rows."""
    blocks = _blocks(alpha)
    out = np.empty((blocks.shape[0], 8), dtype=np.uint8)
    shifts = (np.arange(16, dtype=np.uint64) * np.uint64(3))[None, :]
    for s in range(0, blocks.shape[0], _SLAB):
        b = blocks[s:s + _SLAB].astype(np.int32)
        mx, mn = b.max(1), b.min(1)
        pal_a = np.stack([mx, mn] + [((7 - i) * mx + i * mn) // 7 for i in range(1, 7)], axis=1)
        inner = (b > 0) & (b < 255)
        lo = np.where(inner, b, 255).min(1)
        hi = np.where(inner, b, 0).max(1)
        none = ~inner.any(1)
        lo, hi = np.where(none, 0, lo), np.where(none, 0, hi)
        pal_b = np.stack([lo, hi] + [((5 - i) * lo + i * hi) // 5 for i in range(1, 5)] +
                         [np.zeros_like(lo), np.full_like(lo, 255)], axis=1)
        da = np.abs(b[:, :, None] - pal_a[:, None, :])
        ia = da.argmin(2)
        ea = (da.min(2).astype(np.int64) ** 2).sum(1)
        db = np.abs(b[:, :, None] - pal_b[:, None, :])
        ib = db.argmin(2)
        eb = (db.min(2).astype(np.int64) ** 2).sum(1)
        # Layout A needs a0 > a1; a flat block (maximum = minimum) reads as layout B, whose index 0 is that value.
        use_b = (eb < ea) & (mx > mn)
        idx = np.where(use_b[:, None], ib, ia).astype(np.uint64)
        bits = (idx << shifts).sum(1, dtype=np.uint64)
        o = out[s:s + _SLAB]
        o[:, 0] = np.where(use_b, lo, mx)
        o[:, 1] = np.where(use_b, hi, mn)
        for k in range(6):
            o[:, 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    return out


def _to565(c):
    """(n, 3) float colours 0..255 -> (the 16-bit colour, its three channels as a decoder expands them)."""
    r = np.clip(np.rint(c[:, 0] * (31.0 / 255.0)), 0, 31).astype(np.int32)
    g = np.clip(np.rint(c[:, 1] * (63.0 / 255.0)), 0, 63).astype(np.int32)
    b = np.clip(np.rint(c[:, 2] * (31.0 / 255.0)), 0, 31).astype(np.int32)
    wide = np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=1).astype(np.float32)
    return ((r << 11) | (g << 5) | b).astype(np.uint16), wide


_COEF = np.array([1.0, 0.0, 2.0 / 3.0, 1.0 / 3.0], dtype=np.float32)  # colour0's share in the four colours


def encode_colour(rgb, alpha):
    """(h, w, 3) and (h, w) uint8, both sizes multiples of 4 -> (blocks, 8) uint8, the colour halves in rows."""
    h, w = alpha.shape
    px = rgb.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)
    al = _blocks(alpha)
    out = np.empty((px.shape[0], 8), dtype=np.uint8)
    shifts = (np.arange(16, dtype=np.uint32) * np.uint32(2))[None, :]
    for s in range(0, px.shape[0], _SLAB):
        x = px[s:s + _SLAB].astype(np.float32)
        # A transparent texel's colour still counts a little: the GPU's filter mixes it into its neighbours.
        wt = 0.15 + al[s:s + _SLAB].astype(np.float32) * np.float32(0.85 / 255.0)
        n = x.shape[0]
        xw = x * wt[:, :, None]
        mean = xw.sum(1) / wt.sum(1)[:, None]
        d = x - mean[:, None, :]
        cov = np.matmul((d * wt[:, :, None]).transpose(0, 2, 1), d)
        axis = x.max(1) - x.min(1) + np.float32(1e-3)
        for _ in range(8):
            axis = np.matmul(cov, axis[:, :, None])[:, :, 0]
            axis /= np.maximum(np.sqrt((axis * axis).sum(1, keepdims=True)), 1e-12)
        proj = (d * axis[:, None, :]).sum(2)
        e0 = mean + axis * proj.max(1)[:, None]
        e1 = mean + axis * proj.min(1)[:, None]
        best_err = np.full(n, np.inf, dtype=np.float32)
        best_q0 = np.zeros(n, dtype=np.uint16)
        best_q1 = np.zeros(n, dtype=np.uint16)
        best_idx = np.zeros((n, 16), dtype=np.uint8)
        for attempt in range(4):
            q0, p0 = _to565(e0)
            q1, p1 = _to565(e1)
            # The four colours are p1 + a * (p0 - p1), a = 1, 0, 2/3, 1/3 for the indices 0 to 3: the nearest one
            # follows from the texel's place along that line.
            line = p0 - p1
            place = ((x - p1[:, None, :]) * line[:, None, :]).sum(2) / np.maximum((line * line).sum(1), 1e-6)[:, None]
            idx = np.where(place > 5.0 / 6.0, 0, np.where(place > 0.5, 2, np.where(place > 1.0 / 6.0, 3, 1)))
            coef = _COEF[idx]
            diff = x - (p1[:, None, :] + coef[:, :, None] * line[:, None, :])
            err = ((diff * diff).sum(2) * wt).sum(1)
            better = err < best_err
            best_err = np.where(better, err, best_err)
            best_q0 = np.where(better, q0, best_q0)
            best_q1 = np.where(better, q1, best_q1)
            best_idx = np.where(better[:, None], idx, best_idx).astype(np.uint8)
            if attempt == 3:
                break
            # The endpoints that fit these indices best: texel = a * colour0 + (1 - a) * colour1.
            rest = 1.0 - coef
            wa, wb = wt * coef, wt * rest
            saa, sab, sbb = (wa * coef).sum(1), (wa * rest).sum(1), (wb * rest).sum(1)
            sax = (x * wa[:, :, None]).sum(1)
            sbx = (x * wb[:, :, None]).sum(1)
            det = saa * sbb - sab * sab
            ok = np.abs(det) > 1e-4
            safe = np.where(ok, det, 1.0)
            n0 = (sbb[:, None] * sax - sab[:, None] * sbx) / safe[:, None]
            n1 = (saa[:, None] * sbx - sab[:, None] * sax) / safe[:, None]
            e0 = np.where(ok[:, None], np.clip(n0, 0.0, 255.0), e0)
            e1 = np.where(ok[:, None], np.clip(n1, 0.0, 255.0), e1)
        swap = best_q0 < best_q1
        same = best_q0 == best_q1
        q0 = np.where(swap, best_q1, best_q0)
        q1 = np.where(swap, best_q0, best_q1)
        idx = np.where(swap[:, None], best_idx ^ 1, best_idx)   # 0 <-> 1, 2 <-> 3
        idx = np.where(same[:, None], 0, idx).astype(np.uint32)
        bits = (idx << shifts).sum(1, dtype=np.uint32)
        o = out[s:s + _SLAB]
        o[:, 0], o[:, 1] = (q0 & 255).astype(np.uint8), (q0 >> 8).astype(np.uint8)
        o[:, 2], o[:, 3] = (q1 & 255).astype(np.uint8), (q1 >> 8).astype(np.uint8)
        for k in range(4):
            o[:, 4 + k] = ((bits >> np.uint32(8 * k)) & np.uint32(255)).astype(np.uint8)
    return out


def encode(rgba):
    """(h, w, 4) uint8 RGBA, both sizes multiples of 4 -> the BC3 blocks in rows, as bytes."""
    h, w = rgba.shape[:2]
    assert h % 4 == 0 and w % 4 == 0
    body = np.empty(((w // 4) * (h // 4), 16), dtype=np.uint8)
    alpha = np.ascontiguousarray(rgba[:, :, 3])
    body[:, :8] = encode_alpha(alpha)
    body[:, 8:] = encode_colour(np.ascontiguousarray(rgba[:, :, :3]), alpha)
    return body.tobytes()


def encode_pillow(rgba):
    """The encoder used until 2026-10-14 (Pillow's colour half, this file's alpha half), for comparison."""
    h, w = rgba.shape[:2]
    out = io.BytesIO()
    Image.fromarray(rgba, 'RGBA').save(out, format='DDS', pixel_format='DXT5')
    b = out.getvalue()
    assert b[:4] == b'DDS ' and struct.unpack_from('<I', b, 4)[0] == 124
    body = np.frombuffer(b, dtype=np.uint8, offset=128).reshape(-1, 16).copy()
    body[:, :8] = encode_alpha(np.ascontiguousarray(rgba[:, :, 3]))
    return body.tobytes()


def main():
    """Quality of the encoders on an image: python tools/hd/bc3.py <rgba png> [shift] [x0 y0 x1 y1]
    (PSNR per channel; the colour channels also over the texels that are not transparent)."""
    import os
    import sys
    import time
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ps4_dlc'))
    import ptx
    img = np.asarray(Image.open(sys.argv[1]).convert('RGBA'))
    shift = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    if len(sys.argv) > 6:
        x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
        img = img[y0:y1, x0:x1]
    img = np.ascontiguousarray(np.roll(img, (shift, shift), axis=(0, 1))[:img.shape[0] // 4 * 4, :img.shape[1] // 4 * 4])
    h, w = img.shape[:2]
    seen = img[:, :, 3] >= 8

    def psnr(x, y, mask=None):
        d = (x.astype(np.float64) - y.astype(np.float64)) ** 2
        mse = d[mask].mean() if mask is not None else d.mean()
        return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
    for name, fn in (('Pillow', ptx.encode_bc3), ('Pillow + own alpha', encode_pillow), ('this file', encode)):
        started = time.time()
        data = fn(img)
        took = time.time() - started
        back = ptx.decode_bc3(ptx.x360_blocks(data, w, h))
        print('%-18s PSNR R %.2f G %.2f B %.2f A %.2f dB; colour where visible %.2f dB; %.1f s for %dx%d' % (
            (name,) + tuple(psnr(back[:, :, c], img[:, :, c]) for c in range(4)) +
            (psnr(back[:, :, :3], img[:, :, :3], seen), took, w, h)))


if __name__ == '__main__':
    main()
