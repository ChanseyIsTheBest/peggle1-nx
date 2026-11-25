/**
 * culdcept-nx (2026-11-20, credit culdcept-nx): NAME.xmo, an XMA file whose frames are Opus (rex/filesystem/xmo.h has
 * the format). The handle below synthesizes NAME's bytes on each read, a packet at a time: its RIFF header, packet
 * headers and trailer as they were, each frame's 15-bit length field and last bit, and the packet's tag in its tag bits
 * (everything else zero), so the game and the XMA emulation walk the same packets and frames as before. Deterministic
 * (the read caches and parked handles keep what it gives).
 *
 * sasrt-nx (2026-11-21): NAME.xmop, a Wwise package whose XMA2 sounds are Opus: its segments (stored bytes, zeros, a
 * sound's packets) make NAME; a sound's packets are synthesized as NAME.xmo's are, from its own packet headers and frame
 * table, read when a read first reaches the sound and kept for the most recently used sounds.
 */
#include <rex/filesystem/xmo.h>

#include <algorithm>
#include <cstring>
#include <list>
#include <map>
#include <atomic>
#include <mutex>

#include <rex/filesystem.h>
#include <rex/filesystem/lz4_blocks.h>  // NAME.xmop.lz4b (sasrt-nx, 2026-11-21)
#include <rex/logging.h>
#include <rex/string.h>

namespace rex::filesystem {
namespace {

constexpr char kMagic[8] = {'R', 'X', 'X', 'M', 'A', 'O', 'P', '1'};
constexpr char kPackMagic[8] = {'R', 'X', 'X', 'M', 'A', 'P', 'K', '1'};
constexpr size_t kHeaderSize = 64;
constexpr size_t kSegmentSize = 32;
constexpr size_t kSoundEntrySize = 64;
constexpr uint32_t kPacketBits = kXmoPacketBytes * 8;
constexpr uint32_t kPacketHeaderBits = 32;
constexpr uint32_t kFrameHeaderBits = 15;
constexpr uint32_t kMaxFrameLength = 0x7FFF;
// The Opus data of this many bytes stays read at most (sounds not in use are read again when they play): a piece of
// music is 2-5 MB at 128 kbit/s, an effect a few KB.
constexpr size_t kLoadedLimit = 16u << 20;
// sasrt-nx: the packet headers and frame tables of this many bytes of .xmop sounds stay read at most (a 3-minute piece
// of music at 240 kbit/s XMA: ~17,000 frames, 136 KB).
constexpr size_t kLayoutLimit = 4u << 20;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32); }

bool ReadExactly(FileHandle& f, uint64_t offset, void* to, size_t length) {
  size_t got = 0;
  return f.Read(size_t(offset), to, length, &got) && got == length;
}

bool HasSuffix(std::string_view name, std::string_view suffix) {
  if (name.size() <= suffix.size()) return false;
  const std::string_view tail = name.substr(name.size() - suffix.size());
  for (size_t i = 0; i < tail.size(); ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != suffix[i]) return false;
  }
  return true;
}

// sasrt-nx (2026-11-21, as rr6-nx's sfd_hevc.cpp does for NAME.hevc.lz4b): NAME.xmop.lz4b holds the .xmop package,
// read through OpenLz4Blocks; any other handle as it is.
bool IsPacked(const std::filesystem::path& path) { return HasLz4BlocksSuffix(rex::path_to_utf8(path.filename())); }
std::unique_ptr<FileHandle> Unpacked(std::unique_ptr<FileHandle> f) {
  if (!f || !IsPacked(f->path())) return f;
  return OpenLz4Blocks(std::move(f));  // logs why when it is not a valid .lz4b file
}
bool UnpackedSize(const std::filesystem::path& path, uint64_t* out_size) {
  if (IsPacked(path)) return ReadLz4BlocksSize(path, out_size);
  FileInfo info;
  if (!GetInfo(path, &info)) return false;
  *out_size = info.total_size;
  return true;
}

struct Header {
  uint32_t id = 0;
  uint64_t size = 0;
  uint32_t data_offset = 0, packets = 0, frames = 0, rate = 0;
  uint16_t channels = 0, pre_skip = 0;
  uint32_t opus_count = 0;
  uint64_t opus_table = 0;
  uint32_t trailer = 0;
  uint64_t packets_end() const { return uint64_t(data_offset) + uint64_t(packets) * kXmoPacketBytes; }
  uint64_t frame_table() const { return kHeaderSize + data_offset + uint64_t(packets) * 4; }
};

bool ParseHeader(const uint8_t* h, Header* out) {
  if (std::memcmp(h, kMagic, 8) != 0 || Le32(h + 8) != 1) return false;
  out->id = Le32(h + 12);
  out->size = Le64(h + 16);
  out->data_offset = Le32(h + 24);
  out->packets = Le32(h + 28);
  out->frames = Le32(h + 32);
  out->rate = Le32(h + 36);
  out->channels = Le16(h + 40);
  out->pre_skip = Le16(h + 42);
  out->opus_count = Le32(h + 44);
  out->opus_table = Le64(h + 48);
  out->trailer = Le32(h + 56);
  return out->data_offset <= 4096 && out->channels >= 1 && out->channels <= 2 && out->rate == 48000 &&
         out->trailer <= (1u << 20) && out->packets_end() + out->trailer == out->size &&
         out->packets < (1u << 24);  // the tag's packet index is 24 bits
}

bool ReadHeader(FileHandle& f, Header* out) {
  uint8_t h[kHeaderSize];
  return ReadExactly(f, 0, h, sizeof(h)) && ParseHeader(h, out);
}

// sasrt-nx: NAME.xmop's header and a sound of its table.
struct PackHeader {
  uint32_t segments = 0;
  uint64_t size = 0, segments_offset = 0, stored_offset = 0, stored_length = 0;
  uint32_t sounds = 0;
  uint64_t sounds_offset = 0;
};

bool ParsePackHeader(const uint8_t* h, PackHeader* out) {
  if (std::memcmp(h, kPackMagic, 8) != 0 || Le32(h + 8) != 1) return false;
  out->segments = Le32(h + 12);
  out->size = Le64(h + 16);
  out->segments_offset = Le64(h + 24);
  out->stored_offset = Le64(h + 32);
  out->stored_length = Le64(h + 40);
  out->sounds = Le32(h + 48);
  out->sounds_offset = Le64(h + 56);
  return out->segments >= 1 && out->segments <= (1u << 26) && out->sounds <= (1u << 20);
}

struct PackSound {
  uint32_t id = 0, packets = 0, frames = 0, rate = 0;
  uint16_t channels = 0, pre_skip = 0;
  uint32_t opus_count = 0, opus_rate = 0;
  uint64_t headers = 0, frame_table = 0, opus_table = 0;
};

bool ParsePackSound(const uint8_t* e, PackSound* out) {
  out->id = Le32(e);
  out->packets = Le32(e + 4);
  out->frames = Le32(e + 8);
  out->rate = Le32(e + 12);
  out->channels = Le16(e + 16);
  out->pre_skip = Le16(e + 18);
  out->opus_count = Le32(e + 20);
  out->opus_rate = Le32(e + 24);
  out->headers = Le64(e + 32);
  out->frame_table = Le64(e + 40);
  out->opus_table = Le64(e + 48);
  const bool rates = (out->rate == 48000 && out->opus_rate == 48000) || (out->rate == 24000 && out->opus_rate == 24000) ||
                     (out->rate == 44100 && out->opus_rate == 48000);
  return rates && out->channels >= 1 && out->channels <= 2 && out->packets >= 1 && out->packets < (1u << 24) &&
         out->frames >= 1;
}

struct Frame {
  uint32_t pos;  // bit position in the data
  uint16_t len;  // bits
  uint8_t last;  // the frame's last bit
};

inline uint32_t Bit(const uint8_t* p, uint32_t b) { return (p[b >> 3] >> (7 - (b & 7))) & 1; }
// Bit k of a tag (72 bits, more than a uint64_t: magic, sound id, packet index).
inline uint32_t TagBit(uint32_t k, uint32_t id, uint32_t packet_index) {
  return k < 16 ? (kXmoTagMagic >> (15 - k)) & 1 : k < 48 ? (id >> (47 - k)) & 1 : (packet_index >> (71 - k)) & 1;
}
inline void SetBit(uint8_t* p, uint32_t b) { p[b >> 3] |= uint8_t(0x80 >> (b & 7)); }

// Packet p of a sound's data, synthesized: its header, the length fields and last bits of the frames in it (a frame is
// at most 32767 bits: one that starts two packets before can reach into it), then its tag. NAME.xmo's and NAME.xmop's.
class PacketMaker {
 public:
  PacketMaker(uint32_t id, uint32_t packets, const uint8_t* packet_headers, const Frame* frames, size_t frame_count)
      : id_(id), packets_(packets), headers_(packet_headers), frames_(frames), frame_count_(frame_count) {}

  void Synthesize(uint32_t p, uint8_t* packet) const {
    std::memset(packet, 0, kXmoPacketBytes);
    std::memcpy(packet, headers_ + size_t(p) * 4, 4);
    const uint64_t from = p >= 2 ? uint64_t(p - 2) * kPacketBits : 0, to = uint64_t(p + 1) * kPacketBits;
    const Frame* end = frames_ + frame_count_;
    const Frame* it = std::lower_bound(frames_, end, from, [](const Frame& f, uint64_t v) { return f.pos < v; });
    bool starts = false;  // a frame starts in packet p
    for (; it != end && it->pos < to; ++it) {
      Put(packet, p, *it, 0, kFrameHeaderBits, it->len);
      Put(packet, p, *it, uint32_t(it->len) - 1, 1, it->last);
      starts = starts || it->pos >= uint64_t(p) * kPacketBits;
    }
    // sasrt-nx (2026-11-21, the installer's check): only a packet a frame starts in carries a tag (xmo.h's rule). A
    // sound's last packet can have no frame starting in it and a skip count other than 0xFF: XmoTagBits would then give
    // the end of the frame that runs into it, and the tag would set that frame's last bit ("another frame follows").
    if (!starts) return;
    uint16_t bits[kXmoTagBits];
    if (XmoTagBits(packet, bits, kXmoTagBits) < kXmoTagBits) return;  // no frame starts here (the installer checked)
    for (uint32_t k = 0; k < kXmoTagBits; ++k) {
      if (TagBit(k, id_, p)) SetBit(packet, bits[k]);
    }
  }

 private:
  // Bit `j` of frame `f` as packet and bit: a frame goes on past its packet's end in the packet its header's skip count
  // names (the next one, for a single stream), after that packet's 32-bit header.
  void FrameBit(const Frame& f, uint32_t j, uint64_t* packet, uint32_t* in) const {
    uint64_t pk = f.pos / kPacketBits, b = f.pos % kPacketBits + j;
    while (b >= kPacketBits) {
      b -= kPacketBits;
      const uint8_t skip = pk < packets_ ? headers_[size_t(pk * 4 + 3)] : 0;
      pk += uint64_t(skip == 0xFF ? 0 : skip) + 1;
      b += kPacketHeaderBits;
    }
    *packet = pk;
    *in = uint32_t(b);
  }
  // `count` bits of `value` (MSB first) at frame bits [j, j + count), those that fall in packet p.
  void Put(uint8_t* packet, uint32_t p, const Frame& f, uint32_t j, uint32_t count, uint32_t value) const {
    for (uint32_t k = 0; k < count; ++k) {
      if (!((value >> (count - 1 - k)) & 1)) continue;
      uint64_t pk;
      uint32_t b;
      FrameBit(f, j + k, &pk, &b);
      if (pk == p) SetBit(packet, b);
    }
  }

  uint32_t id_, packets_;
  const uint8_t* headers_;
  const Frame* frames_;
  size_t frame_count_;
};

bool ParseFrames(const uint8_t* table, uint32_t count, uint32_t packets, std::vector<Frame>* out) {
  out->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = table + size_t(i) * 8;
    (*out)[i] = {Le32(e), Le16(e + 4), e[6]};
    if ((*out)[i].len <= kFrameHeaderBits || (*out)[i].pos / kPacketBits >= packets ||
        (i && (*out)[i].pos <= (*out)[i - 1].pos)) {
      return false;
    }
  }
  return true;
}

// --- the registry: sound id -> its .xmo (or its .xmop and place in it), the Opus data read when the game reads the
// sound's data ---
struct Registered {
  std::filesystem::path path;
  bool pack = false;
  Header header;          // .xmo
  PackSound pack_sound;   // .xmop
  std::shared_ptr<XmoSound> sound;  // null until read (and again after it is let go)
};
std::mutex g_mutex;
std::map<uint32_t, Registered> g_sounds;
std::list<uint32_t> g_loaded;  // read sounds, the most recently used first
size_t g_loaded_bytes = 0;
std::atomic<bool> g_any_sound{false};  // base port 259: AnyXmoSound

void Register(const std::filesystem::path& path, const Header& h) {
  std::lock_guard lock(g_mutex);
  g_any_sound.store(true, std::memory_order_relaxed);
  auto [it, inserted] = g_sounds.try_emplace(h.id);
  if (!inserted && it->second.path != path) {
    REXFS_WARN("[xmo] sound id {:08X} is in both {} and {}: the second is used", h.id,
               rex::path_to_utf8(it->second.path), rex::path_to_utf8(path));
    it->second.sound.reset();
  }
  it->second.path = path;
  it->second.pack = false;
  it->second.header = h;
}

void RegisterPack(const std::filesystem::path& path, const std::vector<PackSound>& sounds) {
  std::lock_guard lock(g_mutex);
  if (!sounds.empty()) g_any_sound.store(true, std::memory_order_relaxed);
  for (const PackSound& s : sounds) {
    auto [it, inserted] = g_sounds.try_emplace(s.id);
    if (!inserted && it->second.path != path) {
      REXFS_WARN("[xmo] sound id {:08X} is in both {} and {}: the second is used", s.id,
                 rex::path_to_utf8(it->second.path), rex::path_to_utf8(path));
      it->second.sound.reset();
    }
    it->second.path = path;
    it->second.pack = true;
    it->second.pack_sound = s;
  }
}

std::shared_ptr<XmoSound> LoadOpus(FileHandle& f, uint32_t id, uint32_t rate, uint32_t opus_rate, uint16_t channels,
                                   uint16_t pre_skip, uint64_t frame_table, uint32_t frames, uint64_t opus_table,
                                   uint32_t opus_count) {
  std::vector<uint8_t> table(size_t(frames) * 8), sizes(size_t(opus_count) * 2);
  if (!ReadExactly(f, frame_table, table.data(), table.size()) ||
      !ReadExactly(f, opus_table, sizes.data(), sizes.size())) {
    return nullptr;
  }
  auto s = std::make_shared<XmoSound>();
  s->id = id;
  s->sample_rate = rate;
  s->opus_rate = opus_rate;
  s->channels = channels;
  s->pre_skip = pre_skip;
  s->frame_pos.resize(frames);
  for (uint32_t i = 0; i < frames; ++i) s->frame_pos[i] = Le32(table.data() + size_t(i) * 8);
  s->opus_offset.resize(size_t(opus_count) + 1);
  uint32_t total = 0;
  for (uint32_t i = 0; i < opus_count; ++i) {
    s->opus_offset[i] = total;
    total += Le16(sizes.data() + size_t(i) * 2);
  }
  s->opus_offset[opus_count] = total;
  s->opus.resize(total);
  if (total && !ReadExactly(f, opus_table + sizes.size(), s->opus.data(), total)) return nullptr;
  return s;
}

std::atomic<XmoSourceOpener> g_opener{nullptr};  // the PopCap ports' asset pack (2026-11-24)

std::shared_ptr<XmoSound> Load(const Registered& r) {
  std::unique_ptr<FileHandle> raw;
  if (const XmoSourceOpener opener = g_opener.load(std::memory_order_acquire)) raw = opener(r.path);
  if (!raw) raw = FileHandle::OpenExisting(r.path, FileAccess::kFileReadData);
  auto f = Unpacked(std::move(raw));
  if (!f) return nullptr;
  if (r.pack) {
    const PackSound& p = r.pack_sound;
    return LoadOpus(*f, p.id, p.rate, p.opus_rate, p.channels, p.pre_skip, p.frame_table, p.frames, p.opus_table,
                    p.opus_count);
  }
  const Header& h = r.header;
  return LoadOpus(*f, h.id, h.rate, h.rate, h.channels, h.pre_skip, h.frame_table(), h.frames, h.opus_table,
                  h.opus_count);
}

// --- the synthesized XMA file ---
class XmoHandle : public FileHandle {
 public:
  XmoHandle(std::unique_ptr<FileHandle> packed, Header h, std::vector<uint8_t> riff, std::vector<uint8_t> packet_headers,
            std::vector<Frame> frames, std::vector<uint8_t> trailer)
      : FileHandle(packed->path()),
        packed_(std::move(packed)),
        h_(h),
        riff_(std::move(riff)),
        packet_headers_(std::move(packet_headers)),
        frames_(std::move(frames)),
        trailer_(std::move(trailer)),
        maker_(h_.id, h_.packets, packet_headers_.data(), frames_.data(), frames_.size()) {}

  bool Read(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) override {
    *out_bytes_read = 0;
    if (file_offset >= h_.size) return true;
    const size_t n = size_t(std::min<uint64_t>(buffer_length, h_.size - file_offset));
    uint8_t* out = static_cast<uint8_t*>(buffer);
    const uint64_t lo = file_offset, hi = file_offset + n;
    // NAME's RIFF header
    if (lo < h_.data_offset) std::memcpy(out, riff_.data() + lo, size_t(std::min<uint64_t>(hi, h_.data_offset) - lo));
    // NAME's trailer (its seek chunk)
    const uint64_t t = h_.packets_end();
    if (hi > t) {
      const uint64_t from = std::max(lo, t);
      std::memcpy(out + (from - lo), trailer_.data() + (from - t), size_t(hi - from));
    }
    // the packets, each synthesized whole and the part in the range copied
    if (hi > h_.data_offset && lo < t) {
      // The game reads the data before it plays it: the Opus is read now, on its thread, not by the first frame's
      // decode on the audio thread.
      FindXmoSound(h_.id);
      const uint64_t d_lo = lo > h_.data_offset ? lo - h_.data_offset : 0, d_hi = std::min(hi, t) - h_.data_offset;
      uint8_t packet[kXmoPacketBytes];
      for (uint64_t p = d_lo / kXmoPacketBytes; p * kXmoPacketBytes < d_hi; ++p) {
        maker_.Synthesize(uint32_t(p), packet);
        const uint64_t a = std::max(d_lo, p * kXmoPacketBytes), b = std::min(d_hi, (p + 1) * kXmoPacketBytes);
        std::memcpy(out + (h_.data_offset + a - lo), packet + (a - p * kXmoPacketBytes), size_t(b - a));
      }
    }
    *out_bytes_read = n;
    return true;
  }
  bool Write(size_t, const void*, size_t, size_t*) override { return false; }
  bool SetLength(size_t) override { return false; }
  void Flush() override {}

 private:
  std::unique_ptr<FileHandle> packed_;
  Header h_;
  std::vector<uint8_t> riff_, packet_headers_;
  std::vector<Frame> frames_;
  std::vector<uint8_t> trailer_;
  PacketMaker maker_;
};

// --- sasrt-nx (2026-11-21): the synthesized Wwise package ---
struct PackSegment {
  uint64_t offset = 0, length = 0;
  uint32_t kind = 0, sound = 0;
  uint64_t source = 0;
};

class XmopHandle : public FileHandle {
 public:
  XmopHandle(std::unique_ptr<FileHandle> packed, PackHeader h, std::vector<PackSegment> segments,
             std::vector<PackSound> sounds)
      : FileHandle(packed->path()),
        packed_(std::move(packed)),
        h_(h),
        segments_(std::move(segments)),
        sounds_(std::move(sounds)) {}

  bool Read(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *out_bytes_read = 0;
    if (file_offset >= h_.size) return true;
    const size_t want = size_t(std::min<uint64_t>(buffer_length, h_.size - file_offset));
    auto* out = static_cast<uint8_t*>(buffer);
    size_t si = size_t(std::upper_bound(segments_.begin(), segments_.end(), uint64_t(file_offset),
                                        [](uint64_t v, const PackSegment& s) { return v < s.offset; }) -
                       segments_.begin());
    if (si == 0) return false;
    --si;
    size_t done = 0;
    while (done < want && si < segments_.size()) {
      const PackSegment& seg = segments_[si];
      const uint64_t inside = file_offset + done - seg.offset;
      const size_t take = size_t(std::min<uint64_t>(want - done, seg.length - inside));
      if (seg.kind == 0) {
        if (!ReadExactly(*packed_, h_.stored_offset + seg.source + inside, out + done, take)) {
          REXFS_ERROR("[xmo] {}: a read of its stored bytes failed", rex::path_to_utf8(path_));
          return false;
        }
      } else if (seg.kind == 1) {
        std::memset(out + done, 0, take);
      } else if (!SoundBytes(seg.sound, seg.source + inside, out + done, take)) {
        return false;
      }
      done += take;
      ++si;
    }
    *out_bytes_read = done;
    return true;
  }
  bool Write(size_t, const void*, size_t, size_t*) override { return false; }
  bool SetLength(size_t) override { return false; }
  void Flush() override {}

 private:
  struct Layout {
    std::vector<uint8_t> headers;
    std::vector<Frame> frames;
    size_t bytes() const { return headers.size() + frames.size() * sizeof(Frame); }
  };

  // Bytes [from, from + n) of sound s's packets, synthesized.
  bool SoundBytes(uint32_t s, uint64_t from, uint8_t* out, size_t n) {
    const PackSound& ps = sounds_[s];
    const Layout* layout = GetLayout(s);
    if (!layout) return false;
    FindXmoSound(ps.id);  // the Opus read now, on the game's thread (as NAME.xmo's)
    const PacketMaker maker(ps.id, ps.packets, layout->headers.data(), layout->frames.data(), layout->frames.size());
    uint8_t packet[kXmoPacketBytes];
    const uint64_t to = from + n;
    for (uint64_t p = from / kXmoPacketBytes; p * kXmoPacketBytes < to; ++p) {
      if (p >= ps.packets) {
        std::memset(out + (std::max(from, p * kXmoPacketBytes) - from), 0, size_t(to - std::max(from, p * kXmoPacketBytes)));
        break;
      }
      maker.Synthesize(uint32_t(p), packet);
      const uint64_t a = std::max(from, p * kXmoPacketBytes), b = std::min(to, (p + 1) * kXmoPacketBytes);
      std::memcpy(out + (a - from), packet + (a - p * kXmoPacketBytes), size_t(b - a));
    }
    return true;
  }

  // Sound s's packet headers and frame table, read on first use and kept for the most recently used sounds.
  const Layout* GetLayout(uint32_t s) {
    auto it = layouts_.find(s);
    if (it != layouts_.end()) {
      if (order_.front() != s) {
        order_.remove(s);
        order_.push_front(s);
      }
      return &it->second;
    }
    const PackSound& ps = sounds_[s];
    Layout l;
    l.headers.resize(size_t(ps.packets) * 4);
    std::vector<uint8_t> table(size_t(ps.frames) * 8);
    if (!ReadExactly(*packed_, ps.headers, l.headers.data(), l.headers.size()) ||
        !ReadExactly(*packed_, ps.frame_table, table.data(), table.size()) ||
        !ParseFrames(table.data(), ps.frames, ps.packets, &l.frames)) {
      REXFS_ERROR("[xmo] {}: sound {:08X}'s packet headers or frame table could not be read", rex::path_to_utf8(path_),
                  ps.id);
      return nullptr;
    }
    layout_bytes_ += l.bytes();
    auto& placed = layouts_[s] = std::move(l);
    order_.push_front(s);
    while (layout_bytes_ > kLayoutLimit && order_.size() > 1) {
      const uint32_t old = order_.back();
      order_.pop_back();
      auto o = layouts_.find(old);
      layout_bytes_ -= o->second.bytes();
      layouts_.erase(o);
    }
    return &placed;
  }

  std::unique_ptr<FileHandle> packed_;
  const PackHeader h_;
  const std::vector<PackSegment> segments_;
  const std::vector<PackSound> sounds_;
  std::mutex mutex_;
  std::map<uint32_t, Layout> layouts_;
  std::list<uint32_t> order_;
  size_t layout_bytes_ = 0;
};

}  // namespace

bool HasXmoSuffix(std::string_view name) { return HasSuffix(name, kXmoSuffix); }
bool HasXmopSuffix(std::string_view name) { return HasSuffix(name, kXmopSuffix); }

bool ReadXmoSize(const std::filesystem::path& path, uint64_t* out_size) {
  auto f = FileHandle::OpenExisting(path, FileAccess::kFileReadData);
  Header h;
  if (!f || !ReadHeader(*f, &h)) return false;
  *out_size = h.size;
  return true;
}

bool ReadXmopSize(const std::filesystem::path& path, uint64_t* out_size) {
  auto f = Unpacked(FileHandle::OpenExisting(path, FileAccess::kFileReadData));
  uint8_t raw[kHeaderSize];
  PackHeader h;
  if (!f || !ReadExactly(*f, 0, raw, sizeof(raw)) || !ParsePackHeader(raw, &h)) return false;
  *out_size = h.size;
  return true;
}

std::unique_ptr<FileHandle> OpenXmo(std::unique_ptr<FileHandle> packed) {
  Header h;
  if (!packed || !ReadHeader(*packed, &h)) {
    REXFS_WARN("[xmo] {}: not a valid .xmo file", packed ? rex::path_to_utf8(packed->path()) : std::string("?"));
    return nullptr;
  }
  std::vector<uint8_t> riff(h.data_offset), packets(size_t(h.packets) * 4), table(size_t(h.frames) * 8),
      trailer(h.trailer);
  uint64_t at = kHeaderSize;
  if (!ReadExactly(*packed, at, riff.data(), riff.size()) ||
      !ReadExactly(*packed, at += riff.size(), packets.data(), packets.size()) ||
      !ReadExactly(*packed, at += packets.size(), table.data(), table.size()) ||
      (h.trailer && !ReadExactly(*packed, at += table.size(), trailer.data(), trailer.size()))) {
    REXFS_WARN("[xmo] {}: truncated", rex::path_to_utf8(packed->path()));
    return nullptr;
  }
  std::vector<Frame> frames;
  if (!ParseFrames(table.data(), h.frames, h.packets, &frames)) {
    REXFS_WARN("[xmo] {}: a frame is not valid", rex::path_to_utf8(packed->path()));
    return nullptr;
  }
  Register(packed->path(), h);
  return std::make_unique<XmoHandle>(std::move(packed), h, std::move(riff), std::move(packets), std::move(frames),
                                     std::move(trailer));
}

std::unique_ptr<FileHandle> OpenXmop(std::unique_ptr<FileHandle> packed) {
  if (!packed) return nullptr;
  const std::filesystem::path path = packed->path();
  packed = Unpacked(std::move(packed));  // NAME.xmop.lz4b
  uint64_t file_size = 0;
  if (!packed || !UnpackedSize(path, &file_size)) {
    REXFS_WARN("[xmo] {}: too short to be a .xmop package", rex::path_to_utf8(path));
    return nullptr;
  }
  return OpenXmop(std::move(packed), file_size);
}

// The PopCap ports (2026-11-24): the package as it is and its size given (an entry of the asset pack).
std::unique_ptr<FileHandle> OpenXmop(std::unique_ptr<FileHandle> packed, uint64_t file_size) {
  if (!packed) return nullptr;
  const std::filesystem::path path = packed->path();
  auto bad = [&](const char* why) {
    REXFS_WARN("[xmo] {}: {}", rex::path_to_utf8(path), why);
    return std::unique_ptr<FileHandle>();
  };
  if (file_size < kHeaderSize) return bad("too short to be a .xmop package");
  uint8_t raw[kHeaderSize];
  PackHeader h;
  if (!ReadExactly(*packed, 0, raw, sizeof(raw)) || !ParsePackHeader(raw, &h)) return bad("not a .xmop package");
  auto within = [&](uint64_t offset, uint64_t length) { return offset <= file_size && length <= file_size - offset; };
  if (!within(h.segments_offset, uint64_t(h.segments) * kSegmentSize) || !within(h.stored_offset, h.stored_length) ||
      !within(h.sounds_offset, uint64_t(h.sounds) * kSoundEntrySize)) {
    return bad("its tables are not inside it");
  }
  std::vector<PackSound> sounds(h.sounds);
  {
    std::vector<uint8_t> t(size_t(h.sounds) * kSoundEntrySize);
    if (!t.empty() && !ReadExactly(*packed, h.sounds_offset, t.data(), t.size())) return bad("its sounds could not be read");
    for (uint32_t i = 0; i < h.sounds; ++i) {
      PackSound& s = sounds[i];
      if (!ParsePackSound(t.data() + size_t(i) * kSoundEntrySize, &s) ||
          !within(s.headers, uint64_t(s.packets) * 4) || !within(s.frame_table, uint64_t(s.frames) * 8) ||
          !within(s.opus_table, uint64_t(s.opus_count) * 2)) {
        return bad("a sound of its table is not valid");
      }
    }
  }
  std::vector<PackSegment> segments(h.segments);
  {
    std::vector<uint8_t> t(size_t(h.segments) * kSegmentSize);
    if (!ReadExactly(*packed, h.segments_offset, t.data(), t.size())) return bad("its segments could not be read");
    uint64_t expect = 0;
    for (uint32_t i = 0; i < h.segments; ++i) {
      const uint8_t* p = t.data() + size_t(i) * kSegmentSize;
      PackSegment& s = segments[i];
      s.offset = Le64(p);
      s.length = Le64(p + 8);
      s.kind = Le32(p + 16);
      s.sound = Le32(p + 20);
      s.source = Le64(p + 24);
      bool ok = s.offset == expect && s.length > 0;
      if (s.kind == 0)
        ok = ok && s.source <= h.stored_length && s.length <= h.stored_length - s.source;
      else if (s.kind == 2)
        ok = ok && s.sound < h.sounds &&
             s.source + s.length <= uint64_t(sounds[s.sound].packets) * kXmoPacketBytes;
      else
        ok = ok && s.kind == 1;
      if (!ok) return bad("its segments do not cover the package in order");
      expect += s.length;
    }
    if (expect != h.size) return bad("its segments do not end at its size");
  }
  RegisterPack(path, sounds);
  REXFS_INFO("[xmo] {}: {} of its sounds in Opus, {:.1f} MB as the package it stands for", rex::path_to_utf8(path.filename()),
             sounds.size(), double(h.size) / 1048576.0);
  return std::make_unique<XmopHandle>(std::move(packed), h, std::move(segments), std::move(sounds));
}

bool AnyXmoSound() { return g_any_sound.load(std::memory_order_relaxed); }

void SetXmoSourceOpener(XmoSourceOpener opener) { g_opener.store(opener, std::memory_order_release); }

bool XmoSoundKnown(uint32_t id) {
  std::lock_guard lock(g_mutex);
  return g_sounds.count(id) != 0;
}

std::shared_ptr<const XmoSound> FindXmoSound(uint32_t id) {
  Registered r;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_sounds.find(id);
    if (it == g_sounds.end()) return nullptr;
    if (it->second.sound) {
      if (g_loaded.front() != id) {
        g_loaded.remove(id);
        g_loaded.push_front(id);
      }
      return it->second.sound;
    }
    r = it->second;
  }
  auto s = Load(r);  // outside the lock: a read from the SD
  if (!s) {
    REXFS_WARN("[xmo] {}: sound {:08X}'s Opus data could not be read", rex::path_to_utf8(r.path), id);
    return nullptr;
  }
  std::lock_guard lock(g_mutex);
  auto it = g_sounds.find(id);
  if (it == g_sounds.end()) return s;
  if (!it->second.sound) {
    it->second.sound = s;
    g_loaded.push_front(id);
    g_loaded_bytes += s->opus.size();
    // let go of the least recently used (a decoder still holding one keeps it alive)
    while (g_loaded_bytes > kLoadedLimit && g_loaded.size() > 1) {
      const uint32_t old = g_loaded.back();
      g_loaded.pop_back();
      auto o = g_sounds.find(old);
      if (o != g_sounds.end() && o->second.sound) {
        g_loaded_bytes -= o->second.sound->opus.size();
        o->second.sound.reset();
      }
    }
  }
  return it->second.sound;
}

int64_t FindXmoFrame(const XmoSound& sound, uint64_t bit_pos) {
  auto it = std::lower_bound(sound.frame_pos.begin(), sound.frame_pos.end(), bit_pos);
  return it != sound.frame_pos.end() && *it == bit_pos ? int64_t(it - sound.frame_pos.begin()) : -1;
}

uint32_t XmoTagBits(const uint8_t* packet, uint16_t* bits, uint32_t max) {
  if (packet[3] == 0xFF) return 0;  // no frame starts in it
  const uint32_t first =
      ((uint32_t(packet[0] & 3) << 13) | (uint32_t(packet[1]) << 5) | (uint32_t(packet[2]) >> 3)) + kPacketHeaderBits;
  uint32_t n = 0;
  // the frames that start here, as XmaContext::GetPacketInfo walks them
  for (uint32_t off = first; off < kPacketBits && n < max;) {
    if (kPacketBits - off < kFrameHeaderBits) break;  // a header split across packets
    uint32_t len = 0;
    for (uint32_t k = 0; k < kFrameHeaderBits; ++k) len = (len << 1) | Bit(packet, off + k);
    if (len == 0 || len == kMaxFrameLength) break;
    const uint32_t end = std::min(off + len - 1, kPacketBits);  // the last bit is not a tag bit
    for (uint32_t b = off + kFrameHeaderBits; b < end && n < max; ++b) bits[n++] = uint16_t(b);
    if (off + len > kPacketBits) break;  // it goes on in the next packet
    const uint32_t last = Bit(packet, off + len - 1);
    off += len;
    if (!last) break;
  }
  // then the end of a frame that began in an earlier packet
  for (uint32_t b = kPacketHeaderBits + kFrameHeaderBits; b + 1 < std::min(first, kPacketBits) && n < max; ++b)
    bits[n++] = uint16_t(b);
  return n;
}

bool ReadXmoPacketTag(const uint8_t* packet, uint32_t* id, uint32_t* packet_index) {
  uint16_t bits[kXmoTagBits];
  if (XmoTagBits(packet, bits, kXmoTagBits) < kXmoTagBits) return false;
  uint32_t magic = 0, sound = 0, index = 0;
  for (uint32_t k = 0; k < 16; ++k) magic = (magic << 1) | Bit(packet, bits[k]);
  if (magic != kXmoTagMagic) return false;
  for (uint32_t k = 16; k < 48; ++k) sound = (sound << 1) | Bit(packet, bits[k]);
  for (uint32_t k = 48; k < kXmoTagBits; ++k) index = (index << 1) | Bit(packet, bits[k]);
  *id = sound;
  *packet_index = index;
  return true;
}

}  // namespace rex::filesystem
