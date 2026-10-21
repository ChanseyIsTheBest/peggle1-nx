"""Sound of the PS4 content for the Xbox 360 build (build_mod.py --content ...,sound).

What the two builds are (docs/peggle1-notes.md, "PS4 content in the Xbox 360 build", sound):
  - every sound object is in GLOBAL_DATA.BNK; the other banks only carry media. The PS4's bank is merged into the
    360's (wwise_merge.py), so the events, voices and containers of the new content exist;
  - the PS4's media are ATRAC9, which the 360 executable's Wwise cannot play, and there is no XMA2 encoder on this PC.
    The 360 executable's Wwise does have its PCM source built in (codec id 1: sub_8252FC18 creates it for a bank's
    and for a streamed file's media), so the media the merged objects need are decoded with the installed ffmpeg
    (at9.py) and stored as 16-bit PCM in the 360's byte order.
Which media: those of the sound objects (voices, effects, ambience). Music is left out (PCM of the two new
masters' music alone would be several hundred megabytes of bank that the game loads into memory): the merged bank
has the 360's own music objects only, so the music that plays today keeps playing.

Where they go, as on the PS4: a medium of the PS4's X.BNK goes into the 360's X.BNK (appended to its media; the two
banks the 360 never had, JIMMY.BNK and DESERT.BNK, and Windy's area bank are made new, each in a group of its own), a
streamed one becomes a file of StreamingWaves.
"""
import array
import concurrent.futures
import os
import struct

import at9
import audio_index
import rton
import wwise
import wwise_merge

BS = chr(92)
RATE = 24000   # most of the 360's own media are 24 kHz
# A medium larger than this as PCM (8 s of mono) becomes a streamed file even when the PS4 keeps it in a bank: the
# long ones (area ambience loops of up to 13 MB, long voice lines) would otherwise sit in the banks' memory, which the
# 360 build fills with XMA a tenth the size (Desert.bnk: 46.9 MB, 32.9 of them in five loops).
STREAM_OVER = 400 * 1024
ALIGN = 16
# PS4 bank file -> (the 360 group, its file, manifest id); None group = a new group of that name.
BANKS = {
    'FAERIE.BNK': ('WendySounds', None),
    # The two area banks get groups named as the stock areas' are (X_COMMON, as on the PS4): a level whose area
    # names the 360's own FirefieldSoundBank group crashes while loading (read of guest 0x1C, the loader's null entry
    # for a group it does not find). The 360's FirefieldSoundBank group and its empty bank are left as they are.
    'FIREFIELD.BNK': ('FAIRYAREA_COMMON', ('SOUNDBANK_FAIRYAREA', 'FairyArea.bnk', 1647262551)),
    'GLOBAL.BNK': ('GlobalSoundBank', None),
    'BERG.BNK': ('BergSounds', None),
    'BJORN.BNK': ('BjornSounds', None),
    'GNOME.BNK': ('GnormanSounds', None),
    'LORT.BNK': ('JeffSounds', None),
    'ULNA.BNK': ('LunaSounds', None),
    'JIMMY.BNK': ('JimmySounds', ('SOUNDBANK_JIMMY', 'Jimmy.bnk', 2836931289)),
    'DESERT.BNK': ('DESERT_COMMON', ('SOUNDBANK_DESERT', 'Desert.bnk', 1850388778)),
}
MASKS = {1: 0x4, 2: 0x3, 4: 0x33, 6: 0x3F}


def pcm_wem(channels, rate, pcm_le):
    """A PCM .wem as the 360 build's Wwise reads it: the PS4 bank's own PCM media (RIFF, format tag FFFE, a 24-byte
    fmt chunk, a 4-byte JUNK chunk, data) with every field and sample in the other byte order."""
    samples = array.array('h')
    samples.frombytes(pcm_le[:len(pcm_le) // (2 * channels) * 2 * channels])
    samples.byteswap()
    data = samples.tobytes()
    fmt = struct.pack('>HHIIHHHHI', 0xFFFE, channels, rate, rate * channels * 2, channels * 2, 16, 6, 0,
                      MASKS[channels])
    body = b'WAVE' + b'fmt ' + struct.pack('>I', len(fmt)) + fmt + b'JUNK' + struct.pack('>I', 4) + bytes(4) + \
        b'data' + struct.pack('>I', len(data)) + data
    return b'RIFX' + struct.pack('>I', len(body)) + body


def with_media(base, items):
    """A big-endian media bank with `items` [(source id, .wem bytes)] added after its own media.
    Returns (bank, {source id: (offset in the file, size)}, how far its own media moved)."""
    E, ch = wwise.chunks(base)
    assert E == '>'
    parts = {name: base[at:at + size] for name, at, size in ch}
    assert set(parts) <= {'BKHD', 'DIDX', 'DATA'}, list(parts)
    old_start = next((at for name, at, size in ch if name == 'DATA'), None)
    index = [struct.unpack_from('>III', parts.get('DIDX', b''), 12 * i) for i in range(len(parts.get('DIDX', b'')) // 12)]
    data = bytearray(parts.get('DATA', b''))
    new = {}
    for sid, wem in items:
        data += bytes(-len(data) % ALIGN)
        new[sid] = (len(data), len(wem))
        index.append((sid, len(data), len(wem)))
        data += wem
    didx = b''.join(struct.pack('>III', *e) for e in index)
    # The header chunk is padded so that the data starts on a 2048-byte boundary, as in the 360's banks; a bank that
    # had media keeps the padding it needs to move them by whole blocks.
    header = parts['BKHD'][:20]
    start = 8 + len(header) + 8 + len(didx) + 8
    want = -start % 2048
    if old_start is not None:
        while start + want < old_start:
            want += 2048
    header += bytes(want)
    start += want
    out = b'BKHD' + struct.pack('>I', len(header)) + header + b'DIDX' + struct.pack('>I', len(didx)) + didx + \
        b'DATA' + struct.pack('>I', len(data)) + bytes(data)
    assert out[start - 8:start - 4] == b'DATA'
    return out, {sid: (start + off, size) for sid, (off, size) in new.items()}, \
        (start - old_start) if old_start is not None else 0


def add(mod):
    x, p = mod.x360, mod.ps4
    xgd = x.read('GlobalDataSoundBank', 'SOUNDBANKS' + BS + 'GLOBAL_DATA.BNK')
    pgd = p.read('LoadingThread_Common', 'SOUNDBANKS' + BS + 'GLOBAL_DATA.BNK')
    cache = os.path.join(mod.a.cache, 'sound')
    os.makedirs(cache, exist_ok=True)
    # 1. The sources the merged bank's converted sound objects need.
    wanted = {}

    def ask(sid, stream, bits, kind):
        if kind == 2:
            wanted.setdefault(sid, (stream, bits))
        return None
    # The PS4's Init bank has busses the 360's lacks: what uses them goes to the nearest bus the 360 has.
    busses, init_ids = wwise_merge.bus_fallbacks(x.read('Init_Common', 'SOUNDBANKS' + BS + 'INIT.BNK'),
                                                 p.read('Init_Common', 'SOUNDBANKS' + BS + 'INIT.BNK'))
    wwise_merge.merge(xgd, pgd, media=ask, log=lambda s: None, busses=busses, init_ids=init_ids)
    # 2. Where the PS4 has them.
    rows = audio_index.load(p, os.path.join(cache, 'ps4_media.tsv'))
    where = {}
    for row in rows:
        sid, kind = row[0], row[1]
        if sid in wanted and row[6] == 0xFFFC:
            streamed = wanted[sid][0] != 0
            if sid not in where or (kind == 'stream') == streamed:
                where[sid] = row
    missing = sorted(set(wanted) - set(where))
    # 3. Decode and store as PCM (kept in the cache folder: <source id>_<rate>.wem).
    todo = {}
    for sid, row in where.items():
        if not os.path.exists(os.path.join(cache, '%d_%d.wem' % (sid, RATE))):
            todo.setdefault(row[2], []).append(row)
    for group, group_rows in todo.items():
        part = p.part(p.by_name[group.upper()], 0)

        def convert(row):
            sid, _kind, _group, _file, off, size = row[:6]
            channels, rate, pcm = at9.decode(part[off:off + size], RATE)
            tmp = os.path.join(cache, '%d_%d.tmp' % (sid, RATE))
            with open(tmp, 'wb') as f:
                f.write(pcm_wem(channels, rate, pcm))
            os.replace(tmp, os.path.join(cache, '%d_%d.wem' % (sid, RATE)))
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            list(pool.map(convert, group_rows))
    # 4. The banks.
    per_bank, streams = {}, {}
    for sid, row in where.items():
        with open(os.path.join(cache, '%d_%d.wem' % (sid, RATE)), 'rb') as f:
            wem = f.read()
        if wanted[sid][0] != 0 or len(wem) > STREAM_OVER:
            streams[sid] = wem
        else:
            per_bank.setdefault(os.path.basename(row[3].replace(BS, '/')).upper(), []).append((sid, wem))
    records, relocate, total = {}, {}, 0
    template = x.read('WendySounds', 'SOUNDBANKS' + BS + 'FAERIE.BNK')
    wendy = next(g for g in mod.manifest['groups'] if g['id'].s == 'WendySounds')
    for bank, items in sorted(per_bank.items()):
        group, new = BANKS[bank]
        path = 'SOUNDBANKS' + BS + (new[1].upper() if new else bank)
        if new is None:
            base = x.read(group, path)
        else:  # a bank the 360 never had: the header of its (empty) FAERIE.BNK with this bank's id
            base = bytearray(template)
            struct.pack_into('>I', base, 12, new[2])
            base = bytes(base)
        bank_id = struct.unpack_from('>I', base, 12)[0]
        data, placed, shift = with_media(base, sorted(items))
        if shift:
            relocate[bank_id] = shift
        for sid, (off, size) in placed.items():
            records[sid] = wwise_merge.source_record(wwise_merge.PCM, 0, sid, bank_id, off, size, wanted[sid][1])
        total += len(data) - len(base)
        if new is None:
            mod.w.replace_files(group, {path: data})
        else:
            index = mod.w.add_packet(group, files=[(path, data)])
            mod.w.add_composite(group + '_CompositeShell', [(index, 0, '')])
            g = rton.clone(wendy)
            g.set('id', rton.Str(0x90, group))
            e = g['resources'][0]
            e.set('id', rton.Str(0x90, new[0]))
            e.set('path', [rton.Str(0x90, 'soundbanks'), rton.Str(0x90, new[1])])
            mod.slots[new[0]] = mod.next_slot
            e.set('slot', rton.Num(0x24, mod.next_slot))
            mod.next_slot += 1
            mod.manifest['groups'].append(g)
            mod.have.add(group)
        mod.log.append(f'sound: {bank} +{len(items)} media, {len(data)} bytes'
                       f'{" (new group " + group + ")" if new else ""}{", its own media moved by " + str(shift) if shift else ""}')
    if streams:
        mod.add_files('StreamingWaves', {'streamingwaves' + BS + '%d.wem' % sid: wem for sid, wem in streams.items()})
        for sid in streams:
            records[sid] = wwise_merge.source_record(wwise_merge.PCM, 1, sid, sid, bits=wanted[sid][1])
        total += sum(len(w) for w in streams.values())
        mod.log.append(f'sound: {len(streams)} streamed files, {sum(len(w) for w in streams.values())} bytes')
    # 5. The structure bank, pointing at them.
    lines = []
    merged, silent = wwise_merge.merge(xgd, pgd, media=lambda sid, stream, bits, kind: records.get(sid),
                                       relocate=relocate, log=lines.append, busses=busses, init_ids=init_ids)
    twice, absent, other, late, n = wwise_merge.tree_report(merged)
    assert (twice, absent, other, late) == (0, 0, 0, 0), (twice, absent, other, late)
    mod.w.replace_files('GlobalDataSoundBank', {'SOUNDBANKS' + BS + 'GLOBAL_DATA.BNK': merged})
    mod.sound_bank = merged  # music.py merges the music into this one
    mod.log.append(f'sound: GLOBAL_DATA.BNK {len(xgd)} -> {len(merged)} bytes; {len(records)} media as PCM at {RATE} Hz '
                   f'({total} bytes); {len(silent)} sources without media (music tracks: not converted)'
                   f'{"; NOT FOUND in the PS4 archive: " + str(missing[:5]) if missing else ""}')
    mod.log += ['sound:' + line for line in lines]
