#pragma once
// peggle2-nx: the port's own shaders for the three shaders Peggle 2 compiles at boot with Direct3D's HLSL compiler
// (D3DXCompileShader in sub_8248D7C0): the movie quads' vertex shader and the Bink YCrCb -> RGB pixel shaders without
// and with alpha. Their containers exist in no file of the game, so the shader library the installer makes from
// default.xex cannot have them (2026-10-14). The renderer draws them with the port's SPIR-V instead
// (shaders/port/peggle1_movie_*.hlsl, written for the port in XenosRecomp's interface; app/src/me/
// peggle1_port_shaders_spirv.inc, made by shaders/port/build_port_shaders.sh).
//
// A port shader is selected by the XXH3 of the container the game created (or, for the vertex shader, of its virtual
// part, which Direct3D's fetch patch does not change). Its library entry is made at run time from that container, read
// from guest memory, and the port's SPIR-V; it then behaves like a library entry everywhere (identification by
// object, address and microcode, pipelines, prewarm by number).
#include <cstdint>
#include <span>

#include "masseffect/masseffect_native_shaders.h"

namespace peggle1::port_shaders {

// Entry numbers of port shaders start here. Library entries are numbered from 0 by position, and a library holds at
// most 65536 (masseffect_shader_library.cpp kMaxShaders), so these never collide. The renderer keys pipelines and
// modules on the number, and two per-number tables (masseffect_native_draws.cpp structured_cf_by_number_,
// index_unused_by_number_) grow to the highest number: keep it small (64 KiB each), not a high bit.
constexpr uint32_t kFirstNumber = 65536u;

// The entry for `container` (the game's own, as D3DDevice_Create*Shader received it or as the shader object holds it
// now, with Direct3D's fetch patch) when a port shader replaces it; nullptr otherwise. One entry per distinct
// container, kept for the life of the process. Thread safe.
const masseffect::native::ShaderEntry* EntryFor(std::span<const uint8_t> container, bool vertex);

inline bool IsPortEntry(const masseffect::native::ShaderEntry* entry) {
  return entry && entry->number >= kFirstNumber;
}

}  // namespace peggle1::port_shaders
