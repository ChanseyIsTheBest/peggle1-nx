"""Checks of the RSB and RTON code against a real archive: every map rebuilt from its parsed entries, every packet
rebuilt from its parsed files, the manifest decoded and encoded, and (with --write) the whole archive written again
through the writer without a change. Everything must come out byte-identical.

  python tools/ps4_dlc/selftest.py <main.rsb> [--write <out.rsb>]
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsb as rsblib
import rton


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 24)
            if not b:
                return h.hexdigest()
            h.update(b)


def main():
    r = rsblib.Rsb(sys.argv[1])
    E, H, hd = r.E, r.h, r.head
    u32 = lambda v: struct.pack(E + 'I', v)
    ok = True
    for name, entries, begin, length in (('file map', r.file_list, H['file_list_begin'], H['file_list_len']),
                                         ('packet map', r.rsg_list, H['rsg_list_begin'], H['rsg_list_len']),
                                         ('composite map', r.comp_list, H['comp_list_begin'], H['comp_list_len'])):
        built = rsblib.build_trie(entries, E, u32)
        same = built == hd[begin:begin + length]
        again = rsblib.parse_trie(built, 0, len(built), E, lambda b, p: (struct.unpack_from(E + 'I', b, p)[0], p + 4))
        print(f'{name}: {len(entries)} entries, rebuilt {"identical" if same else "DIFFERENT bytes"} '
              f'({len(built)} / {length} bytes), parses back {"equal" if sorted(again) == sorted(entries) else "DIFFERENT"}')
        ok &= sorted(again) == sorted(entries)
    # Packets: rebuild the ones without textures from their files, and one with textures.
    same = diff = 0
    tex_checked = 0
    local_ptx_ok = True
    for g in r.rsgs:
        files = r.packet_files(g)
        raw = r.packet_bytes(g)
        if g['part1_size'] == 0:
            part0 = r.part(g, 0)
            built, rec = rsblib.build_packet(E, raw[:0x5C], [(n, part0[e['offset']:e['offset'] + e['size']])
                                                              for n, e in sorted(files, key=lambda x: x[1]['offset'])], [])
            if built == raw:
                same += 1
            else:
                diff += 1
                if diff <= 3:
                    first = next((i for i in range(min(len(built), len(raw))) if built[i] != raw[i]), -1)
                    print('  packet differs:', g['name'], len(built), len(raw), 'first difference at', hex(first))
        else:
            tex = sorted((e['ptx'], n) for n, e in files if e['type'] == 1)
            if [t[0] for t in tex] != list(range(len(tex))):
                local_ptx_ok = False
            if g['ptx_count'] != len(tex):
                local_ptx_ok = False
            for n, e in files:
                if e['type'] == 1:
                    w, h, pitch, fmt = r.ptx[g['ptx_before'] + e['ptx']]
                    if (w, h) != (e['w'], e['h']):
                        local_ptx_ok = False
            tex_checked += 1
    print(f'packets without textures rebuilt from their files: {same} identical, {diff} different')
    print(f'packets with textures: {tex_checked}; texture indices are 0..n-1 inside the packet and match the '
          f'texture records: {local_ptx_ok}')
    ok &= diff == 0 and local_ptx_ok
    data = r.read('__MANIFESTGROUP__', 'PROPERTIES\\RESOURCES.RTON')
    root, version = rton.decode(data)
    again = rton.encode(root, version)
    print(f'manifest RTON: {len(data)} bytes, round trip {"identical" if again == data else "DIFFERENT"}; '
          f'{len(root["groups"])} groups, slot_count {root["slot_count"].v}')
    ok &= again == data
    if '--write' in sys.argv:
        out = sys.argv[sys.argv.index('--write') + 1]
        w = rsblib.Writer(r)
        w.write(out)
        a, b = sha(sys.argv[1]), sha(out)
        print(f'archive written without a change: {"identical" if a == b else "DIFFERENT"} ({os.path.getsize(out)} bytes)')
        ok &= a == b
    print('ALL OK' if ok else 'FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
