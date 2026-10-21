// peggle2-nx: the port's own shaders for the shaders Peggle 2 compiles at boot (see peggle1_port_shaders.h).

// xxHash first, reading with memcpy, as masseffect_native_shaders.cpp includes it (GCC strict aliasing).
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "peggle1_port_shaders.h"

#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <unordered_map>

#include <rex/logging.h>

#include "masseffect/masseffect_shader_library.h"

namespace peggle1::port_shaders {
namespace {

// kPeggle1MovieVs, kPeggle1MoviePs, kPeggle1MoviePsAlpha: SPIR-V words (shaders/port/build_port_shaders.sh).
#include "peggle1_port_shaders_spirv.inc"

struct PortShader {
  const char* name;
  bool vertex;
  uint64_t container_xxh3;  // the container as D3DXCompileShader made it
  uint64_t virtual_xxh3;    // its virtual part (vertex shader: unchanged by Direct3D's fetch patch); 0: not used
  const uint32_t* words;
  size_t count;
};

// The containers' XXH3 are the names peggle1_dump_shaders gives them (vs_3caebedd6edf56e6.bin ...). The vertex
// shader as Direct3D patches it for its 20-byte vertices is 04b874f1a0f87055; its virtual part 4c9d82ce4a6d491f.
constexpr PortShader kPortShaders[] = {
    {"movie VS (position + texture coordinate)", true, 0x3caebedd6edf56e6ull, 0x4c9d82ce4a6d491full, kPeggle1MovieVs,
     std::size(kPeggle1MovieVs)},
    {"movie PS (YCrCb)", false, 0x23fb686cb21303f8ull, 0, kPeggle1MoviePs, std::size(kPeggle1MoviePs)},
    {"movie PS (YCrCb + alpha)", false, 0xb79e9877de15d6dbull, 0, kPeggle1MoviePsAlpha,
     std::size(kPeggle1MoviePsAlpha)},
};

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

const PortShader* Find(std::span<const uint8_t> container, bool vertex, uint64_t hash) {
  for (const PortShader& p : kPortShaders) {
    if (p.vertex == vertex && p.container_xxh3 == hash) return &p;
  }
  if (container.size() < 8) return nullptr;
  const uint32_t virtual_size = Be32(container.data() + 4);
  if (virtual_size > container.size()) return nullptr;
  const uint64_t virtual_hash = XXH3_64bits(container.data(), virtual_size);
  for (const PortShader& p : kPortShaders) {
    if (p.vertex == vertex && p.virtual_xxh3 && p.virtual_xxh3 == virtual_hash) return &p;
  }
  return nullptr;
}

std::mutex g_mutex;
std::deque<masseffect::native::Shader> g_shaders;          // stable addresses: entries point at them
std::deque<masseffect::native::ShaderEntry> g_entries;     // stable addresses: the renderer keeps pointers
std::unordered_map<uint64_t, const masseffect::native::ShaderEntry*> g_by_container;  // XXH3 of the container

}  // namespace

const masseffect::native::ShaderEntry* EntryFor(std::span<const uint8_t> container, bool vertex) {
  const uint64_t hash = XXH3_64bits(container.data(), container.size());
  const PortShader* port = Find(container, vertex, hash);
  if (!port) return nullptr;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (const auto it = g_by_container.find(hash); it != g_by_container.end()) return it->second;
  masseffect::native::Shader& shader = g_shaders.emplace_back();
  shader.original.assign(container.begin(), container.end());  // the game's, from guest memory; never written out
  shader.spirv.assign(port->words, port->words + port->count);
  shader.fingerprint = hash;
  shader.vertices = vertex;
  masseffect::native::ShaderEntry& entry = g_entries.emplace_back();
  const uint32_t number = kFirstNumber + uint32_t(g_entries.size() - 1);
  if (const char* reason = masseffect::native::MakeEntry(shader, number, entry)) {
    REXLOG_WARN("[native] port shader {} not used for {}_{:016x}: {}", port->name, vertex ? "vs" : "ps", hash, reason);
    g_entries.pop_back();
    g_shaders.pop_back();
    g_by_container.emplace(hash, nullptr);
    return nullptr;
  }
  REXLOG_INFO("[native] port shader {} for {}_{:016x} (n{}, {} words of SPIR-V)", port->name, vertex ? "vs" : "ps",
              hash, number, port->count);
  g_by_container.emplace(hash, &entry);
  return &entry;
}

}  // namespace peggle1::port_shaders
