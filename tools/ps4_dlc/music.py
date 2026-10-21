"""Music of the PS4 content for the Xbox 360 build (build_mod.py --content ...,music).

The music is one tree in GLOBAL_DATA.BNK. Five switch containers in it pick a master's branch by a state named after
the master (Bjorn, Lort, Berg, Gnorman, Ulna, Fairy; the PS4's also Jimmy). The 360's tree has no branch for Fairy
(Windy) or Jimmy: Windy's levels are silent (the "Play_Music_Set_F" of MASTERS.JSON only starts the music; the six
set events are the same two actions) and Jimmy's fall to another branch. The PS4's bank has both branches: Windy's
444 tracks on 218 media of its FAERIE.BNK (3,811 s), Jimmy's 288 tracks on 101 media of JIMMY.BNK and 14 of
GLOBAL.BNK (915 s), all ATRAC9. The 360 build's own FAERIE.BNK is empty and it has no JIMMY.BNK.

What this adds:
  - GLOBAL_DATA.BNK: the 360's objects with the PS4's two branches (wwise_merge.merge with sounds=False,
    music=...): the PS4's version of the five switch containers, everything under their two new children, the states
    those hold. Every other object, and every track the 360 has, keeps the 360's bytes, so the music of the five
    masters of the 360 game is what it was. A medium counts as the new branches' own when on the PS4 it is in no
    bank but FAERIE.BNK / JIMMY.BNK (a few media of those banks are copies of shared ones: a track under another
    master's playlist that names one would pull the PS4's version of that playlist in);
  - every medium the brought tracks name, decoded with the installed ffmpeg (at9.py) and stored as 16-bit PCM in the
    360's byte order (the 360 executable's Wwise has its PCM source built in; no XMA2 encoder is on this PC), as
    STREAMED files of StreamingWaves: as PCM in a bank they would be several hundred megabytes of memory. Their
    tracks get LOOK_AHEAD ms of look-ahead, as the 360's own streamed music tracks have.

With --content sound as well, run sound first: this module then keeps the bank it made for all that is not music.
"""
import concurrent.futures
import os

import at9
import audio_index
import sound
import wwise_merge

BS = chr(92)
RATE = 32000        # the 360's own music media are 24, 32 and 48 kHz
LOOK_AHEAD = 300    # ms
BANKS = ('FAERIE.BNK', 'JIMMY.BNK')


def add(mod):
    x, p = mod.x360, mod.ps4
    xgd = mod.sound_bank if getattr(mod, 'sound_bank', None) else \
        x.read('GlobalDataSoundBank', 'SOUNDBANKS' + BS + 'GLOBAL_DATA.BNK')
    pgd = p.read('LoadingThread_Common', 'SOUNDBANKS' + BS + 'GLOBAL_DATA.BNK')
    cache = os.path.join(mod.a.cache, 'sound')
    os.makedirs(cache, exist_ok=True)
    # The PS4's ATRAC9 media: where each is (any place will do to decode it), and those only the wanted banks hold.
    rows, places = {}, {}
    for row in audio_index.load(p, os.path.join(cache, 'ps4_media.tsv')):
        if row[6] == 0xFFFC:
            rows.setdefault(row[0], row)
            places.setdefault(row[0], set()).add(os.path.basename(row[3].replace(BS, '/')).upper())
    own = {sid for sid, where in places.items() if where <= set(BANKS)}
    busses, init_ids = wwise_merge.bus_fallbacks(x.read('Init_Common', 'SOUNDBANKS' + BS + 'INIT.BNK'),
                                                 p.read('Init_Common', 'SOUNDBANKS' + BS + 'INIT.BNK'))
    # 1. The media the merged music's converted tracks name.
    wanted = {}

    def ask(sid, stream, bits, kind):
        if kind == 11 and sid in rows:
            wanted[sid] = bits
        return None
    wwise_merge.merge(xgd, pgd, media=ask, log=lambda s: None, busses=busses, init_ids=init_ids, sounds=False,
                      music=lambda sid: sid in own)
    # 2. Decode and store as PCM (kept in the cache folder: <source id>_<rate>.wem).
    todo = {}
    for sid in wanted:
        if not os.path.exists(os.path.join(cache, '%d_%d.wem' % (sid, RATE))):
            todo.setdefault(rows[sid][2], []).append(rows[sid])
    for group, group_rows in todo.items():
        part = p.part(p.by_name[group.upper()], 0)

        def convert(row):
            sid, _kind, _group, _file, off, size = row[:6]
            channels, rate, pcm = at9.decode(part[off:off + size], RATE)
            tmp = os.path.join(cache, '%d_%d.tmp' % (sid, RATE))
            with open(tmp, 'wb') as f:
                f.write(sound.pcm_wem(channels, rate, pcm))
            os.replace(tmp, os.path.join(cache, '%d_%d.wem' % (sid, RATE)))
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            list(pool.map(convert, group_rows))
    # 3. The streamed files and the records that name them.
    streams, records, per_bank = {}, {}, {}
    for sid, bits in wanted.items():
        with open(os.path.join(cache, '%d_%d.wem' % (sid, RATE)), 'rb') as f:
            streams[sid] = f.read()
        records[sid] = wwise_merge.source_record(wwise_merge.PCM, 1, sid, sid, bits=bits)
        bank = sorted(places[sid])[0]
        per_bank[bank] = per_bank.get(bank, 0) + 1
    mod.add_files('StreamingWaves', {'streamingwaves' + BS + '%d.wem' % sid: wem for sid, wem in streams.items()})
    # 4. The structure bank.
    lines = []
    merged, silent = wwise_merge.merge(xgd, pgd, media=lambda sid, stream, bits, kind: records.get(sid),
                                       log=lines.append, busses=busses, init_ids=init_ids, sounds=False,
                                       music=lambda sid: sid in own, look_ahead=LOOK_AHEAD)
    twice, absent, other, late, n = wwise_merge.tree_report(merged)
    assert (twice, absent, other, late) == (0, 0, 0, 0), (twice, absent, other, late)
    mod.w.replace_files('GlobalDataSoundBank', {'SOUNDBANKS' + BS + 'GLOBAL_DATA.BNK': merged})
    mod.log.append(f'music: GLOBAL_DATA.BNK {len(xgd)} -> {len(merged)} bytes, {n} objects; {len(streams)} media '
                   f'({", ".join("%s %d" % kv for kv in sorted(per_bank.items()))}) as streamed PCM at {RATE} Hz '
                   f'({sum(len(w) for w in streams.values())} bytes), look-ahead {LOOK_AHEAD} ms; {len(silent)} '
                   f'sources of converted objects without media')
    mod.log += ['music:' + line for line in lines]
