"""Reads and writes Peggle's progress save: the profile setting XPROFILE_TITLE_SPECIFIC1 (0x63E83FFF), which the SDK
keeps as the file <content root>/58410889/profile/User/63E83FFF (ResolveGameUserContentPath).

Peggle keeps no save-game package: its user data goes into the three title-specific profile settings
(sub_821F5598 writes them, sub_821F5830 reads them back). 0x63E83FFF is the "sync" stream of the profile object
(sub_821F4C20; one function for reading and writing, PopCap's DataSync pattern):
  u32 version (the game writes 28 and reads 19..28; little-endian, like every multi-byte field: sub_8271BB10)
  u32 x8      profile +44 (the stage reached: the multiplayer menu opens when it is >= 1, sub_823C3F20, "You'll need
              to complete the first stage of Adventure Mode before this option becomes available"), +48, +64, +52,
              +56, +68, +72, +60
  bools       128 (version < 28) or 192 bits of the set at +256, then 192 bits of the set at +284; bit-packed, least
              significant bit first (sub_82627438), each group starting a fresh byte (sub_82621790)
  map +268    u32 count, then count x (u32 key, u32 value) (sub_821FADF0)
  bools       +241 +80 +242 +100 +101 +243, one byte
  u32 +84, u8 +88, u8 +296, u8 +300, u32 +76, u8 +92, u32 +104, u32 +752, u32 +756, u32 +760
  stats x2    (+304, +528; sub_821D8DA0) u8 version 11, u32 x8, u32 count (24) + count x u32, u32, u32 x5
  version >= 21..25: further fields (shorts, doubles, bytes; see sub_821F4C20)
Before reading, the game resets the profile to its defaults (sub_821F4678), so a version-19 stream sets only the
fields above and leaves every later one at the game's own default. The other two settings (0x63E83FFE: a 768-byte
table; 0x63E83FFD: the Peggle Nights stream with a keyed SHA-1) may stay absent: the game then skips them.

  python -P tools/peggle1_save.py dump <file>
  python -P tools/peggle1_save.py write <file> [--multiplayer]
    (--multiplayer: profile +44 = 1, the first Adventure stage done, which unlocks the MultiPlayer menu)
Test saves only: never a player's real save.
"""
import argparse
import struct

# The profile's defaults (sub_821F4678) for the fields of a version-19 stream.
DEFAULT_LONGS = {44: 0, 48: 0, 64: 0, 52: 0xFFFFFFFF, 56: 0, 68: 0, 72: 1, 60: 0}
LONG_ORDER = [44, 48, 64, 52, 56, 68, 72, 60]
DEFAULT_FLAGS = [(241, 1), (80, 1), (242, 1), (100, 0), (101, 1), (243, 0)]


def bits(values, count):
    out = bytearray((count + 7) // 8)
    for i in range(count):
        if values.get(i):
            out[i // 8] |= 1 << (i % 8)
    return bytes(out)


def stats_block():
    # u8 version 11; +0, +4; +64 (version >= 10); +96, +100; +104..+112; count 24 + 24 values; +212 (>= 10);
    # +8..+24 (>= 11): all zero in a new profile (the reset clears them, sub_821D8DA0)
    longs = [0, 0, 0, 0, 0, 0, 0, 0, 24] + [0] * 24 + [0] + [0] * 5
    return bytes([11]) + b''.join(struct.pack('<I', v) for v in longs)


def build(multiplayer):
    longs = dict(DEFAULT_LONGS)
    if multiplayer:
        longs[44] = 1
    out = bytearray(struct.pack('<I', 19))
    for off in LONG_ORDER:
        out += struct.pack('<I', longs[off])
    out += bits({}, 128)          # set +256 (version < 28: 128 entries)
    out += bits({}, 192)          # set +284
    out += struct.pack('<I', 0)   # map +268: empty
    out += bits({i: v for i, (_, v) in enumerate(DEFAULT_FLAGS)}, 6)
    out += struct.pack('<I', 0xFFFFFFFF)  # +84
    out += bytes([0, 0, 0])               # +88, +296, +300
    out += struct.pack('<I', 0)           # +76
    out += bytes([0])                     # +92
    out += struct.pack('<IIII', 0, 0, 0, 0)  # +104, +752, +756, +760
    out += stats_block() + stats_block()  # +304, +528
    return bytes(out)


class Reader:
    def __init__(self, data):
        self.d, self.p = data, 0

    def take(self, n):
        if self.p + n > len(self.d):
            raise EOFError('the stream ends at %d, %d more bytes wanted' % (len(self.d), n))
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u32(self):
        return struct.unpack('<I', self.take(4))[0]

    def u8(self):
        return self.take(1)[0]

    def bools(self, n):
        raw = self.take((n + 7) // 8)
        return [i for i in range(n) if raw[i // 8] >> (i % 8) & 1]


def dump(data):
    r = Reader(data)
    version = r.u32()
    print('version %d, %d bytes' % (version, len(data)))
    for off in LONG_ORDER:
        v = r.u32()
        print('  +%d = %d (0x%08X)%s' % (off, struct.unpack('<i', struct.pack('<I', v))[0], v,
                                         '   <- stage reached (multiplayer needs >= 1)' if off == 44 else ''))
    print('  set +256: %s' % r.bools(192 if version >= 28 else 128))
    print('  set +284: %s' % r.bools(192))
    n = r.u32()
    print('  map +268: %s' % [(r.u32(), r.u32()) for _ in range(n)])
    flags = r.bools(6)
    print('  flags: %s' % ' '.join('+%d=%d' % (off, int(i in flags)) for i, (off, _) in enumerate(DEFAULT_FLAGS)))
    print('  +84 = %d, +88 = %d, +296 = %d, +300 = %d, +76 = %d, +92 = %d, +104 = %d, +752..760 = %d %d %d' % (
        struct.unpack('<i', struct.pack('<I', r.u32()))[0], r.u8(), r.u8(), r.u8(), r.u32(), r.u8(), r.u32(),
        r.u32(), r.u32(), r.u32()))
    print('  (%d bytes read; the rest is the two stats blocks and version >= 21 fields)' % r.p)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('dump')
    d.add_argument('file')
    w = sub.add_parser('write')
    w.add_argument('file')
    w.add_argument('--multiplayer', action='store_true')
    a = ap.parse_args()
    if a.cmd == 'dump':
        dump(open(a.file, 'rb').read())
    else:
        data = build(a.multiplayer)
        open(a.file, 'wb').write(data)
        print('wrote %d bytes to %s' % (len(data), a.file))
        dump(data)


if __name__ == '__main__':
    main()
