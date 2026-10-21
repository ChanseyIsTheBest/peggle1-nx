"""Prints the headers of an XEX2 file (or an XEXP title-update patch): base address, entry point, image size,
original PE name, imported modules and the static libraries the game was linked with (the XDK version).

Usage: python tools/xex_info.py <default.xex | default.xexp>
"""
import struct
import sys

KEYS = {
    0x000002FF: 'resource_info',
    0x000003FF: 'file_format_info',
    0x000005FF: 'delta_patch_descriptor',
    0x00010001: 'image_base_address',
    0x00010100: 'entry_point',
    0x00010201: 'original_base_address',
    0x000103FF: 'import_libraries',
    0x000183FF: 'original_pe_name',
    0x000200FF: 'static_libraries',
    0x00020104: 'default_stack_size',
    0x00020200: 'default_filesystem_cache_size',
    0x00020301: 'default_heap_size',
    0x00030000: 'system_flags',
    0x00040006: 'execution_info',
    0x00040201: 'title_workspace_size',
    0x00040310: 'game_ratings',
    0x00040404: 'lan_key',
    0x000405FF: 'xbox360_logo',
    0x000406FF: 'multidisc_media_ids',
    0x000407FF: 'alternate_title_ids',
    0x00040801: 'additional_title_memory',
    0x00E10402: 'exports_by_name',
}


def main(path):
    d = open(path, 'rb').read()
    magic, flags, pe_off, _, sec_off, nhdr = struct.unpack_from('>4sIIIII', d, 0)
    print(f'{path}: magic {magic} flags {flags:#x} pe_data {pe_off:#x} security {sec_off:#x} headers {nhdr}')
    if magic != b'XEX2':
        return
    for i in range(nhdr):
        key, val = struct.unpack_from('>II', d, 0x18 + i * 8)
        name = KEYS.get(key, f'{key:#010x}')
        size = key & 0xFF
        if size == 0 or size == 1:
            print(f'  {name}: {val:#x}')
            continue
        if size == 0xFF:
            length = struct.unpack_from('>I', d, val)[0]
            body = d[val + 4:val + length]
        else:
            body = d[val:val + size * 4]
        if name == 'original_pe_name':
            print(f'  {name}: {body.split(b"\0")[0].decode()}')
        elif name == 'static_libraries':
            for o in range(0, len(body), 16):
                lib = body[o:o + 8].split(b'\0')[0].decode()
                ma, mi, bu, qf = struct.unpack_from('>HHHH', body, o + 8)
                print(f'  static {lib:10s} {ma}.{mi}.{bu}.{qf & 0xFF} approval {qf >> 13}')
        elif name == 'import_libraries':
            str_size, count = struct.unpack_from('>II', body, 0)
            names = body[8:8 + str_size].split(b'\0')
            names = [n.decode() for n in names if n]
            o = 8 + str_size
            for _ in range(count):
                lsize = struct.unpack_from('>I', body, o)[0]
                ver, ver_min = struct.unpack_from('>II', body, o + 0x1C)
                name_idx, nrec = struct.unpack_from('>HH', body, o + 0x24)
                vs = lambda v: f'{v >> 28}.{(v >> 24) & 15}.{(v >> 8) & 0xFFFF}.{v & 0xFF}'
                print(f'  import {names[name_idx]:14s} version {vs(ver)} min {vs(ver_min)} records {nrec}')
                o += lsize
        elif name == 'execution_info':
            mid, ver, base_ver, tid = struct.unpack_from('>IIII', body, 0)
            plat, et, dn, dc = struct.unpack_from('>BBBB', body, 16)
            vs = lambda v: f'{v >> 28}.{(v >> 24) & 15}.{(v >> 8) & 0xFFFF}.{v & 0xFF}'
            print(f'  execution: media {mid:08x} version {vs(ver)} base {vs(base_ver)} title {tid:08x} disc {dn}/{dc}')
        elif name == 'delta_patch_descriptor':
            size_, tv, sv = struct.unpack_from('>III', body, 0)
            vs = lambda v: f'{v >> 28}.{(v >> 24) & 15}.{(v >> 8) & 0xFFFF}.{v & 0xFF}'
            print(f'  delta patch: target version {vs(tv)} source version {vs(sv)} digest_source {body[12:32].hex()}')
            print(f'    image_key_source {body[32:48].hex()} size_of_target_headers {struct.unpack_from(">I", body, 48)[0]:#x}')
        elif name == 'file_format_info':
            enc, comp = struct.unpack_from('>HH', body, 0)
            print(f'  {name}: encryption {enc} compression {comp} ({len(body)} bytes)')
        else:
            print(f'  {name}: {body[:32].hex()}{"..." if len(body) > 32 else ""} ({len(body)} bytes)')
    # Security info: image size and base address.
    hdr_size, image_size = struct.unpack_from('>II', d, sec_off)
    load_addr = struct.unpack_from('>I', d, sec_off + 0x110)[0]
    print(f'  security: image size {image_size:#x} load address {load_addr:#x} sha1 of rsa {d[sec_off + 8:sec_off + 8 + 0x100][:0].hex()}')


if __name__ == '__main__':
    for p in sys.argv[1:]:
        main(p)
