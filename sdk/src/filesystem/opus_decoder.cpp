/**
 * rr6-nx (2026-11-21): opus_decoder.h; culdcept-nx's sdk/src/audio/xmo_opus.cpp with its names (credit culdcept-nx).
 */
#include "opus_decoder.h"

#include <mutex>

#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_SWITCH
#include <opus.h>
#else
#include <rex/platform/dynlib.h>
#endif

namespace rex::filesystem::opus {

#if REX_PLATFORM_SWITCH

struct Decoder {
  OpusDecoder* d;
};

Decoder* Create(int channels) {
  int error = 0;
  OpusDecoder* d = opus_decoder_create(48000, channels, &error);
  if (!d) {
    REXFS_ERROR("[opcm] opus_decoder_create failed: {}", error);
    return nullptr;
  }
  return new Decoder{d};
}

void Destroy(Decoder* decoder) {
  if (!decoder) return;
  opus_decoder_destroy(decoder->d);
  delete decoder;
}

void Reset(Decoder* decoder) { opus_decoder_ctl(decoder->d, OPUS_RESET_STATE); }

int Decode(Decoder* decoder, const uint8_t* packet, int size, float* pcm, int max_samples) {
  return opus_decode_float(decoder->d, packet, size, pcm, max_samples, 0);
}

#else  // a PC: libopus loaded on first use

namespace {
constexpr int kOpusResetState = 4028;  // OPUS_RESET_STATE (opus_defines.h)
struct Api {
  void* (*create)(int32_t, int, int*) = nullptr;
  void (*destroy)(void*) = nullptr;
  int (*ctl)(void*, int, ...) = nullptr;
  int (*decode_float)(void*, const unsigned char*, int32_t, float*, int, int) = nullptr;
  rex::platform::DynamicLibrary lib;
  bool ok = false;
};

const Api& GetApi() {
  static Api api;
  static std::once_flag once;
  std::call_once(once, [] {
#if REX_PLATFORM_WIN32
    const char* names[] = {"libopus-0.dll", "opus.dll"};
#elif REX_PLATFORM_MAC
    const char* names[] = {"libopus.0.dylib", "libopus.dylib"};
#else
    const char* names[] = {"libopus.so.0", "libopus.so"};
#endif
    for (const char* name : names) {
      if (api.lib.Load(name)) break;
    }
    if (api.lib) {
      api.create = api.lib.GetSymbol<decltype(api.create)>("opus_decoder_create");
      api.destroy = api.lib.GetSymbol<decltype(api.destroy)>("opus_decoder_destroy");
      api.ctl = api.lib.GetSymbol<decltype(api.ctl)>("opus_decoder_ctl");
      api.decode_float = api.lib.GetSymbol<decltype(api.decode_float)>("opus_decode_float");
      api.ok = api.create && api.destroy && api.ctl && api.decode_float;
    }
    if (!api.ok)
      REXFS_WARN("[opcm] libopus was not found ({}): the music in .opcm files reads as silence", names[0]);
  });
  return api;
}
}  // namespace

struct Decoder {
  void* d;
};

Decoder* Create(int channels) {
  const Api& api = GetApi();
  if (!api.ok) return nullptr;
  int error = 0;
  void* d = api.create(48000, channels, &error);
  if (!d) {
    REXFS_ERROR("[opcm] opus_decoder_create failed: {}", error);
    return nullptr;
  }
  return new Decoder{d};
}

void Destroy(Decoder* decoder) {
  if (!decoder) return;
  GetApi().destroy(decoder->d);
  delete decoder;
}

void Reset(Decoder* decoder) { GetApi().ctl(decoder->d, kOpusResetState); }

int Decode(Decoder* decoder, const uint8_t* packet, int size, float* pcm, int max_samples) {
  return GetApi().decode_float(decoder->d, packet, size, pcm, max_samples, 0);
}

#endif

}  // namespace rex::filesystem::opus
