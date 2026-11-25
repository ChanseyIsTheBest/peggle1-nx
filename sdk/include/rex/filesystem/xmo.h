/**
 * XMA sounds whose frames are Opus, shown to the game as the XMA they were.
 *
 * culdcept-nx (2026-11-20, credit culdcept-nx; the user: re-encode the game's audio to Opus): NAME.xmo is a game's XMA
 * file NAME whose frames are Opus. The game sees NAME as it was (the same size, RIFF header, packet headers, frame
 * length fields and frames' last bits, and its trailer), but the frames' other bits are zero except a tag in each packet
 * a frame starts in: the XMA emulation (sdk/src/audio/xma_context.cpp, XmaContext::DecodeXmoFrame) finds it and takes
 * the frame's 512 samples from the sound's Opus stream instead of decoding XMA bits. Loops, subframes and the output
 * buffer stay the emulation's own: only the samples come from elsewhere.
 *
 * sasrt-nx (2026-11-21, the user: "please compress all the videos and audio", "you can take them from culdcept
 * directly"): SASRT's XMA is XMA2 inside Wwise packages (the .pck files in Data/sound: AKPK, big-endian; each sound a RIFX WEM,
 * streamed or inside a sound bank), many sounds to a file. NAME.xmop is such a package NAME with every XMA2 sound worth
 * it in Opus: NAME's bytes as segments (stored, zero, or a sound's synthesized packets), each sound's packets made as
 * NAME.xmo makes a file's (the same tag, tag bits and frame table), its sound id from the package's sound table. A
 * sound whose Opus is not smaller stays XMA (stored bytes). sasrt-nx-installer's lib/xmop.js writes it.
 *
 * NAME.xmo (little-endian; culdcept-nx-installer's lib/xmo.js):
 *   header, 64 bytes:  "RXXMAOP1", u32 version (1), u32 sound id (the tag's), u64 size of NAME, u32 data offset (where
 *                      NAME's XMA packets start: after its RIFF header), u32 packet count, u32 frame count, u32 sample
 *                      rate (48000), u16 channels, u16 Opus pre-skip, u32 Opus packet count, u64 offset of the Opus
 *                      packet table, u32 trailer size (NAME's bytes after its packets: the XMA files' "seek" chunk),
 *                      u32 0
 *   NAME's first `data offset` bytes (its RIFF header, as it was)
 *   the packet headers: packet count x 4 bytes, as they were (a packet is 2048 bytes of NAME from the data offset)
 *   the frames: frame count x 8 bytes: u32 bit position in NAME's data (packet headers counted), u16 length in bits (the
 *               frame's own 15-bit field), u8 its last bit ("another frame follows in this packet"), u8 0
 *   the trailer: NAME's last `trailer size` bytes, as they were
 *   the Opus packets: u16 size each (count of them), then the packets back to back (20 ms each, 48 kHz)
 *
 * NAME.xmop (little-endian; sasrt-nx-installer's lib/xmop.js):
 *   header, 64 bytes:  "RXXMAPK1", u32 version (1), u32 segment count, u64 size of NAME, u64 offset of the segments,
 *                      u64 offset of the stored bytes, u64 length of the stored bytes, u32 sound count, u32 0,
 *                      u64 offset of the sound table
 *   segments, 32 bytes each, in NAME's order, back to back from 0 to NAME's size:
 *                      u64 offset in NAME, u64 length, u32 kind (0: stored bytes, 1: zeros, 2: a sound's packets),
 *                      u32 sound (kind 2: its index in the sound table), u64 source (kind 0: offset in the stored
 *                      bytes; kind 2: the segment's first byte as an offset in the sound's packets)
 *   sound table, 64 bytes a sound:
 *                      u32 sound id (the tag's: unique among every .xmo and .xmop the game has), u32 packet count,
 *                      u32 frame count, u32 sample rate (the XMA's: 24000, 44100 or 48000), u16 channels (1 or 2),
 *                      u16 Opus pre-skip (in samples at the Opus rate), u32 Opus packet count, u32 Opus rate (48000 or
 *                      24000; 48000 for a 44100 sound: the Opus was made from the sound resampled to 48 kHz and the
 *                      port resamples it back), u32 0, u64 offset of the packet headers (packet count x 4 bytes, as they
 *                      were), u64 offset of the frame table (as NAME.xmo's: bit positions in the sound's packets), u64
 *                      offset of the Opus packet table (u16 size each, then the packets back to back, 20 ms each),
 *                      u64 0
 *   then the stored bytes and every table and Opus packet, wherever the offsets say
 * A sound's packets are its WEM's data chunk: packet count x 2048 bytes from the data chunk's first byte (Wwise's XMA2
 * WEMs are single-stream, 2048-byte packets). The tag's packet index counts from the sound's first packet.
 *
 * The tag, 72 bits: 16 bits 0xA55C, 32 bits the sound id, 24 bits the packet's index in the sound, MSB first, in the
 * packet's tag bits (XmoTagBits): walking the frames that start in the packet as the emulation does (from the header's
 * first frame offset, while a frame's last bit says another follows), each one's bits after its 15-bit length field and
 * before its last bit, as far as the packet's end; then the packet's bits from 47 (after the header and the 15 bits a
 * frame header split across packets can put there) to the bit before its first frame (the end of a frame that began in
 * an earlier packet). A sound is kept as XMA when a packet a frame starts in has fewer than 72.
 *
 * A frame is the one at (the tag's packet, the frame's bit in it): its index k in the frame table. Its samples are
 * [512 k, 512 k + 512) of the sound (at the XMA's rate) after the Opus pre-skip: what FFmpeg's xmaframes decoder gave
 * for that frame when the frames were decoded in order (the installer made the Opus from that).
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace rex::filesystem {
class FileHandle;

inline constexpr std::string_view kXmoSuffix = ".xmo";
inline constexpr std::string_view kXmopSuffix = ".xmop";  // sasrt-nx (2026-11-21)
inline constexpr uint16_t kXmoTagMagic = 0xA55C;
inline constexpr uint32_t kXmoTagBits = 16 + 32 + 24;
inline constexpr uint32_t kXmoPacketBytes = 2048;

bool HasXmoSuffix(std::string_view name);
bool HasXmopSuffix(std::string_view name);
// The size of the file a NAME.xmo / NAME.xmop stands for (its header only), or false.
bool ReadXmoSize(const std::filesystem::path& path, uint64_t* out_size);
bool ReadXmopSize(const std::filesystem::path& path, uint64_t* out_size);
// The file NAME, synthesized from NAME.xmo / NAME.xmop (`packed`, read only); null when it is not valid.
std::unique_ptr<FileHandle> OpenXmo(std::unique_ptr<FileHandle> packed);
std::unique_ptr<FileHandle> OpenXmop(std::unique_ptr<FileHandle> packed);
// The PopCap ports (2026-11-24, as sa2-nx-ult's OpenLz4Blocks overload): the .xmop package itself (read as it is: an
// .lz4b already opened) and its size given, for an entry of the asset pack (no host file of its own).
std::unique_ptr<FileHandle> OpenXmop(std::unique_ptr<FileHandle> package, uint64_t size);
// The PopCap ports (2026-11-24): opens a package again by the path its reader was made with, for the Opus data a
// sound reads later (an asset pack's entry is no file on the SD card). None set, or null back: the host file.
using XmoSourceOpener = std::unique_ptr<FileHandle> (*)(const std::filesystem::path& path);
void SetXmoSourceOpener(XmoSourceOpener opener);

// A sound's Opus stream and frame positions, for the XMA emulation.
struct XmoSound {
  uint32_t id = 0;
  uint32_t sample_rate = 48000;  // the XMA's
  uint32_t opus_rate = 48000;    // the Opus's (48000 for a 44100 sound: resampled to the XMA's rate when decoded)
  uint16_t channels = 0;
  uint16_t pre_skip = 0;              // samples at opus_rate
  std::vector<uint32_t> frame_pos;    // each frame's bit position in the sound's data, ascending
  std::vector<uint8_t> opus;          // the packets back to back
  std::vector<uint32_t> opus_offset;  // each packet's offset in `opus`, and its end as the last entry
};
// Whether a NAME.xmo / NAME.xmop the game has opened has this sound id (a tag naming an unknown id is not one: XMA bits).
bool XmoSoundKnown(uint32_t id);
// Base port 259: whether any NAME.xmo / NAME.xmop has been registered (XmaContext::DecodeXmoFrame asks this first, so a
// game without them pays one load a frame, not a walk of the packet's tag bits).
bool AnyXmoSound();
// The sound with this id, its Opus data read if it is not held (the game's read of the file's data reads it, on the
// game's thread); null if unknown or unreadable.
std::shared_ptr<const XmoSound> FindXmoSound(uint32_t id);
// The index of the frame at `bit_pos` of the sound's data, or -1.
int64_t FindXmoFrame(const XmoSound& sound, uint64_t bit_pos);

// The tag bits of a packet (its 2048 bytes: the header, frame length fields and last bits as in the file): up to `max`
// bit positions in it, in order; how many.
uint32_t XmoTagBits(const uint8_t* packet, uint16_t* bits, uint32_t max);
// The tag of a packet: true with the sound id and the packet's index when it has one.
bool ReadXmoPacketTag(const uint8_t* packet, uint32_t* id, uint32_t* packet_index);

}  // namespace rex::filesystem
