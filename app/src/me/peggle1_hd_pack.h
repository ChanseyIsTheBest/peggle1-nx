// Peggle 2: HD atlases (peggle1_hd_atlases, peggle1_gpu_hd_atlases; docs/peggle1-notes.md "HD atlases").
//
// The Xbox 360 game loads its 720 art (the group names are in its executable); the PS4 release has the same sprites
// drawn for 1080. tools/hd/build_hd_pack.py makes, for every 720 atlas, an image 1.5 times the size in each axis with
// the SAME layout, its sprites taken from the PS4 art, and files it in peggle1_hd.pack under the XXH3-64 of the 720
// atlas's bytes. The renderer (PrepareTexture, masseffect/masseffect_native_draws.cpp) already takes that fingerprint
// of every texture it uploads: when the pack has it, it uploads the pack's image instead. The game's texture
// coordinates are fractions of the atlas, so nothing else changes; the sizes the shaders get stay the game's.
//
// Pack file (little endian): magic 'P2HD', version, count, chunk size; `count` entries sorted by key; then per image
// a table of `chunks` compressed sizes and the chunks: LZ4 blocks of `chunk size` bytes of BC3 (a chunk whose stored
// size is its full size is not compressed).
//
// Ring thread only.
#pragma once

#include <cstddef>
#include <cstdint>

namespace peggle1::hd {

struct Entry {
  uint64_t key;     // XXH3-64 of the guest texture's bytes
  uint64_t offset;  // of its chunk table in the file
  uint64_t hash;    // XXH3-64 of the HD image's bytes (the build tool checks it; peggle1_hd_verify does here)
  uint32_t raw_bytes;
  uint32_t chunks;
  uint16_t guest_width, guest_height, hd_width, hd_height;
};
static_assert(sizeof(Entry) == 40);

// Whether HD atlases are used at this render scale (quarters of 1280x720): the keys, the mode and an open pack.
// Opens the pack at the first call that needs it; without the file this is false for the session.
bool Wanted(uint32_t scale_quarters);

// The pack's image for a guest texture of that fingerprint and size, or nullptr.
const Entry* Find(uint64_t key, uint32_t guest_width, uint32_t guest_height);

// Reads and decompresses an image (`bytes` must be its raw_bytes). false = the file could not be read or is damaged:
// the pack is then closed for the session.
bool Read(const Entry& entry, uint8_t* out, size_t bytes);

}  // namespace peggle1::hd
