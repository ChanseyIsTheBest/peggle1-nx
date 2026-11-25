/**
 * rr6-nx (2026-11-21): the Opus decoder for NAME.opcm (rex/filesystem/opus_pcm.h), as culdcept-nx's
 * sdk/src/audio/xmo_opus.h (credit culdcept-nx). libopus: devkitPro's portlibs build, linked in, on the Switch; on a
 * PC the system's or one next to the executable (libopus-0.dll / opus.dll, libopus.so.0, libopus.0.dylib), loaded on
 * first use. Without it an .opcm's samples read as silence (warned once).
 */
#pragma once

#include <cstdint>

namespace rex::filesystem::opus {

struct Decoder;

// A 48 kHz decoder for `channels` (1 or 2); null when libopus is not there.
Decoder* Create(int channels);
void Destroy(Decoder* decoder);
// As a fresh one (a seek).
void Reset(Decoder* decoder);
// One packet into `pcm` (interleaved float, up to `max_samples` a channel): the samples a channel, or < 0.
int Decode(Decoder* decoder, const uint8_t* packet, int size, float* pcm, int max_samples);

}  // namespace rex::filesystem::opus
