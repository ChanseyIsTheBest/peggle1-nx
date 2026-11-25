/**
 * rr6-nx (2026-11-10, the user's request): game files stored as LZ4 blocks (*.lz4b), read as the files they were.
 *
 * The installer page packs Ridge Racer 6's three RRM archives (3 GB) with LZ4 HC level 9 in 256 KB blocks: about a
 * fifth smaller on the SD card, decompressed at a few ms per MB on the console. A file NAME.lz4b in a host folder is
 * the game's NAME, with NAME's size (HostPathEntry::Create); opening it wraps the host handle in one that decompresses
 * the blocks a read covers (OpenLz4Blocks). A plain NAME next to it wins: the .lz4b is then left as an ordinary file.
 *
 * rr6-nx (2026-11-11, the user's hybrid): version 2 holds ZXC blocks (ZXC 0.15, sdk/thirdparty/zxc): the installer
 * packs the RRM archives as LZ4 HC 9 in 256 KB blocks (what loads read: LZ4 1.10.0's decoder, ~590 MB/s on the console)
 * and every other game file (the videos, music and packs, read while they play) as ZXC 7 in 512 KB blocks (smaller;
 * ~200 MB/s). The console runs of 2026-11-11 (docs/progress-log.md) measured both.
 *
 * The format, little-endian:
 *   header, 32 bytes:  "RXLZ4BLK", u32 version (1: LZ4 blocks; 2: ZXC blocks), u32 block size, u64 size of the file it
 *                      holds, u32 block count, u32 0
 *   the blocks:        each block's LZ4 block-format data (version 1) or zxc_compress_block output (version 2) back to
 *                      back; a block whose stored size equals its own length is stored as it is (peggle2-nx's HD pack
 *                      rule)
 *   the index:         u32 per block, its stored size
 *   footer, 16 bytes:  u64 offset of the index, "RXLZ4END"
 * Every block is 'block size' long but the last. The footer lets the page write the file as a stream.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include <rex/filesystem.h>

namespace rex::filesystem {

inline constexpr std::string_view kLz4BlocksSuffix = ".lz4b";

// Whether a host file name ends in .lz4b (any case).
bool HasLz4BlocksSuffix(std::string_view name);

// The size of the file an .lz4b file holds, from its header. False if it is not one.
bool ReadLz4BlocksSize(const std::filesystem::path& path, uint64_t* out_size);

// A handle that reads the file `packed` holds, or nullptr when `packed` is not a valid .lz4b file (logged).
std::unique_ptr<FileHandle> OpenLz4Blocks(std::unique_ptr<FileHandle> packed);
// sa2-nx-ult (2026-11-24): the same for a handle that is not a host file of its own (an entry of the asset pack,
// rex/filesystem/devices/pack_device.h): its size is given, not taken from the host path.
std::unique_ptr<FileHandle> OpenLz4Blocks(std::unique_ptr<FileHandle> packed, uint64_t packed_size);

}  // namespace rex::filesystem
