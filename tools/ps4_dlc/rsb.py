"""PopCap RSB version 4, reader and writer, for Peggle 2 ("rsb1" big-endian on the Xbox 360, "1bsr" little-endian on
the PS4; the same layout in both).

Layout (every number in the archive's byte order):
  header 0x70 bytes, then in this order: the file map (path -> packet index), the packet-name map, the composite
  records (1156 bytes: name, up to 64 {packet index, resolution, language} and their count), the composite-name map,
  the packet records (204 bytes), the pool records (152 bytes, one per packet), the texture records (16 bytes: width,
  height, pitch, format), padding to 4 KiB, then three description tables this tool keeps as they are, then the packets.
  A packet ("rsgp"): header 0x5C bytes, its own file map (path -> type, offset, size [, texture index, 0, 0, width,
  height]), padding to 4 KiB, part 0 (plain files, each padded to 4 KiB, stored raw) and part 1 (textures, zlib).
  A map is a compiled string trie: 32-bit cells, low byte a character, high 24 bits the cell index where the strings
  that share the prefix so far continue; the value follows the terminating zero.

The writer copies every existing packet's bytes untouched and can replace plain files in a packet, add packets,
composites and textures. See build_mod.py for its use.
"""
import struct
import zlib

PAGE = 0x1000


def pad(n, to=PAGE):
    return (n + to - 1) // to * to


def parse_trie(buf, begin, length, E, value_fn):
    out = []
    forks = {}
    pos, end = begin, begin + length
    while pos < end:
        key = forks.pop(pos, '')
        while True:
            v = struct.unpack_from(E + 'I', buf, pos)[0]
            pos += 4
            c, o = v & 0xFF, v >> 8
            if o:
                forks[begin + o * 4] = key
            if c == 0:
                break
            key += chr(c)
        val, pos = value_fn(buf, pos)
        out.append((key, val))
    return out


def build_trie(entries, E, value_bytes):
    """entries: [(key, value)] with unique keys; value_bytes(value) -> the bytes that follow the key (a multiple of
    4). Returns the map's bytes."""
    cells = []  # ints (a character cell, its link filled in later) or bytes (a value)

    def emit(group, p):
        # group: entries that share key[:p]; the first is written from p, the others hang off the cell where they
        # leave it.
        first_key, first_val = group[0]
        start = len(cells)
        chars = [ord(c) for c in first_key[p:]] + [0]
        for c in chars:
            assert 0 <= c < 256, first_key
            cells.append(c)
        vb = value_bytes(first_val)
        assert len(vb) % 4 == 0
        for i in range(0, len(vb), 4):
            cells.append(vb[i:i + 4])
        by_split = {}
        for key, val in group[1:]:
            i = p
            n = min(len(key), len(first_key))
            while i < n and key[i] == first_key[i]:
                i += 1
            by_split.setdefault(i, []).append((key, val))
        for i in sorted(by_split, reverse=True):  # the order of the sorted keys: the longest shared prefix first
            target = len(cells)
            cells[start + (i - p)] |= target << 8
            emit(by_split[i], i)

    if entries:
        import sys
        limit = sys.getrecursionlimit()
        sys.setrecursionlimit(max(limit, 20000))
        emit(sorted(entries, key=lambda e: e[0]), 0)
        sys.setrecursionlimit(limit)
    out = bytearray()
    for c in cells:
        out += c if isinstance(c, (bytes, bytearray)) else struct.pack(E + 'I', c)
    return bytes(out)


HEADER_FIELDS = ['version', '_z', 'file_offset', 'file_list_len', 'file_list_begin', '_a', '_b', 'rsg_list_len',
                 'rsg_list_begin', 'rsg_count', 'rsg_info_begin', 'rsg_info_size', 'comp_count', 'comp_info_begin',
                 'comp_info_size', 'comp_list_len', 'comp_list_begin', 'pool_count', 'pool_info_begin',
                 'pool_info_size', 'ptx_count', 'ptx_info_begin', 'ptx_info_size', 'part1_begin', 'part2_begin',
                 'part3_begin', 'header_end']
RSG_FIELDS = ['offset', 'length', 'pool', 'flags', 'header_len', 'part0_off', 'part0_zsize', 'part0_size', 'unk',
              'part1_off', 'part1_zsize', 'part1_size', 'r0', 'r1', 'r2', 'r3', 'r4', 'ptx_count', 'ptx_before']


def _u32val(E):
    return lambda buf, pos: (struct.unpack_from(E + 'I', buf, pos)[0], pos + 4)


class Rsb:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        magic = self.f.read(4)
        self.E = {b'1bsr': '<', b'rsb1': '>'}[magic]
        self.magic = magic
        E = self.E
        self.f.seek(0)
        h = self.f.read(0x70)
        self.h = dict(zip(HEADER_FIELDS, struct.unpack_from(E + '27I', h, 4)))
        self.f.seek(0)
        self.head = hd = self.f.read(self.h['file_offset'])
        H = self.h
        self.file_list = parse_trie(hd, H['file_list_begin'], H['file_list_len'], E, _u32val(E))
        self.rsg_list = parse_trie(hd, H['rsg_list_begin'], H['rsg_list_len'], E, _u32val(E))
        self.comp_list = parse_trie(hd, H['comp_list_begin'], H['comp_list_len'], E, _u32val(E))
        self.rsgs = []
        for i in range(H['rsg_count']):
            o = H['rsg_info_begin'] + i * H['rsg_info_size']
            g = dict(zip(RSG_FIELDS, struct.unpack_from(E + '19I', hd, o + 128)))
            g.update(index=i, name=hd[o:o + 128].split(b'\0')[0].decode('latin-1'))
            self.rsgs.append(g)
        self.composites = []
        for i in range(H['comp_count']):
            o = H['comp_info_begin'] + i * H['comp_info_size']
            n = struct.unpack_from(E + 'I', hd, o + 128 + 1024)[0]
            subs = []
            for k in range(n):
                idx, res, lang, _ = struct.unpack_from(E + 'II4sI', hd, o + 128 + k * 16)
                subs.append((idx, res, (lang[::-1] if E == '<' else lang).replace(b'\0', b'').decode('latin-1')))
            self.composites.append(dict(index=i, name=hd[o:o + 128].split(b'\0')[0].decode('latin-1'), subs=subs))
        self.pools = []
        for i in range(H['pool_count']):
            o = H['pool_info_begin'] + i * H['pool_info_size']
            self.pools.append(dict(index=i, name=hd[o:o + 128].split(b'\0')[0].decode('latin-1'),
                                   vals=struct.unpack_from(E + '6I', hd, o + 128)))
        self.ptx = [struct.unpack_from(E + '4I', hd, H['ptx_info_begin'] + i * H['ptx_info_size'])
                    for i in range(H['ptx_count'])]
        self.by_name = {g['name'].upper(): g for g in self.rsgs}
        self.comp_by_name = {c['name'].upper(): c for c in self.composites}

    # ---- packets ----
    def packet_bytes(self, g):
        self.f.seek(g['offset'])
        return self.f.read(g['length'])

    def packet_files(self, g, data=None):
        """[(path, dict(type, offset, size[, ptx, w, h]))] of a packet (types: 0 plain file, 1 texture)."""
        E = self.E
        if data is None:
            self.f.seek(g['offset'])
            data = self.f.read(g['header_len'])
        v = struct.unpack_from(E + '22I', data, 4)

        def val(buf, pos):
            typ, off, size = struct.unpack_from(E + 'III', buf, pos)
            pos += 12
            d = dict(type=typ, offset=off, size=size)
            if typ == 1:
                ptx, _z0, _z1, w, h = struct.unpack_from(E + '5I', buf, pos)
                pos += 20
                d.update(ptx=ptx, w=w, h=h)
            return d, pos
        return parse_trie(data, v[18], v[17], E, val)

    def part(self, g, which):
        """The whole of part 0 (plain files) or part 1 (textures, decompressed) of a packet."""
        off, zs, sz = (g['part0_off'], g['part0_zsize'], g['part0_size']) if which == 0 else \
            (g['part1_off'], g['part1_zsize'], g['part1_size'])
        if not sz:
            return b''
        self.f.seek(g['offset'] + off)
        raw = self.f.read(zs)
        if zs != sz or raw[:1] == b'\x78':
            try:
                return zlib.decompress(raw)
            except zlib.error:
                pass
        return raw

    def read(self, group_name, path):
        """One file by packet name and path (None if it is not there)."""
        g = self.by_name[group_name.upper()]
        for name, e in self.packet_files(g):
            if name.upper() == path.upper():
                return self.part(g, e['type'])[e['offset']:e['offset'] + e['size']]
        return None


def build_packet(E, template_header, files, textures, first_ptx_slot=0):
    """A packet's bytes. files: [(path, bytes)] plain files; textures: [(path, bytes, width, height)] in the order
    of their texture index inside the packet (0, 1, ...). template_header: the 0x5C header bytes of an existing
    packet of the same kind (its magic, version and flags are kept). Returns (bytes, fields for the packet record)."""
    entries = []
    part0 = bytearray()
    for path, data in files:
        entries.append((path.upper(), struct.pack(E + '3I', 0, len(part0), len(data))))
        part0 += data
        part0 += b'\0' * (pad(len(part0)) - len(part0))
    part1 = bytearray()
    for i, (path, data, w, h) in enumerate(textures):
        entries.append((path.upper(), struct.pack(E + '8I', 1, len(part1), len(data), i, 0, 0, w, h)))
        part1 += data
        part1 += b'\0' * (pad(len(part1)) - len(part1))
    trie = build_trie(entries, E, lambda b: b)
    header_len = pad(0x5C + len(trie))
    z1 = zlib.compress(bytes(part1), 9) if part1 else b''
    z1 += b'\0' * (pad(len(z1)) - len(z1))
    v = list(struct.unpack_from(E + '22I', template_header, 4))
    v[4] = header_len
    v[5], v[6], v[7], v[8] = header_len, len(part0), len(part0), 0
    v[9], v[10], v[11] = header_len + len(part0), len(z1), len(part1)
    v[17], v[18] = len(trie), 0x5C
    head = template_header[:4] + struct.pack(E + '22I', *v) + trie
    head += b'\0' * (header_len - len(head))
    data = head + bytes(part0) + z1
    rec = dict(length=len(data), flags=v[3], header_len=header_len, part0_off=v[5], part0_zsize=v[6],
               part0_size=v[7], unk=len(part0), part1_off=v[9], part1_zsize=v[10], part1_size=v[11], r0=0, r1=0, r2=0, r3=0,
               r4=0, ptx_count=len(textures))
    return data, rec


class Writer:
    """Builds a new archive from an existing one: existing packets are copied as they are unless replaced."""

    def __init__(self, base):
        self.base = base
        self.E = base.E
        # Each packet: dict(name, record fields, data: bytes or None (copy from the base), files: [(path, index)]).
        self.packets = []
        for g in base.rsgs:
            self.packets.append(dict(name=g['name'], rec=dict(g), data=None, base=g))
        self.files = {k: v for k, v in base.file_list}  # path -> packet index
        self.composites = [dict(name=c['name'], subs=list(c['subs'])) for c in base.composites]
        self.ptx = list(base.ptx)  # the new packets' textures are appended
        self.pool_tail = {}  # packet index -> the pool record's values

    def template(self, kind):
        """The header of an existing packet with only plain files (kind 0) or only textures (kind 1)."""
        for g in self.base.rsgs:
            if (kind == 0 and g['part1_size'] == 0 and g['part0_size']) or \
                    (kind == 1 and g['part0_size'] == 0 and g['part1_size']):
                self.base.f.seek(g['offset'])
                return self.base.f.read(0x5C)
        raise ValueError(kind)

    def replace_files(self, group_name, changes):
        """Replaces (or adds) plain files of an existing packet without textures. changes: {path: bytes}."""
        b = self.base
        index = b.by_name[group_name.upper()]['index']
        p = self.packets[index]
        g = p['base']
        assert g['part1_size'] == 0, 'a packet with textures is not rebuilt'
        old = b.packet_files(g)
        part0 = b.part(g, 0)
        files = []
        seen = set()
        up = {k.upper(): v for k, v in changes.items()}
        for name, e in sorted(old, key=lambda x: x[1]['offset']):  # the files keep their order in the packet
            data = up.get(name.upper())
            if data is None:
                data = part0[e['offset']:e['offset'] + e['size']]
            seen.add(name.upper())
            files.append((name, data))
        for k, v in up.items():
            if k not in seen:
                files.append((k, v))
                self.files[k] = index
        b.f.seek(g['offset'])
        data, rec = build_packet(self.E, b.f.read(0x5C), files, [])
        rec.update(pool=g['pool'], ptx_before=g['ptx_before'], ptx_count=0)
        p['data'], p['rec'] = data, dict(p['rec'], **rec)

    def add_packet(self, name, files=(), textures=(), tex_format=5):
        """A new packet. textures: [(path, bytes, width, height)]. Returns its index."""
        index = len(self.packets)
        ptx_before = len(self.ptx)
        data, rec = build_packet(self.E, self.template(1 if textures else 0), list(files), list(textures))
        rec.update(pool=index, ptx_before=ptx_before)
        for path, tex, w, h in textures:
            self.ptx.append((w, h, w, tex_format))  # width, height, pitch, format
        self.packets.append(dict(name=name, rec=rec, data=data, base=None))
        for path, _ in files:
            self.files[path.upper()] = index
        for path, *_ in textures:
            self.files[path.upper()] = index
        return index

    def comp_index(self, name):
        """The index of a composite in self.composites (its `subs` list can be appended to)."""
        return next(i for i, c in enumerate(self.composites) if c['name'].upper() == name.upper())

    def add_composite(self, name, subs):
        """subs: [(packet index, resolution or 0, language or '')]."""
        self.composites.append(dict(name=name, subs=list(subs)))

    def write(self, out_path):
        b, E = self.base, self.E
        H = dict(b.h)
        # Texture counts before each packet follow the packet order; existing packets keep theirs (the new
        # textures are appended after all existing ones).
        file_map = build_trie(list(self.files.items()), E, lambda v: struct.pack(E + 'I', v))
        rsg_map = build_trie([(p['name'].upper(), i) for i, p in enumerate(self.packets)], E,
                             lambda v: struct.pack(E + 'I', v))
        comp_map = build_trie([(c['name'].upper(), i) for i, c in enumerate(self.composites)], E,
                              lambda v: struct.pack(E + 'I', v))
        comp_info = bytearray()
        for c in self.composites:
            rec = bytearray(H['comp_info_size'])
            nm = c['name'].encode('latin-1')
            rec[:len(nm)] = nm
            assert len(c['subs']) <= 64
            for k, (idx, res, lang) in enumerate(c['subs']):
                lb = lang.encode('latin-1').ljust(4, b'\0')
                struct.pack_into(E + 'II4sI', rec, 128 + k * 16, idx, res, lb[::-1] if E == '<' else lb, 0)
            struct.pack_into(E + 'I', rec, 128 + 1024, len(c['subs']))
            comp_info += rec
        ptx_info = b''.join(struct.pack(E + '4I', *t) for t in self.ptx)
        n = len(self.packets)
        sizes = [('file_list', len(file_map)), ('rsg_list', len(rsg_map)), ('comp_info', len(comp_info)),
                 ('comp_list', len(comp_map)), ('rsg_info', n * H['rsg_info_size']),
                 ('pool_info', n * H['pool_info_size']), ('ptx_info', len(ptx_info))]
        pos = 0x70
        begins = {}
        for name, size in sizes:
            begins[name] = pos
            pos += size
        tables_end = pad(pos)
        # The three description tables of the base archive, moved as a block (their offsets are relative fields).
        desc = b.head[b.h['header_end']:b.h['file_offset']]
        shift = tables_end - b.h['header_end']
        file_offset = pad(tables_end + len(desc))
        # Packet offsets.
        offset = file_offset
        rsg_info = bytearray()
        pool_info = bytearray()
        for i, p in enumerate(self.packets):
            rec = dict(p['rec'])
            rec['offset'] = offset
            p['new_offset'] = offset
            offset += rec['length']
            r = bytearray(H['rsg_info_size'])
            nm = p['name'].encode('latin-1')
            r[:len(nm)] = nm
            struct.pack_into(E + '19I', r, 128, *[rec[f] for f in RSG_FIELDS])
            rsg_info += r
            pr = bytearray(H['pool_info_size'])
            pn = (p['name'] + '_AutoPool').encode('latin-1')
            pr[:len(pn)] = pn
            if p['base'] is not None and p['data'] is None:
                vals = b.pools[p['base']['index']]['vals']
            else:
                # As the existing records: the packet's size without part 1, the unpacked size of part 1, 1.
                vals = (rec['part1_off'], rec['part1_size'], 1, 0, 0, 0)
            struct.pack_into(E + '6I', pr, 128, *vals)
            pool_info += pr
        H.update(file_offset=file_offset, file_list_len=len(file_map), file_list_begin=begins['file_list'],
                 rsg_list_len=len(rsg_map), rsg_list_begin=begins['rsg_list'], rsg_count=n,
                 rsg_info_begin=begins['rsg_info'], comp_count=len(self.composites),
                 comp_info_begin=begins['comp_info'], comp_list_len=len(comp_map),
                 comp_list_begin=begins['comp_list'], pool_count=n, pool_info_begin=begins['pool_info'],
                 ptx_count=len(self.ptx), ptx_info_begin=begins['ptx_info'], header_end=tables_end,
                 part1_begin=b.h['part1_begin'] + shift, part2_begin=b.h['part2_begin'] + shift,
                 part3_begin=b.h['part3_begin'] + shift)
        head = bytearray(b.magic + struct.pack(E + '27I', *[H[f] for f in HEADER_FIELDS]))
        head += file_map + rsg_map + comp_info + comp_map + rsg_info + pool_info + ptx_info
        head += b'\0' * (tables_end - len(head))
        head += desc
        head += b'\0' * (file_offset - len(head))
        with open(out_path, 'wb') as out:
            out.write(head)
            for p in self.packets:
                assert out.tell() == p['new_offset']
                if p['data'] is not None:
                    out.write(p['data'])
                else:
                    g = p['base']
                    b.f.seek(g['offset'])
                    left = g['length']
                    while left:
                        chunk = b.f.read(min(left, 1 << 24))
                        out.write(chunk)
                        left -= len(chunk)
        return offset
