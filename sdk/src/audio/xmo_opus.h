/**
 * culdcept-nx (2026-11-20): the Opus decoder for NAME.xmo frames (rex/filesystem/xmo.h, XmaContext::DecodeXmoFrame).
 * libopus: devkitPro's portlibs build, linked in, on the Switch; on a PC the system's or one next to the executable
 * (libopus-0.dll / opus.dll, libopus.so.0, libopus.0.dylib), loaded on first use. Without it the frames are silent.
 */
#pragma once

#include <cstdint>

namespace rex::audio::xmo_opus {

struct Decoder;

// A decoder at `rate` (48000 or 24000: sasrt-nx, 2026-11-21) for `channels` (1 or 2); null when libopus is not there.
Decoder* Create(int channels, int rate = 48000);
void Destroy(Decoder* decoder);
// As a fresh one (a seek).
void Reset(Decoder* decoder);
// One packet into `pcm` (interleaved float, up to `max_samples` a channel): the samples a channel, or < 0.
int Decode(Decoder* decoder, const uint8_t* packet, int size, float* pcm, int max_samples);

}  // namespace rex::audio::xmo_opus
