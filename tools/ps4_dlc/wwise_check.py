"""Checks tools/ps4_dlc/wwise.py against the two builds of one sound bank.

  python -P -E tools/ps4_dlc/wwise_check.py <x360 .BNK> <ps4 .BNK> [plugins | fields <type> <id>]

  (default)  every object of both banks must be walked to its last byte; the objects both banks have with the same
             size are converted PS4 -> big-endian and compared with the 360's bytes: the fields that differ are
             counted by name (a difference in a value both platforms may set differently is expected; a difference
             in a count, a flag or an id is a layout error)
  plugins    the field widths of the effect / source plug-in parameter blocks, from the pairs (for PLUGIN_FIELDS)
  fields     one object's fields, both builds side by side
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wwise


def plugin_blocks(objs, E):
    """{(plugin, size): [block bytes]} of the effect objects and of sounds with a source plug-in."""
    out = collections.defaultdict(list)
    for t, oid, body in objs:
        if t in (18, 19):
            plugin, size = struct.unpack_from(E + 'II', body, 0)
            out[(plugin, size)].append((oid, body[8:8 + size]))
        elif t == 2:
            plugin, stream = struct.unpack_from(E + 'II', body, 0)
            if plugin & 0xF == 2:
                at = 16 + (8 if stream != 1 else 0) + 1
                size = struct.unpack_from(E + 'I', body, at)[0]
                out[(plugin, size)].append((oid, body[at + 4:at + 4 + size]))
    return out


def learn(x, p):
    """Field widths per (plugin, size): 4 wherever every pair reads reversed as 4 bytes, else 2, else 1."""
    xb, pb = plugin_blocks(x[1], x[0]), plugin_blocks(p[1], p[0])
    table = {}
    for key, pblocks in sorted(pb.items()):
        xmap = dict(xb.get(key, []))
        pairs = [(le, xmap[oid]) for oid, le in pblocks if oid in xmap]
        size = key[1]
        widths = []
        i = 0
        while i < size:
            for wd in (4, 2, 1):
                if i + wd <= size and all(le[i:i + wd] == be[i:i + wd][::-1] for le, be in pairs):
                    widths.append(wd)
                    i += wd
                    break
            else:
                widths.append(1)
                i += 1
        table[key] = (widths, len(pairs), len(pblocks))
    return table


def main():
    xd, pd = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
    x, p = wwise.objects(xd), wwise.objects(pd)
    mode = sys.argv[3] if len(sys.argv) > 3 else ''
    table = learn(x, p)
    if mode == 'plugins':
        for (plugin, size), (widths, pairs, total) in table.items():
            print('    (0x%08X, %d): %r,  # %d pairs of %d PS4 objects' % (plugin, size, widths, pairs, total))
        return
    if not wwise.PLUGIN_FIELDS:
        wwise.PLUGIN_FIELDS.update({k: v[0] for k, v in table.items()})
    xi = {(t, oid): body for t, oid, body in x[1]}
    if mode == 'fields':
        t, oid = int(sys.argv[4]), int(sys.argv[5])
        pbody = next(b for tt, o, b in p[1] if (tt, o) == (t, oid))
        w = wwise.walk(t, pbody, p[0], trace=True)
        other = xi.get((t, oid))
        for at, width, name in w.fields:
            le = pbody[at:at + width]
            be = other[at:at + width] if other is not None and len(other) == len(pbody) else b''
            mark = '' if not be or le[::-1] == be or 'bytes' in name else '   <-- differs'
            print('%5d %-40s ps4 %-18s x360 %s%s' % (at, name, le[::-1].hex(), be.hex(), mark))
        return
    for tag, (E, objs) in (('x360', x), ('ps4', p)):
        bad = collections.Counter()
        first = {}
        for t, oid, body in objs:
            try:
                wwise.walk(t, body, E)
            except (ValueError, KeyError, IndexError, struct.error) as e:
                bad[t] += 1
                first.setdefault(t, (oid, str(e)))
        print('%s: %d objects, not walked to the end: %s' % (tag, len(objs), dict(bad) or 'none'))
        for t, (oid, why) in sorted(first.items()):
            print('    type %d, e.g. id %u: %s' % (t, oid, why))
    diff = collections.Counter()
    example = {}
    same = total = 0
    for t, oid, body in p[1]:
        other = xi.get((t, oid))
        if other is None or len(other) != len(body):
            continue
        try:
            w = wwise.walk(t, body, p[0], trace=True)
        except (ValueError, KeyError, IndexError, struct.error):
            continue
        total += 1
        if bytes(w.out) == other:
            same += 1
            continue
        for at, width, name in w.fields:
            if bytes(w.out[at:at + width]) != other[at:at + width]:
                diff[(t, name)] += 1
                example.setdefault((t, name), oid)
    print('objects in both banks with the same size: %d, of which identical after conversion: %d' % (total, same))
    for (t, name), n in sorted(diff.items()):
        print('    %-16s %-44s differs in %5d objects (e.g. %u)' % (wwise.TYPES[t][0], name, n, example[(t, name)]))


main()
