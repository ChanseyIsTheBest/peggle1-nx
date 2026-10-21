// Peggle 2: HD atlases, the pack reader (see peggle1_hd_pack.h).
#include "peggle1_hd_pack.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../peggle1_enhancements.h"

REXCVAR_DEFINE_INT32(peggle1_hd_atlases, 1, "Peggle1",
                     "HD atlases from peggle1_hd.pack (the PS4 release's 1080 art in the game's atlas layout): 0 = "
                     "never; 1 = when the render height is above 720 (docked, by default); 2 = always, handheld too "
                     "(the larger atlases are then filtered down to the 720p picture). Without the pack file nothing "
                     "changes")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(peggle1_hd_pack, "", "Peggle1",
                      "Path of the HD atlas pack (empty = peggle1_hd.pack next to the program)");
REXCVAR_DEFINE_BOOL(peggle1_hd_verify, false, "Peggle1",
                    "Check every HD image read from the pack against its stored hash (a damaged file or SD card)");

// Non-CPU group: it changes the picture on purpose (sharper sprites), so it is a key like every other change.
PEGGLE1_RING_GPU_ENHANCEMENT(g_hd_atlases, peggle1_gpu_hd_atlases, true,
                             "GPU: atlases from the HD pack in place of the game's 720 atlases (peggle1_hd_atlases: "
                             "docked only or always); needs peggle1_hd.pack next to the program");

namespace peggle1::hd {
namespace {

constexpr uint32_t kMagic = 0x44483250;  // 'P2HD'
constexpr uint32_t kVersion = 1;

std::FILE* g_file = nullptr;
bool g_tried = false;
uint32_t g_chunk = 0;
std::vector<Entry> g_entries;
std::vector<uint32_t> g_sizes;
std::vector<uint8_t> g_packed;
uint64_t g_reads = 0, g_bytes = 0, g_packed_bytes = 0, g_ns = 0;

bool Seek(std::FILE* file, uint64_t offset) {
#if REX_PLATFORM_WIN32
  return _fseeki64(file, int64_t(offset), SEEK_SET) == 0;
#else
  return fseeko(file, off_t(offset), SEEK_SET) == 0;
#endif
}

void Close(const char* why) {
  if (g_file) std::fclose(g_file);
  g_file = nullptr;
  g_entries.clear();
  if (why) REXLOG_ERROR("[hd] the HD atlas pack is closed for this session: {}", why);
}

void Open() {
  g_tried = true;
  std::filesystem::path path = std::string(REXCVAR_GET(peggle1_hd_pack));
  if (path.empty()) path = rex::filesystem::GetExecutableFolder() / "peggle1_hd.pack";
  g_file = std::fopen(path.string().c_str(), "rb");
  if (!g_file) {
    REXLOG_INFO("[hd] no HD atlas pack at {}: the game's own atlases are used", path.string());
    return;
  }
  uint32_t header[4] = {};
  if (std::fread(header, sizeof(header), 1, g_file) != 1 || header[0] != kMagic || header[1] != kVersion ||
      header[2] == 0 || header[2] > 65536 || header[3] < 4096 || header[3] > (16u << 20)) {
    return Close("not a version 1 pack");
  }
  g_chunk = header[3];
  g_entries.resize(header[2]);
  if (std::fread(g_entries.data(), sizeof(Entry), g_entries.size(), g_file) != g_entries.size()) {
    return Close("its index is cut short");
  }
  uint64_t raw = 0;
  for (size_t i = 0; i < g_entries.size(); ++i) {
    const Entry& e = g_entries[i];
    if ((i && g_entries[i - 1].key >= e.key) || e.raw_bytes != uint32_t(e.hd_width / 4) * (e.hd_height / 4) * 16 ||
        e.chunks != (e.raw_bytes + g_chunk - 1) / g_chunk || (e.hd_width & 3) || (e.hd_height & 3)) {
      return Close("an index entry is not what the format says");
    }
    raw += e.raw_bytes;
  }
  REXLOG_INFO("[hd] HD atlas pack {}: {} atlases, {:.0f} MB of BC3 when all are loaded (peggle1_hd_atlases = {}: {})",
              path.string(), g_entries.size(), double(raw) / 1048576.0, REXCVAR_GET(peggle1_hd_atlases),
              REXCVAR_GET(peggle1_hd_atlases) == 2 ? "always" : REXCVAR_GET(peggle1_hd_atlases) == 1
                  ? "when the render height is above 720" : "never");
}

// LZ4 block format, every read and write checked. Returns the bytes written or -1.
int64_t Lz4Decode(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
  const uint8_t* ip = in;
  const uint8_t* const iend = in + in_size;
  uint8_t* op = out;
  uint8_t* const oend = out + out_size;
  while (ip < iend) {
    const uint32_t token = *ip++;
    size_t literals = token >> 4;
    if (literals == 15) {
      uint8_t b;
      do {
        if (ip >= iend) return -1;
        b = *ip++;
        literals += b;
      } while (b == 255);
    }
    if (literals > size_t(iend - ip) || literals > size_t(oend - op)) return -1;
    std::memcpy(op, ip, literals);
    ip += literals;
    op += literals;
    if (ip >= iend) break;  // the last sequence is literals only
    if (iend - ip < 2) return -1;
    const size_t offset = size_t(ip[0]) | (size_t(ip[1]) << 8);
    ip += 2;
    if (offset == 0 || offset > size_t(op - out)) return -1;
    size_t match = token & 15;
    if (match == 15) {
      uint8_t b;
      do {
        if (ip >= iend) return -1;
        b = *ip++;
        match += b;
      } while (b == 255);
    }
    match += 4;
    if (match > size_t(oend - op)) return -1;
    // A match may overlap its own output (a repeated pattern): copied in runs no longer than what is already there.
    const uint8_t* const from = op - offset;
    while (match) {
      const size_t run = std::min(match, size_t(op - from));
      std::memcpy(op, from, run);
      op += run;
      match -= run;
    }
  }
  return op - out;
}

}  // namespace

bool Wanted(uint32_t scale_quarters) {
  if (!g_hd_atlases.active) return false;
  const int32_t mode = REXCVAR_GET(peggle1_hd_atlases);
  if (mode <= 0 || (mode == 1 && scale_quarters <= 4)) return false;
  if (!g_tried) Open();
  return g_file != nullptr;
}

const Entry* Find(uint64_t key, uint32_t guest_width, uint32_t guest_height) {
  const auto it = std::lower_bound(g_entries.begin(), g_entries.end(), key,
                                   [](const Entry& e, uint64_t k) { return e.key < k; });
  if (it == g_entries.end() || it->key != key || it->guest_width != guest_width || it->guest_height != guest_height) {
    return nullptr;
  }
  return &*it;
}

bool Read(const Entry& entry, uint8_t* out, size_t bytes) {
  if (!g_file || bytes != entry.raw_bytes) return false;
  const auto started = std::chrono::steady_clock::now();
  g_sizes.resize(entry.chunks);
  if (!Seek(g_file, entry.offset) || std::fread(g_sizes.data(), 4, entry.chunks, g_file) != entry.chunks) {
    Close("a chunk table could not be read");
    return false;
  }
  uint64_t packed = 0;
  for (uint32_t i = 0; i < entry.chunks; ++i) {
    const uint32_t want = std::min<uint32_t>(g_chunk, entry.raw_bytes - i * g_chunk);
    if (g_sizes[i] == 0 || g_sizes[i] > want) {
      Close("a chunk size is not what the format says");
      return false;
    }
    packed += g_sizes[i];
  }
  g_packed.resize(size_t(packed));
  if (std::fread(g_packed.data(), 1, g_packed.size(), g_file) != g_packed.size()) {
    Close("an image is cut short");
    return false;
  }
  const uint8_t* in = g_packed.data();
  for (uint32_t i = 0; i < entry.chunks; ++i) {
    const uint32_t want = std::min<uint32_t>(g_chunk, entry.raw_bytes - i * g_chunk);
    uint8_t* const to = out + size_t(i) * g_chunk;
    if (g_sizes[i] == want) {
      std::memcpy(to, in, want);
    } else if (Lz4Decode(in, g_sizes[i], to, want) != int64_t(want)) {
      Close("a chunk does not decompress");
      return false;
    }
    in += g_sizes[i];
  }
  if (REXCVAR_GET(peggle1_hd_verify) && XXH3_64bits(out, bytes) != entry.hash) {
    Close("an image does not have its stored hash");
    return false;
  }
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - started).count());
  ++g_reads;
  g_bytes += bytes;
  g_packed_bytes += packed;
  g_ns += ns;
  if (g_reads <= 12 || (g_reads & 15) == 0) {
    REXLOG_INFO("[hd] atlas {}x{} -> {}x{} from the pack: {:.1f} MB packed to {:.1f} MB in {:.1f} ms; {} so far, "
                "{:.0f} MB, {:.1f} ms each on average",
                entry.guest_width, entry.guest_height, entry.hd_width, entry.hd_height, double(packed) / 1048576.0,
                double(bytes) / 1048576.0, double(ns) / 1e6, g_reads, double(g_bytes) / 1048576.0,
                double(g_ns) / 1e6 / double(g_reads));
  }
  g_hd_atlases.Use();
  return true;
}

}  // namespace peggle1::hd
