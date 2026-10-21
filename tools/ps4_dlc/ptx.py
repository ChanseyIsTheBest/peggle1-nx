"""Peggle 2's atlas textures (PTX format 5 = BC3 / DXT5, no header: the size is in the archive's records).

Xbox 360: the blocks in rows, in the standard byte order. PS4: the same blocks in tiles of 8x8 blocks, inside a tile
in Morton order (found by scoring every order of the six index bits on block-border continuity: ps4_order()).

  python tools/ps4_dlc/ptx.py <file.ptx> <width> <height> <ps4|x360> <out.png>      a preview
"""
import io
import itertools
import struct
import sys

import numpy as np
from PIL import Image

_order = None


def decode_bc3(blocks):
    """blocks: (bh, bw, 16) uint8 -> (bh * 4, bw * 4, 4) uint8 RGBA."""
    bh, bw = blocks.shape[:2]
    b = blocks.reshape(-1, 16).astype(np.uint32)
    n = b.shape[0]
    a0, a1 = b[:, 0], b[:, 1]
    abits = np.zeros(n, dtype=np.uint64)
    for i in range(6):
        abits |= b[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    c0 = b[:, 8] | (b[:, 9] << 8)
    c1 = b[:, 10] | (b[:, 11] << 8)
    cbits = b[:, 12] | (b[:, 13] << 8) | (b[:, 14] << 16) | (b[:, 15] << 24)

    def rgb(c):
        r, g, bl = (c >> 11) & 31, (c >> 5) & 63, c & 31
        return np.stack([(r * 255 + 15) // 31, (g * 255 + 31) // 63, (bl * 255 + 15) // 31], axis=1)
    p0, p1 = rgb(c0), rgb(c1)
    pal = np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], axis=1)
    apal = np.zeros((n, 8), dtype=np.uint32)
    apal[:, 0], apal[:, 1] = a0, a1
    gt = a0 > a1
    for i in range(1, 7):
        apal[:, 1 + i] = np.where(gt, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        apal[:, 1 + i] = np.where(gt, apal[:, 1 + i], ((5 - i) * a0 + i * a1) // 5)
    apal[:, 6] = np.where(gt, apal[:, 6], 0)
    apal[:, 7] = np.where(gt, apal[:, 7], 255)
    out = np.zeros((n, 16, 4), dtype=np.uint8)
    idx = np.arange(n)
    for k in range(16):
        ci = (cbits >> (2 * k)) & 3
        ai = ((abits >> np.uint64(3 * k)) & np.uint64(7)).astype(np.int64)
        out[:, k, :3] = pal[idx, ci]
        out[:, k, 3] = apal[idx, ai]
    return out.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)


def _index_map(bw, bh, perm):
    x = np.arange(bw, dtype=np.int64)[None, :].repeat(bh, 0)
    y = np.arange(bh, dtype=np.int64)[:, None].repeat(bw, 1)
    tile = (y // 8) * ((bw + 7) // 8) + (x // 8)
    bits = [x & 1, (x >> 1) & 1, (x >> 2) & 1, y & 1, (y >> 1) & 1, (y >> 2) & 1]
    pix = 0
    for pos, src in enumerate(perm):
        pix = pix | (bits[src] << pos)
    return tile * 64 + pix


def _score(img):
    f = img.astype(np.int32)
    dx = np.abs(f[:, 1:, :] - f[:, :-1, :]).sum(axis=2)
    dy = np.abs(f[1:, :, :] - f[:-1, :, :]).sum(axis=2)
    cols, rows = np.arange(dx.shape[1]), np.arange(dy.shape[0])
    border = (dx[:, (cols % 4) == 3].mean() + dy[(rows % 4) == 3, :].mean()) / 2
    inner = (dx[:, (cols % 4) != 3].mean() + dy[(rows % 4) != 3, :].mean()) / 2
    return border / max(inner, 1e-6)


def ps4_order(data, w, h):
    """The order of the six index bits inside a PS4 tile, found once on a texture with content."""
    global _order
    if _order is None:
        bw, bh = w // 4, h // 4
        lin = np.frombuffer(data, dtype=np.uint8)[:bw * bh * 16].reshape(bh * bw, 16)
        cw, ch = min(bw, 128), min(bh, 128)
        best = None
        for perm in itertools.permutations(range(6)):
            idx = _index_map(bw, bh, perm)[:ch, :cw]
            s = _score(decode_bc3(lin[idx.reshape(-1)].reshape(ch, cw, 16)))
            if best is None or s < best[0]:
                best = (s, perm)
        _order = best[1]
    return _order


def ps4_untile(data, w, h, order=None):
    """A PS4 texture's blocks in rows: (bh, bw, 16) uint8."""
    bw, bh = w // 4, h // 4
    assert bw % 8 == 0 and bh % 8 == 0, (w, h)
    lin = np.frombuffer(data, dtype=np.uint8)[:bw * bh * 16].reshape(bh * bw, 16)
    idx = _index_map(bw, bh, order or ps4_order(data, w, h))
    return lin[idx.reshape(-1)].reshape(bh, bw, 16)


def x360_blocks(data, w, h):
    bw, bh = w // 4, h // 4
    return np.frombuffer(data, dtype=np.uint8)[:bw * bh * 16].reshape(bh, bw, 16)


def encode_bc3(rgba):
    """(h, w, 4) uint8 RGBA, both sizes multiples of 4 -> the BC3 blocks in rows, as bytes (Pillow's encoder)."""
    h, w = rgba.shape[:2]
    assert h % 4 == 0 and w % 4 == 0
    out = io.BytesIO()
    Image.fromarray(rgba, 'RGBA').save(out, format='DDS', pixel_format='DXT5')
    b = out.getvalue()
    assert b[:4] == b'DDS ' and struct.unpack_from('<I', b, 4)[0] == 124
    body = b[128:]
    assert len(body) == (w // 4) * (h // 4) * 16, (len(body), w, h)
    return body


def resize_rgba(rgba, w, h):
    """Resampled with the colours weighted by alpha, so transparent pixels' colours do not bleed into the edges."""
    f = rgba.astype(np.float32) / 255.0
    pm = np.concatenate([f[:, :, :3] * f[:, :, 3:4], f[:, :, 3:4]], axis=2)
    chans = [np.asarray(Image.fromarray(pm[:, :, c], 'F').resize((w, h), Image.LANCZOS)) for c in range(4)]
    out = np.stack(chans, axis=2)
    a = np.clip(out[:, :, 3:4], 0.0, 1.0)
    rgb = np.where(a > 1e-4, out[:, :, :3] / np.maximum(a, 1e-4), 0.0)
    return (np.clip(np.concatenate([rgb, a], axis=2), 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def main():
    path, w, h, kind, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
    data = open(path, 'rb').read()
    blocks = ps4_untile(data, w, h) if kind == 'ps4' else x360_blocks(data, w, h)
    if kind == 'ps4':
        print('index bits inside a tile, lowest first:', [('x0', 'x1', 'x2', 'y0', 'y1', 'y2')[i] for i in _order])
    Image.fromarray(decode_bc3(blocks), 'RGBA').save(out)
    print(out)


if __name__ == '__main__':
    main()
