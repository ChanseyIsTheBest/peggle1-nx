"""Decodes the PS4 build's sound media (Wwise .wem with ATRAC9, format tag 0xFFFC) to PCM with the ffmpeg already on
this PC: the Wwise header is rewritten as Sony's own .at9 header (format tag 0xFFFE with the ATRAC9 GUID, a `fact`
chunk), which ffmpeg's wav reader and ATRAC9 decoder take; the frames are not touched.

  decode(wem bytes, rate) -> (channels, rate, samples as bytes of signed 16-bit little-endian, interleaved)

The Wwise header: fmt = tag FFFC, channels, rate, bytes per second, block size, 0 bits; 18 extra bytes: samples per
block (u16), channel mask (u32), the 4 ATRAC9 configuration bytes (FE ..), total samples (u32), encoder delay (u16),
u16. Then the data chunk.
"""
import os
import struct
import subprocess
import tempfile

FFMPEG = os.environ.get('FFMPEG', r'C:\ffmpeg\ffmpeg.exe')
GUID = bytes.fromhex('D242E147BA368D4D88FC61654F8C836C')


def riff_chunks(data):
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE', data[:12]
    out = {}
    p = 12
    while p + 8 <= len(data):
        size = struct.unpack_from('<I', data, p + 4)[0]
        out.setdefault(data[p:p + 4], (p + 8, size))
        p += 8 + size + (size & 1)
    return out


def info(wem):
    """dict(tag, channels, rate, block, config, samples, delay, data=(offset, size)) of a little-endian .wem."""
    ch = riff_chunks(wem)
    at, size = ch[b'fmt ']
    tag, channels, rate, _avg, block, _bits = struct.unpack_from('<HHIIHH', wem, at)
    out = dict(tag=tag, channels=channels, rate=rate, block=block, data=ch[b'data'])
    if tag == 0xFFFC:
        _cb, spb, mask = struct.unpack_from('<HHI', wem, at + 16)
        out.update(samples_per_block=spb, mask=mask, config=wem[at + 24:at + 28],
                   samples=struct.unpack_from('<I', wem, at + 28)[0], delay=struct.unpack_from('<H', wem, at + 32)[0])
    return out


def to_at9(wem):
    i = info(wem)
    assert i['tag'] == 0xFFFC and i['config'][0] == 0xFE, i
    at, size = i['data']
    fmt = struct.pack('<HHIIHHHHI', 0xFFFE, i['channels'], i['rate'], i['rate'] * i['block'] // i['samples_per_block'],
                      i['block'], 0, 34, i['samples_per_block'], i['mask']) + GUID + struct.pack('<I', 1) + \
        i['config'] + struct.pack('<I', 0)
    fact = struct.pack('<III', i['samples'], 0, i['delay'])
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'fact' + struct.pack('<I', len(fact)) + fact + \
        b'data' + struct.pack('<I', size) + wem[at:at + size]
    return b'RIFF' + struct.pack('<I', len(body)) + body, i


def decode(wem, rate=None):
    """ATRAC9 .wem -> (channels, rate, s16le bytes): the encoder delay cut off, the length the header's sample count,
    resampled to `rate` when given."""
    at9, i = to_at9(wem)
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'in.at9')
        with open(src, 'wb') as f:
            f.write(at9)
        first, last = i['delay'], i['delay'] + i['samples']
        # -cpuflags 0: ffmpeg's plain C code paths only. With its AVX2/FMA3 ones (the default on this PC) the samples
        # depend on the CPU (1 LSB on about one sample in 10,000); the web installer's WebAssembly build of the same
        # decoder and resampler (peggle2-nx-installer, wasm/atrac9) gives exactly these.
        cmd = [FFMPEG, '-hide_banner', '-loglevel', 'error', '-y', '-cpuflags', '0', '-i', src,
               '-af', 'atrim=start_sample=%d:end_sample=%d' % (first, last)]
        out_rate = i['rate']
        if rate and rate != i['rate']:
            cmd += ['-ar', str(rate)]
            out_rate = rate
        cmd += ['-f', 's16le', '-acodec', 'pcm_s16le', '-']
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.decode('latin-1')[:300])
        return i['channels'], out_rate, r.stdout


if __name__ == '__main__':
    import sys
    data = open(sys.argv[1], 'rb').read()
    print({k: (v.hex() if isinstance(v, bytes) else v) for k, v in info(data).items()})
    channels, rate, pcm = decode(data, int(sys.argv[3]) if len(sys.argv) > 3 else None)
    n = len(pcm) // 2 // channels
    print('decoded: %d channels, %d Hz, %d samples (%.3f s)' % (channels, rate, n, n / rate))
    if len(sys.argv) > 2:
        import wave
        with wave.open(sys.argv[2], 'wb') as w:
            w.setnchannels(channels), w.setsampwidth(2), w.setframerate(rate)
            w.writeframes(pcm)
