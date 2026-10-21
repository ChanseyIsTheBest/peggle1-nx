"""The Xbox 360 build's sound structure bank (GLOBAL_DATA.BNK) with the PS4 build's objects.

Both builds keep every sound object (events, actions, sounds, containers, music) in this one bank; the PS4's has the
objects of the content the 360 never got. The sound engine only loads a bank whose containers form one tree: every
child listed by exactly one container, and an event after its actions (both seen: a bank that mixes the two builds'
versions of containers is refused, "Could not load SoundBank"). So the merged bank is

  sounds and their containers   the PS4's tree as it is. An object both builds have keeps the 360's bytes (its own
                                mix values and media) when it has the same size, children and parent; a sound the 360
                                has keeps the 360's bytes in any case (its parent field set to the PS4's). All else
                                is the PS4's object, converted. The 360's objects that the PS4's tree does not have
                                (older takes of sounds, their containers) are left out.
  actions and events            the PS4's, converted (they name the PS4 tree's objects)
  music                         the 360's objects exactly, unless `music` is given (below)
  states, effects, attenuation  both builds' (the 360's bytes where both have one)

in an order in which what an object holds comes before it.
A converted object's sound sources are rewritten: a source the 360 build has media for gets the 360's own record
(codec, bank, place); the others get the record `media(source id, stream type, bits, object type)` returns, or one
that names no loaded media (the sound then stays silent, as the 360's own sounds without media do).
`relocate` = {bank id: bytes}: the media of that 360 bank have moved by so much in its file (media were added to the
bank); the 360's records of media in the bank follow.

Music (`music` = a function: is this source id one whose medium is being brought over?). The music is one tree of
switch containers, playlists, segments and tracks that both builds share down to the masters' branches; the PS4's has
the branches of its two extra masters (five switch containers both builds have list them, one per kind of music).
The merged music is the 360's tree with the PS4's version of exactly those containers that lie
above a new track with a wanted medium and differ from the 360's (they list the new branches), and everything those
list that the 360 lacks. A track both builds have keeps the 360's bytes, so the music that plays today is untouched.
A converted track whose media became streamed files gets `look_ahead` ms of look-ahead, as the 360's own streamed
tracks have (100 to 600): the music engine then schedules it that much early so the stream is ready on the beat.
`sounds=False` leaves everything that is not music as the 360 has it (a music-only merge).

  python -P -E tools/ps4_dlc/wwise_merge.py <x360 GLOBAL_DATA.BNK> <ps4 GLOBAL_DATA.BNK> [out.BNK]
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wwise

XMA, PCM, ADPCM = 0x00030001, 0x00010001, 0x00020001
MUSIC = (10, 11, 12, 13)
CONTAINERS = (5, 6, 7, 9)
OTHER = (1, 14, 18, 19)


def source_record(plugin, stream, sid, fid, offset=0, size=0, bits=0):
    """A big-endian source record (wwise.source)."""
    out = struct.pack('>IIII', plugin, stream, sid, fid)
    if stream != 1:
        out += struct.pack('>II', offset, size)
    return out + bytes([bits])


def merge(xdata, pdata, media=None, relocate=None, log=print, leave_out=(), busses=None, init_ids=None, sounds=True,
          music=None, look_ahead=0):
    xE, xo = wwise.objects(xdata)
    pE, po = wwise.objects(pdata)
    assert (xE, pE) == ('>', '<')
    relocate = relocate or {}
    busses = busses or {}
    # The 360's objects, their records of media in a bank that grew moved along.
    moved = 0
    if relocate:
        fixed = []
        for t, oid, body in xo:
            if t in (2, 11):
                out = None
                for at, plugin, stream, sid, fid, length in wwise.walk(t, body, xE).sources:
                    if plugin & 0xF == 1 and stream == 0 and fid in relocate:
                        out = out or bytearray(body)
                        struct.pack_into('>I', out, at + 16, struct.unpack_from('>I', out, at + 16)[0] + relocate[fid])
                        moved += 1
                if out is not None:
                    body = bytes(out)
            fixed.append((t, oid, body))
        xo = fixed
    xi = {(t, oid): body for t, oid, body in xo}
    pk = {(t, oid) for t, oid, _ in po}
    # The 360's record of every source it has.
    x_sources = {}
    for t, oid, body in xo:
        if t in (2, 11):
            for at, plugin, stream, sid, fid, length in wwise.walk(t, body, xE).sources:
                x_sources.setdefault(sid, body[at:at + length])
    effects = {oid for t, oid, _ in xo + po if t in (18, 19)}
    count = collections.Counter()
    chosen = {}   # (type, id) -> (big-endian body, ids it holds)
    order = []
    silent = set()

    def converted(t, w):
        out = bytearray(w.out)
        # A bus only the PS4's Init bank has: the sound engine refuses an object whose bus is not loaded.
        for at, bus in w.bus:
            if bus in busses:
                struct.pack_into('>I', out, at, busses[bus])
                count['  (converted objects moved to the nearest 360 bus)'] += 1
        for at, bus in w.aux:
            if bus in busses:
                struct.pack_into('>I', out, at, 0)
                count['  (aux sends to PS4-only busses removed)'] += 1
        # An effect only the PS4's Init bank has: the slot is emptied (id 0, as some slots of both banks are).
        for at, fx in w.fx:
            if fx and init_ids is not None and fx not in effects and fx not in init_ids:
                struct.pack_into('>I', out, at, 0)
                count['  (effects only the PS4 Init bank has: slot emptied)'] += 1
        for at, plugin, stream, sid, fid, length in reversed(w.sources):
            if plugin & 0xF == 2 or plugin == 0:
                continue  # a source plug-in (silence) or no source: nothing to point at
            bits = out[at + length - 1]
            record = x_sources.get(sid)
            if record is None and media is not None:
                record = media(sid, stream, bits, t)
            if record is None:
                record = source_record(XMA, 0, sid, fid, 0, 0, bits)
                silent.add(sid)
            out[at:at + length] = record
            if t == 11 and look_ahead and struct.unpack_from('>I', record, 4)[0] == 1:
                # A streamed source: the track's last field is its look-ahead time.
                if struct.unpack_from('>I', out, len(out) - 4)[0] < look_ahead:
                    struct.pack_into('>I', out, len(out) - 4, look_ahead)
                    count['  (converted tracks given look-ahead for their streamed media)'] += 1
        return bytes(out)

    for t, oid, body in po:
        if t in MUSIC or t in leave_out or not sounds:
            continue
        other = xi.get((t, oid))
        w = wwise.walk(t, body, pE)
        x = wwise.walk(t, other, xE) if other is not None else None
        if t == 2 and x is not None:
            if x.parent != w.parent:
                other = other[:x.parent_at] + struct.pack('>I', w.parent) + other[x.parent_at + 4:]
                count['sound both have: the 360 object under the PS4 parent'] += 1
            else:
                count['both, same structure: the 360 object'] += 1
            chosen[(t, oid)] = (other, x.refs)
        elif t in CONTAINERS + OTHER and x is not None and len(other) == len(body) and x.kids == w.kids and \
                x.parent == w.parent:
            chosen[(t, oid)] = (other, x.refs)
            count['both, same structure: the 360 object'] += 1
        else:
            chosen[(t, oid)] = (converted(t, w), w.refs)
            count['PS4 only: converted' if other is None else
                  'both: the PS4 object converted (%s)' % ('action / event' if t in (3, 4) else 'other structure')] += 1
        order.append((t, oid))
    for t, oid, body in xo:
        if t in MUSIC and music is not None:
            continue  # chosen below
        if t in MUSIC or not sounds or (t in OTHER and (t, oid) not in pk):
            chosen[(t, oid)] = (body, wwise.walk(t, body, xE).refs)
            order.append((t, oid))
            count['360 music object' if t in MUSIC else '360 object kept (sounds not merged)' if not sounds
                  else '360 only (state / effect): kept'] += 1
        elif (t, oid) not in pk:
            count['360 only (not in the PS4 tree): left out'] += 1
    if music is not None:
        pm = {oid: (t, body, wwise.walk(t, body, pE)) for t, oid, body in po if t in MUSIC}
        xm = {oid: (t, body, wwise.walk(t, body, xE)) for t, oid, body in xo if t in MUSIC}
        # The containers above a new track with a wanted medium.
        above = set()
        for oid, (t, body, w) in pm.items():
            if t == 11 and oid not in xm and any(plugin and plugin & 0xF != 2 and music(sid)
                                                 for _at, plugin, _stream, sid, _fid, _len in w.sources):
                count['music: new tracks with a wanted medium'] += 1
                a = w.parent
                while a in pm and a not in above:
                    above.add(a)
                    a = pm[a][2].parent
        taken = set()
        todo = [(oid, None) for oid, (t, body, w) in pm.items() if w.parent not in pm]
        todo += [(oid, None) for oid, (t, body, w) in xm.items() if w.parent not in xm and oid not in pm]
        todo.reverse()
        while todo:
            oid, parent = todo.pop()
            if oid in taken:
                continue
            taken.add(oid)
            ours = xm.get(oid)
            theirs = pm.get(oid)
            if ours is not None and (theirs is None or oid not in above or
                                     (ours[2].kids == theirs[2].kids and len(ours[1]) == len(theirs[1]))):
                t, body, w = ours
                if parent is not None and w.parent != parent:
                    body = body[:w.parent_at] + struct.pack('>I', parent) + body[w.parent_at + 4:]
                    count['music: a 360 object under another parent'] += 1
                chosen[(t, oid)] = (body, w.refs)
                count['music: the 360 object'] += 1
            elif theirs is not None:
                t, body, w = theirs
                chosen[(t, oid)] = (converted(t, w), w.refs)
                count['music: PS4 only, converted' if ours is None else
                      'music: the PS4 version of a container above the new tracks'] += 1
                # States and effects the PS4's music objects hold and a music-only merge has not brought.
                for ref in w.refs:
                    for kind in OTHER:
                        if (kind, ref) in pk and (kind, ref) not in chosen:
                            body2 = next(b for tt, oo, b in po if (tt, oo) == (kind, ref))
                            w2 = wwise.walk(kind, body2, pE)
                            chosen[(kind, ref)] = (converted(kind, w2), w2.refs)
                            order.append((kind, ref))
                            count['music: a state or effect only the PS4 has, converted'] += 1
            else:
                continue  # listed by a container, in neither bank's music
            order.append((t, oid))
            todo.extend((k, oid) for k in reversed(w.kids))
        count['music: 360 objects no kept container lists, left out'] = sum(1 for oid in xm if oid not in taken)
        count['music: PS4 objects not brought (under containers kept as the 360 has them)'] = \
            sum(1 for oid in pm if oid not in taken)
    by_id = collections.defaultdict(list)
    for key in order:
        by_id[key[1]].append(key)

    # What an object holds goes first: a container's children, effects and state objects, an event's actions. An
    # action's target need not be loaded, so an action holds nothing here.
    def held(key):
        return iter(chosen[key][1] if key[0] != 3 else ())
    emitted, final = set(), []
    for key in order:
        if key in emitted:
            continue
        emitted.add(key)
        stack = [(key, held(key))]
        while stack:
            k, it = stack[-1]
            for ref in it:
                nxt = next((c for c in by_id.get(ref, ()) if c not in emitted), None)
                if nxt is not None:
                    emitted.add(nxt)
                    stack.append((nxt, held(nxt)))
                    break
            else:
                final.append(k)
                stack.pop()
    assert len(final) == len(order) == len(set(final))
    hirc = bytearray(struct.pack('>I', len(final)))
    for t, oid in final:
        body = chosen[(t, oid)][0]
        hirc += bytes([t]) + struct.pack('>II', len(body) + 4, oid) + body
    out = bytearray()
    for name, at, size in wwise.chunks(xdata)[1]:
        if name == 'HIRC':
            out += b'HIRC' + struct.pack('>I', len(hirc)) + hirc
        else:
            out += xdata[at - 8:at + size]
    for k, v in sorted(count.items()):
        log('  %-58s %6d' % (k, v))
    log('  sources of converted objects without media: %d; 360 records moved with their bank: %d' % (len(silent), moved))
    return bytes(out), silent


def bus_fallbacks(x_init, p_init):
    """({bus only the PS4's Init bank has: its nearest ancestor that the 360's has}, the ids of the 360 Init bank's
    objects). A bus object (types 8 and 20) starts with its parent bus's id."""
    xE, xo = wwise.objects(x_init)
    pE, po = wwise.objects(p_init)
    have = {oid for t, oid, body in xo if t in (8, 20)}
    parent = {oid: struct.unpack_from(pE + 'I', body, 0)[0] for t, oid, body in po if t in (8, 20)}
    out = {}
    for oid in parent:
        if oid not in have:
            a = parent[oid]
            while a and a not in have:
                a = parent.get(a, 0)
            out[oid] = a
    return out, {oid for _t, oid, _b in xo}


def tree_report(data):
    """How far the bank's containers are from one tree: (children listed twice, listed children not in the bank,
    children whose parent field names another object, references to later objects, objects)."""
    E, objs = wwise.objects(data)
    owner = collections.defaultdict(list)
    parent, pos = {}, {}
    late = 0
    for i, (t, oid, body) in enumerate(objs):
        pos.setdefault(oid, i)
    for i, (t, oid, body) in enumerate(objs):
        w = wwise.walk(t, body, E)
        for k in w.kids:
            owner[k].append(oid)
        if t not in (1, 3, 4, 14, 18, 19):
            parent[oid] = w.parent
        if t != 3:
            late += sum(1 for r in w.refs if pos.get(r, -1) > i)
    return (sum(1 for v in owner.values() if len(v) > 1), sum(1 for k in owner if k not in pos),
            sum(1 for k, v in owner.items() if k in parent and parent[k] != v[0]), late, len(objs))


if __name__ == '__main__':
    x, p = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
    folder = [os.path.dirname(os.path.abspath(a)) for a in sys.argv[1:3]]
    busses, init_ids = bus_fallbacks(open(os.path.join(folder[0], 'INIT.BNK'), 'rb').read(),
                                     open(os.path.join(folder[1], 'INIT.BNK'), 'rb').read())
    data, silent = merge(x, p, busses=busses, init_ids=init_ids)
    if '--music-all' in sys.argv:  # a trial of the music-only merge with every new medium wanted
        data, silent = merge(x, p, busses=busses, init_ids=init_ids, sounds=False, music=lambda sid: True,
                             look_ahead=300)
    twice, absent, other, late, n = tree_report(data)
    print('merged bank: %d bytes, %d objects; children listed twice %d, listed but absent %d, with another parent '
          '%d; references to later objects %d' % (len(data), n, twice, absent, other, late))
    if len(sys.argv) > 3:
        os.makedirs(os.path.dirname(os.path.abspath(sys.argv[3])), exist_ok=True)
        open(sys.argv[3], 'wb').write(data)
