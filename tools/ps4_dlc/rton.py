"""PopCap RTON, both ways and exact: a file decoded and encoded again without a change is the same bytes.

Values keep the type byte they were stored with (Num, Str below), so only what a caller changes is re-encoded
differently. The format is little-endian on every platform (Peggle 2's Xbox 360 and PS4 files are byte-identical).

  python tools/ps4_dlc/rton.py <file.rton>            round-trip check, prints the top-level keys
  python tools/ps4_dlc/rton.py <file.rton> out.json   plain JSON (types dropped)
"""
import json
import struct
import sys

# Fixed-size numbers: type byte -> struct format. Types that carry no payload mean zero.
FIXED = {0x08: '<b', 0x0A: '<B', 0x10: '<h', 0x12: '<H', 0x20: '<i', 0x22: '<f', 0x26: '<I', 0x40: '<q', 0x42: '<d',
         0x46: '<Q'}
ZERO = {0x09: 0, 0x0B: 0, 0x11: 0, 0x13: 0, 0x21: 0, 0x23: 0.0, 0x27: 0, 0x41: 0, 0x43: 0.0, 0x47: 0}
VARINT = (0x24, 0x28, 0x44, 0x48)
ZIGZAG = (0x25, 0x29, 0x45, 0x49)


class Num:
    """A number with the type byte it is stored as."""
    __slots__ = ('tag', 'v')

    def __init__(self, tag, v):
        self.tag, self.v = tag, v

    def __repr__(self):
        return f'Num({self.tag:#x}, {self.v!r})'


class Str:
    """A string. kind: 0x81 plain latin-1, 0x82 plain UTF-8, 0x90 cached latin-1, 0x92 cached UTF-8 (a cached string
    is written whole the first time it occurs in the file and as an index afterwards)."""
    __slots__ = ('kind', 's')

    def __init__(self, kind, s):
        self.kind, self.s = kind, s

    def __repr__(self):
        return f'Str({self.kind:#x}, {self.s!r})'

    def __eq__(self, other):
        return isinstance(other, Str) and other.s == self.s

    def __hash__(self):
        return hash(self.s)


class Raw:
    """A value kept as its bytes (RTID references)."""
    __slots__ = ('tag', 'b')

    def __init__(self, tag, b):
        self.tag, self.b = tag, b


class Reader:
    def __init__(self, b):
        assert b[:4] == b'RTON', b[:4]
        self.b, self.p = b, 8
        self.version = struct.unpack_from('<I', b, 4)[0]
        self.s90, self.s92 = [], []

    def u8(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def varint(self):
        r = s = 0
        while True:
            c = self.u8()
            r |= (c & 0x7F) << s
            s += 7
            if not c & 0x80:
                return r

    def raw(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def value(self, t=None):
        if t is None:
            t = self.u8()
        if t in (0x00, 0x01):
            return bool(t)
        if t in FIXED:
            f = FIXED[t]
            v = struct.unpack_from(f, self.b, self.p)[0]
            self.p += struct.calcsize(f)
            return Num(t, v)
        if t in ZERO:
            return Num(t, ZERO[t])
        if t in VARINT:
            return Num(t, self.varint())
        if t in ZIGZAG:
            v = self.varint()
            return Num(t, -(v >> 1) - 1 if v & 1 else v >> 1)
        if t == 0x81:
            return Str(0x81, self.raw(self.varint()).decode('latin-1'))
        if t == 0x82:
            self.varint()  # the character count
            return Str(0x82, self.raw(self.varint()).decode('utf-8'))
        if t == 0x90:
            s = Str(0x90, self.raw(self.varint()).decode('latin-1'))
            self.s90.append(s.s)
            return s
        if t == 0x91:
            return Str(0x90, self.s90[self.varint()])
        if t == 0x92:
            self.varint()
            s = Str(0x92, self.raw(self.varint()).decode('utf-8'))
            self.s92.append(s.s)
            return s
        if t == 0x93:
            return Str(0x92, self.s92[self.varint()])
        if t in (0x83, 0x84):
            start = self.p
            if t == 0x83:
                st = self.u8()
                if st == 0x02:
                    self.varint()
                    self.raw(self.varint())
                    self.varint()
                    self.varint()
                    self.raw(4)
                elif st == 0x03:
                    for _ in range(2):
                        self.varint()
                        self.raw(self.varint())
                elif st != 0x00:
                    raise ValueError(f'RTID subtype {st:#x} at {self.p:#x}')
            return Raw(t, self.b[start:self.p])
        if t == 0x85:
            return self.obj()
        if t == 0x86:
            assert self.u8() == 0xFD
            arr = [self.value() for _ in range(self.varint())]
            assert self.u8() == 0xFE
            return arr
        raise ValueError(f'unknown RTON type {t:#04x} at {self.p - 1:#x}')

    def obj(self):
        """An object as a list of (key, value) pairs: order and repeated keys are kept."""
        items = []
        while True:
            t = self.u8()
            if t == 0xFF:
                return Obj(items)
            items.append((self.value(t), self.value()))


class Obj:
    """An RTON object: ordered (key, value) pairs with dictionary-style access by key text."""
    __slots__ = ('items',)

    def __init__(self, items=None):
        self.items = items if items is not None else []

    def get(self, key, default=None):
        for k, v in self.items:
            if k.s == key:
                return v
        return default

    def __getitem__(self, key):
        v = self.get(key, self)
        if v is self:
            raise KeyError(key)
        return v

    def __contains__(self, key):
        return any(k.s == key for k, _ in self.items)

    def set(self, key, value, kind=0x90):
        for i, (k, _) in enumerate(self.items):
            if k.s == key:
                self.items[i] = (k, value)
                return
        self.items.append((Str(kind, key), value))

    def keys(self):
        return [k.s for k, _ in self.items]


def decode(b):
    r = Reader(b)
    root = r.obj()
    assert b[r.p:r.p + 4] == b'DONE', b[r.p:r.p + 4]
    return root, r.version


def _varint(v):
    out = bytearray()
    while True:
        c = v & 0x7F
        v >>= 7
        if v:
            out.append(c | 0x80)
        else:
            out.append(c)
            return bytes(out)


class Writer:
    def __init__(self):
        self.out = bytearray()
        self.s90, self.s92 = {}, {}

    def value(self, v):
        o = self.out
        if v is True or v is False:
            o.append(1 if v else 0)
        elif isinstance(v, Num):
            o.append(v.tag)
            if v.tag in FIXED:
                o += struct.pack(FIXED[v.tag], v.v)
            elif v.tag in VARINT:
                o += _varint(v.v)
            elif v.tag in ZIGZAG:
                o += _varint(((-v.v - 1) << 1) | 1 if v.v < 0 else v.v << 1)
            else:
                assert v.tag in ZERO and v.v == 0, v
        elif isinstance(v, Str):
            if v.kind == 0x81:
                b = v.s.encode('latin-1')
                o.append(0x81)
                o += _varint(len(b)) + b
            elif v.kind == 0x82:
                b = v.s.encode('utf-8')
                o.append(0x82)
                o += _varint(len(v.s)) + _varint(len(b)) + b
            elif v.kind == 0x90:
                if v.s in self.s90:
                    o.append(0x91)
                    o += _varint(self.s90[v.s])
                else:
                    self.s90[v.s] = len(self.s90)
                    b = v.s.encode('latin-1')
                    o.append(0x90)
                    o += _varint(len(b)) + b
            elif v.kind == 0x92:
                if v.s in self.s92:
                    o.append(0x93)
                    o += _varint(self.s92[v.s])
                else:
                    self.s92[v.s] = len(self.s92)
                    b = v.s.encode('utf-8')
                    o.append(0x92)
                    o += _varint(len(v.s)) + _varint(len(b)) + b
            else:
                raise ValueError(v)
        elif isinstance(v, Raw):
            o.append(v.tag)
            o += v.b
        elif isinstance(v, Obj):
            o.append(0x85)
            self.obj(v)
        elif isinstance(v, list):
            o += b'\x86\xFD' + _varint(len(v))
            for x in v:
                self.value(x)
            o.append(0xFE)
        else:
            raise TypeError(type(v))

    def obj(self, obj):
        for k, v in obj.items:
            self.value(k)
            self.value(v)
        self.out.append(0xFF)


def encode(root, version=1):
    w = Writer()
    w.out += b'RTON' + struct.pack('<I', version)
    w.obj(root)
    w.out += b'DONE'
    return bytes(w.out)


def plain(v):
    """The value as plain Python (for JSON dumps and comparisons): types dropped."""
    if isinstance(v, Num):
        return v.v
    if isinstance(v, Str):
        return v.s
    if isinstance(v, Raw):
        return 'RTID'
    if isinstance(v, Obj):
        return {k.s: plain(x) for k, x in v.items}
    if isinstance(v, list):
        return [plain(x) for x in v]
    return v


def clone(v):
    """A deep copy of a value tree (to move entries from one file into another)."""
    if isinstance(v, Num):
        return Num(v.tag, v.v)
    if isinstance(v, Str):
        return Str(v.kind, v.s)
    if isinstance(v, Obj):
        return Obj([(clone(k), clone(x)) for k, x in v.items])
    if isinstance(v, list):
        return [clone(x) for x in v]
    return v


def main():
    data = open(sys.argv[1], 'rb').read()
    root, version = decode(data)
    again = encode(root, version)
    print('round trip:', 'identical' if again == data else f'DIFFERENT ({len(data)} -> {len(again)} bytes)',
          '; top-level keys', root.keys())
    if len(sys.argv) > 2:
        open(sys.argv[2], 'w', encoding='utf-8').write(json.dumps(plain(root), indent=1, ensure_ascii=False))
    return 0 if again == data else 1


if __name__ == '__main__':
    sys.exit(main())
