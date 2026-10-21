"""Research aid for the Wwise bank converter: objects that the Xbox 360 (big-endian) and PS4 (little-endian) builds of
one sound bank both have, shown side by side as the widths of their fields.

  python -P -E tools/ps4_dlc/wwise_pairs.py <x360 .BNK> <ps4 .BNK> stats
  python -P -E tools/ps4_dlc/wwise_pairs.py <x360 .BNK> <ps4 .BNK> show <type> [count] [skip] [min size]
  python -P -E tools/ps4_dlc/wwise_pairs.py <x360 .BNK> <ps4 .BNK> id <object id>

A field of width w reads the same in both byte orders when reversed: the listing splits each object greedily into
8/4/2/1-byte fields that do (zero runs and palindromes are ambiguous and shown as z<n> / raw bytes).
"""
import collections
import struct
import sys


def chunks(data):
    E = '>' if data[4:8] != struct.pack('<I', struct.unpack('<I', data[4:8])[0]) or data[8:12] == b'\0\0\0X' else '<'
    # the header chunk's version word decides: 88 in the bank's byte order
    for e in '<>':
        if struct.unpack_from(e + 'I', data, 8)[0] in range(1, 200):
            E = e
    out = {}
    p = 0
    while p + 8 <= len(data):
        cid = data[p:p + 4]
        size = struct.unpack_from(E + 'I', data, p + 4)[0]
        out[cid.decode('latin-1')] = (p + 8, size)
        p += 8 + size
    return E, out


def hirc(data):
    """[(type, id, body bytes after the id)] and the byte order."""
    E, ch = chunks(data)
    at, _size = ch['HIRC']
    n = struct.unpack_from(E + 'I', data, at)[0]
    q = at + 4
    out = []
    for _ in range(n):
        t = data[q]
        size = struct.unpack_from(E + 'I', data, q + 1)[0]
        oid = struct.unpack_from(E + 'I', data, q + 5)[0]
        out.append((t, oid, data[q + 9:q + 5 + size]))
        q += 5 + size
    return E, out


def split(le, be):
    """Field widths of two equally long byte strings (see the module text)."""
    out = []
    i = 0
    n = len(le)
    while i < n:
        if le[i] == 0 and be[i] == 0:
            j = i
            while j < n and le[j] == 0 and be[j] == 0:
                j += 1
            out.append('z%d' % (j - i))
            i = j
            continue
        for w in (8, 4, 2, 1):
            if w == 8 and not (i + 8 <= n and le[i:i + 4] != be[i:i + 4][::-1] and be[i] in (0x3f, 0x40, 0x41, 0xbf, 0xc0, 0xc1, 0x3d, 0x3e)):
                continue
            if i + w <= n and le[i:i + w] == be[i:i + w][::-1]:
                # a wider field that starts or ends in zero bytes was eaten by the zero run: show the value
                v = int.from_bytes(be[i:i + w], 'big')
                out.append('%d:%x' % (w, v))
                i += w
                break
        else:
            out.append('?%02x/%02x' % (le[i], be[i]))
            i += 1
    return ' '.join(out)


def main():
    x = open(sys.argv[1], 'rb').read()
    p = open(sys.argv[2], 'rb').read()
    _, xo = hirc(x)
    _, po = hirc(p)
    xi = {(t, oid): body for t, oid, body in xo}
    cmd = sys.argv[3]
    if cmd == 'stats':
        c = collections.Counter()
        for t, oid, body in po:
            other = xi.get((t, oid))
            c[(t, 'ps4 only' if other is None else 'same size' if len(other) == len(body) else 'other size')] += 1
        for k in sorted(c):
            print(k, c[k])
        return
    if cmd == 'id':
        want = int(sys.argv[4])
        for t, oid, body in po:
            if oid == want:
                other = xi.get((t, oid))
                print('type', t, 'ps4', len(body), 'x360', None if other is None else len(other))
                print('ps4 :', body.hex())
                if other is not None:
                    print('x360:', other.hex())
                    if len(other) == len(body):
                        print(split(body, other))
        return
    want = int(sys.argv[4])
    count = int(sys.argv[5]) if len(sys.argv) > 5 else 5
    skip = int(sys.argv[6]) if len(sys.argv) > 6 else 0
    least = int(sys.argv[7]) if len(sys.argv) > 7 else 0
    for t, oid, body in po:
        other = xi.get((t, oid))
        if t != want or other is None or len(other) != len(body) or len(body) < least:
            continue
        if skip:
            skip -= 1
            continue
        print('%u (%d bytes): %s' % (oid, len(body), split(body, other)))
        count -= 1
        if not count:
            break


main()
