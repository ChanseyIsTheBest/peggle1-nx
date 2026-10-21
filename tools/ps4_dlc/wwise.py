"""Wwise sound banks of Peggle 2 (bank version 88) in both byte orders: the object list (HIRC) field by field.

The Xbox 360 build's banks are big-endian, the PS4's little-endian; the objects are the same structures. This module
walks an object with a reader that knows each field's width, which gives
  - the same object in the other byte order (Walker.out),
  - where its sound sources are (Walker.sources: offsets of each source record in the output), to point a source at
    other media,
  - the ids it refers to.
The layouts were worked out from the 12,000 objects both builds have (tools/ps4_dlc/wwise_pairs.py shows a pair's
field widths) and are checked by tools/ps4_dlc/wwise_check.py: every object of both banks must be consumed exactly.

Not a general Wwise tool: only what these two banks contain is handled, and an unknown case raises.
"""
import struct


class Walker:
    """Reads fields of `data` (byte order E) and writes them in the other byte order."""

    def __init__(self, data, E):
        self.d, self.E, self.p = data, E, 0
        self.out = bytearray()
        self.sources = []  # (offset in out, plugin, stream type, source id, file id, length of the record)
        self.fields = []   # (offset, width, name) when `trace` is set
        self.trace = False
        self.refs = []     # ids of what it holds: children, effects, state objects, actions, targets
        self.parent = 0
        self.parent_at = -1
        self.bus, self.aux, self.fx = [], [], []  # (offset in out, id) of its bus, aux sends, effects
        self.kids = []     # the ids of its children (a container)

    def _n(self, w, name):
        b = self.d[self.p:self.p + w]
        if len(b) != w:
            raise ValueError('object too short at %d (%s)' % (self.p, name))
        if self.trace:
            self.fields.append((self.p, w, name))
        self.p += w
        self.out += b[::-1]
        return int.from_bytes(b, 'little' if self.E == '<' else 'big')

    def u8(self, name=''):
        return self._n(1, name)

    def u16(self, name=''):
        return self._n(2, name)

    def u32(self, name=''):
        return self._n(4, name)

    def f32(self, name=''):
        return self._n(4, name)

    def f64(self, name=''):
        return self._n(8, name)

    def raw(self, n, name=''):
        b = self.d[self.p:self.p + n]
        if len(b) != n:
            raise ValueError('object too short at %d (%s)' % (self.p, name))
        if self.trace:
            self.fields.append((self.p, n, name + ' (bytes)'))
        self.p += n
        self.out += b
        return b

    def left(self):
        return len(self.d) - self.p


# ---- shared pieces ----
def source(w):
    """AkBankSourceData: the codec plug-in, how the media is stored, and which media."""
    at = len(w.out)
    plugin = w.u32('source plugin')
    stream = w.u32('source stream type')   # 0 in the bank, 1 streamed file, 2 streamed with its start in the bank
    sid = w.u32('source id')
    fid = w.u32('source file id')          # the bank that holds it, or the id again for a streamed file
    if stream != 1:
        w.u32('source offset in bank')
        w.u32('source bytes in bank')
    w.u8('source bits')
    if plugin & 0xF == 2:                  # a source plug-in (Wwise Silence): its parameter block
        size = w.u32('source plugin size')
        plugin_params(w, plugin, size)
    w.sources.append((at, plugin, stream, sid, fid, len(w.out) - at))


# Parameter blocks of effect and source plug-ins: the field widths of each plug-in id and block size. From the pairs
# both banks have (tools/ps4_dlc/wwise_check.py plugins), with the three whose pairs leave a width open (bytes that
# are equal in every pair) set as the plug-in's parameter structure has them: one byte per flag.
PLUGIN_FIELDS = {
    (0x00650002, 0): [],                                    # Wwise Silence
    (0x00650002, 12): [4, 4, 4],
    (0x00690003, 56): [4, 4, 4, 4, 1] * 3 + [4, 1],         # Parametric EQ: three bands, output level, LFE flag
    (0x006E0003, 22): [4, 4, 4, 4, 4, 1, 1],                # Peak Limiter
    (0x007D0003, 59): [4] * 14 + [1, 2],                    # Flanger (both banks have the same two)
    (0x00880003, 38): [4, 4, 4, 4, 1, 1, 4, 4, 4, 4, 4],    # Pitch Shifter
    (0x008A0003, 68): [1, 4, 4, 4, 4, 4, 4, 1, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 1, 1],  # Harmonizer
}


def plugin_params(w, plugin, size):
    if size == 0:
        return
    widths = PLUGIN_FIELDS.get((plugin, size))
    if widths is None:
        raise ValueError('no field widths for plug-in %08X with %d bytes of parameters' % (plugin, size))
    assert sum(widths) == size, (plugin, size)
    for width in widths:
        w._n(width, 'plugin %08X parameter' % plugin)


def rtpcs(w):
    for _ in range(w.u16('rtpc count')):
        w.u32('rtpc id')
        w.u32('rtpc parameter')
        w.u32('rtpc curve id')
        w.u8('rtpc scaling')
        for _ in range(w.u16('rtpc points')):
            w.f32('point from')
            w.f32('point to')
            w.u32('point shape')


def props(w):
    n = w.u8('property count')
    for _ in range(n):
        w.u8('property id')
    for _ in range(n):
        w.u32('property value')
    n = w.u8('ranged property count')
    for _ in range(n):
        w.u8('ranged property id')
    for _ in range(n):
        w.u32('ranged property min')
        w.u32('ranged property max')


def node_base(w):
    """NodeBaseParams: what every sound and container has."""
    w.u8('override parent fx')
    n = w.u8('fx count')
    if n:
        w.u8('fx bypass bits')
        for _ in range(n):
            w.u8('fx index')
            w.fx.append((len(w.out), w.u32('fx id')))
            w.refs.append(w.fx[-1][1])
            w.u8('fx is share set')
            w.u8('fx is rendered')
    w.bus.append((len(w.out), w.u32('override bus id')))
    w.parent_at = len(w.out)
    w.parent = w.u32('parent id')
    w.u8('priority override parent')
    w.u8('priority apply distance')
    props(w)
    if w.u8('positioning override parent'):
        has_2d = w.u8('2d positioning')
        has_3d = w.u8('3d positioning')
        if has_2d:
            w.u8('panner enabled')
        if has_3d:
            kind = w.u32('3d type')
            w.u32('attenuation id')
            w.u8('spatialized')
            if kind != 0:                  # game-defined (1 in these banks)
                w.u8('dynamic')
            else:                          # user-defined: a path
                w.u32('path mode')
                w.u8('path looping')
                w.u32('path transition time')
                w.u8('follow orientation')
                for _ in range(w.u32('path vertices')):
                    w.f32('x'), w.f32('y'), w.f32('z'), w.u32('duration')
                items = w.u32('path lists')
                for _ in range(items):
                    w.u32('first vertex'), w.u32('vertex count')
                for _ in range(items):
                    w.f32('x range'), w.f32('y range')
    w.u8('override game aux sends')
    w.u8('use game aux sends')
    w.u8('override user aux sends')
    if w.u8('has aux'):
        for _ in range(4):
            w.aux.append((len(w.out), w.u32('aux id')))
    w.u8('virtual queue behaviour')
    w.u8('kill newest')
    w.u8('use virtual behaviour')
    w.u16('max instances')
    w.u8('global limit')
    w.u8('below threshold behaviour')
    w.u8('max instances override parent')
    w.u8('virtual voices override parent')
    w.u8('override hdr envelope')
    w.u8('override analysis')
    w.u8('normalize loudness')
    w.u8('enable envelope')
    for _ in range(w.u32('state group count')):
        w.u32('state group id')
        w.u8('state sync type')
        for _ in range(w.u16('state count')):
            w.u32('state id')
            w.refs.append(w.u32('state object id'))
    rtpcs(w)


def children(w):
    for _ in range(w.u32('child count')):
        w.kids.append(w.u32('child id'))
    w.refs += w.kids


def music_node(w):
    node_base(w)
    children(w)
    w.f64('grid period')
    w.f64('grid offset')
    w.f32('tempo')
    w.u8('beats per bar')
    w.u8('beat value')
    w.u8('meter flag')
    for _ in range(w.u32('stinger count')):
        w.u32('trigger id'), w.u32('segment id'), w.u32('sync play at'), w.u32('cue filter')
        w.u32('do not repeat time'), w.u32('look-ahead count')


def music_trans_node(w):
    music_node(w)
    for _ in range(w.u32('transition rule count')):
        for _ in range(w.u32('rule source count')):
            w.u32('rule source id')
        for _ in range(w.u32('rule destination count')):
            w.u32('rule destination id')
        w.u32('source transition time'), w.u32('source fade curve'), w.u32('source fade offset')
        w.u32('source sync type'), w.u32('source cue filter'), w.u8('source play post-exit')
        w.u32('destination transition time'), w.u32('destination fade curve'), w.u32('destination fade offset')
        w.u32('destination cue filter'), w.u32('destination jump to'), w.u16('destination entry type')
        w.u8('destination play pre-entry'), w.u8('destination match cue name')
        if w.u8('has transition segment'):
            w.u32('transition segment id')
            w.u32('fade in time'), w.u32('fade in curve'), w.u32('fade in offset')
            w.u32('fade out time'), w.u32('fade out curve'), w.u32('fade out offset')
            w.u8('transition play pre-entry'), w.u8('transition play post-exit')


# ---- the object types ----
def state(w):
    n = w.u8('property count')
    for _ in range(n):
        w.u8('property id')
    for _ in range(n):
        w.u32('property value')


def sound(w):
    source(w)
    node_base(w)


def action(w):
    kind = w.u16('action type')
    w.refs.append(w.u32('target id'))
    w.u8('target is bus')
    props(w)
    group = kind >> 8
    if group in (0x01, 0x02, 0x03):          # stop, pause, resume
        w.u8('fade curve')
        if group in (0x02, 0x03):
            w.u8('include pending resume / master resume')
        exceptions(w)
    elif group == 0x04:                       # play
        w.u8('fade curve')
        w.u32('bank id')
    elif group in (0x12, 0x19):               # set state, set switch
        w.u32('group id')
        w.u32('value id')
    elif group in (0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x20, 0x30):
        # mute / set pitch / volume / bus volume / low-pass filter and their resets
        w.u8('fade curve')
        if group not in (0x06, 0x07):
            w.u8('value meaning')
            w.f32('value base')
            w.f32('value min')
            w.f32('value max')
        exceptions(w)
    elif group in (0x13, 0x14):               # set / reset game parameter
        w.u8('fade curve')
        w.u8('value meaning')
        w.f32('value base')
        w.f32('value min')
        w.f32('value max')
        exceptions(w)
    elif group in (0x10, 0x11):               # use state, do not use state
        pass
    elif group == 0x1A or group == 0x1B:      # bypass fx / reset bypass fx
        w.u8('bypass')
        w.u8('target mask')
        exceptions(w)
    elif group == 0x1C:                       # break
        pass
    elif group == 0x1D:                       # trigger
        pass
    elif group == 0x1E:                       # seek
        w.u8('seek relative to duration')
        w.f32('seek value')
        w.f32('seek min')
        w.f32('seek max')
        w.u8('snap to marker')
        exceptions(w)
    elif group == 0x21:                       # event
        pass
    else:
        raise ValueError('action type %04X' % kind)


def exceptions(w):
    for _ in range(w.u32('exception count')):
        w.u32('exception id')
        w.u8('exception is bus')


def event(w):
    for _ in range(w.u32('action count')):
        w.refs.append(w.u32('action id'))


def ran_seq(w):
    node_base(w)
    w.u16('loop count'), w.u16('loop min'), w.u16('loop max')
    w.f32('transition time'), w.f32('transition time min'), w.f32('transition time max')
    w.u16('avoid repeat count')
    for name in ('transition mode', 'random mode', 'mode', 'using weight', 'reset playlist at each play',
                 'restart backward', 'continuous', 'global'):
        w.u8(name)
    children(w)
    for _ in range(w.u16('playlist count')):
        w.u32('playlist id')
        w.u32('playlist weight')


def switch(w):
    node_base(w)
    w.u32('group type')
    w.u32('group id')
    w.u32('default switch')
    w.u8('continuous validation')
    children(w)
    for _ in range(w.u32('switch group count')):
        w.u32('switch id')
        for _ in range(w.u32('switch item count')):
            w.u32('switch node id')
    for _ in range(w.u32('switch parameter count')):
        w.u32('node id')
        w.u8('first only')
        w.u8('continue playback')
        w.u32('on switch mode')
        w.u32('fade out time')
        w.u32('fade in time')


def actor_mixer(w):
    node_base(w)
    children(w)


def layer(w):
    node_base(w)
    children(w)
    for _ in range(w.u32('layer count')):
        w.u32('layer id')
        rtpcs(w)
        w.u32('layer rtpc id')
        w.f32('layer crossfade default')
        for _ in range(w.u32('layer child count')):
            w.u32('layer child id')
            for _ in range(w.u32('layer curve points')):
                w.f32('point from')
                w.f32('point to')
                w.u32('point shape')


def music_segment(w):
    music_node(w)
    w.f64('duration')
    for _ in range(w.u32('marker count')):
        w.u32('marker id')
        w.f64('marker position')
        size = w.u32('marker name length')
        w.raw(size, 'marker name')


def music_track(w):
    for _ in range(w.u32('source count')):
        source(w)
    n = w.u32('playlist count')
    for _ in range(n):
        w.u32('track id')
        w.u32('playlist source id')
        w.f64('play at'), w.f64('begin trim'), w.f64('end trim'), w.f64('source duration')
    if n:
        w.u32('sub-track count')
    for _ in range(w.u32('clip automation count')):
        w.u32('clip index')
        w.u32('automation type')
        for _ in range(w.u32('automation points')):
            w.f32('point from')
            w.f32('point to')
            w.u32('point shape')
    node_base(w)
    w.u32('track type')
    w.u32('look-ahead time')


def music_switch(w):
    music_trans_node(w)
    w.u8('continue playback')
    depth = w.u32('tree depth')
    for _ in range(depth):
        w.u32('argument group id')
    for _ in range(depth):
        w.u8('argument group type')
    size = w.u32('tree bytes')
    w.u8('tree mode')
    end = w.p + size

    def node(level):
        w.u32('tree key')
        if level == depth:
            w.refs.append(w.u32('tree node id'))
            first = count = 0
        else:
            first, count = w.u16('tree first child'), w.u16('tree child count')
        w.u16('tree weight')
        w.u16('tree probability')
        return first, count

    # The nodes are stored level by level, the root first; a node's children are consecutive.
    levels = {0: 0}
    for index in range(size // 12):
        if index not in levels:
            raise ValueError('music switch tree: node %d has no parent' % index)
        first, count = node(levels[index])
        for c in range(first, first + count):
            levels[c] = levels[index] + 1
    if w.p != end:
        raise ValueError('music switch tree size')


def music_ran_seq(w):
    music_trans_node(w)
    for _ in range(w.u32('playlist item count')):
        w.u32('segment id')
        w.u32('playlist item id')
        w.u32('child count')
        w.u32('playlist type')
        w.u16('loop')
        w.u32('weight')
        w.u16('avoid repeat count')
        w.u8('using weight')
        w.u8('shuffle')


def attenuation(w):
    w.raw(w.left(), 'attenuation')  # both banks have the same two: taken from the 360 bank, never converted


def effect(w):
    plugin = w.u32('fx plugin id')
    size = w.u32('fx parameter bytes')
    plugin_params(w, plugin, size)
    for _ in range(w.u8('fx media count')):
        w.u8('fx media index')
        w.u32('fx media source id')
    rtpcs(w)


TYPES = {1: ('State', state), 2: ('Sound', sound), 3: ('Action', action), 4: ('Event', event),
         5: ('RanSeqCntr', ran_seq), 6: ('SwitchCntr', switch), 7: ('ActorMixer', actor_mixer),
         9: ('LayerCntr', layer), 10: ('MusicSegment', music_segment), 11: ('MusicTrack', music_track),
         12: ('MusicSwitchCntr', music_switch), 13: ('MusicRanSeqCntr', music_ran_seq),
         14: ('Attenuation', attenuation), 18: ('FxShareSet', effect), 19: ('FxCustom', effect)}


def walk(kind, body, E, trace=False):
    """Walks one object's bytes after its id. Returns the Walker (out = the other byte order)."""
    w = Walker(body, E)
    w.trace = trace
    TYPES[kind][1](w)
    if w.left():
        raise ValueError('%d bytes left over' % w.left())
    return w


# ---- bank files ----
def byte_order(data):
    for E in '<>':
        if struct.unpack_from(E + 'I', data, 4)[0] < 0x10000 and struct.unpack_from(E + 'I', data, 8)[0] < 0x1000:
            return E
    raise ValueError('not a sound bank')


def chunks(data):
    """(byte order, [(name, offset of its data, size)])."""
    E = byte_order(data)
    out = []
    p = 0
    while p + 8 <= len(data):
        size = struct.unpack_from(E + 'I', data, p + 4)[0]
        out.append((data[p:p + 4].decode('latin-1'), p + 8, size))
        p += 8 + size
    return E, out


def objects(data):
    """(byte order, [(type, id, body after the id)]) of a bank's HIRC chunk (empty when it has none)."""
    E, ch = chunks(data)
    out = []
    for name, at, _size in ch:
        if name != 'HIRC':
            continue
        q = at + 4
        for _ in range(struct.unpack_from(E + 'I', data, at)[0]):
            size = struct.unpack_from(E + 'I', data, q + 1)[0]
            out.append((data[q], struct.unpack_from(E + 'I', data, q + 5)[0], data[q + 9:q + 5 + size]))
            q += 5 + size
    return E, out


def media(data):
    """(byte order, {source id: (offset in the file, size)}) of a bank's embedded media."""
    E, ch = chunks(data)
    index, base = [], 0
    for name, at, size in ch:
        if name == 'DIDX':
            index = [struct.unpack_from(E + 'III', data, at + 12 * i) for i in range(size // 12)]
        elif name == 'DATA':
            base = at
    return E, {wid: (base + off, size) for wid, off, size in index}
