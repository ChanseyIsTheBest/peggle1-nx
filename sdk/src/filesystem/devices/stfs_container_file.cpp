/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/stfs_container_entry.h>
#include <rex/filesystem/devices/stfs_container_file.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>

// rr6-nx (2026-11-18): a check of StfsContainerFile::ReadSync: every read done the old way too (a walk from the
// file's start, a seek and a read a block) and the bytes compared. Off: as it ships.
REXCVAR_DEFINE_BOOL(stfs_read_verify, false, "Filesystem",
                    "A check: every read of a file inside an Xbox 360 package (DLC read in place) is also done block "
                    "by block from the file's start, and the bytes compared; differences logged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::filesystem {

namespace {
// rr6-nx (2026-11-18): a read smaller than this fills the file's read-ahead with this much from where it starts.
constexpr size_t kReadAhead = 256 * 1024;
}  // namespace

StfsContainerFile::StfsContainerFile(uint32_t file_access, StfsContainerEntry* entry)
    : File(file_access, entry), entry_(entry) {}

StfsContainerFile::~StfsContainerFile() = default;

void StfsContainerFile::Destroy() {
  delete this;
}

// rr6-nx (2026-11-18, DLC read in place): an STFS file is a list of 4 KB block records. Each read used to walk the
// list from the file's start (a 57 MB music track: up to 14,000 records) and seek and read each block on its own;
// read in place on the PC, a track's 8 KB reads took up to 574 ms and the race's music had 484 frames of silence in
// 40 s. Here the walk starts at the record the last read ended in (when this read is at or after it), and blocks that
// lie one after another in the package are read with one seek and one read. The same bytes.
size_t StfsContainerFile::ReadRuns(uint8_t* out, size_t byte_offset, size_t length) {
  const auto& list = entry_->block_list();
  size_t i = 0;
  size_t src_offset = 0;  // the file offset record i starts at
  if (byte_offset >= cursor_offset_ && cursor_index_ < list.size()) {
    i = cursor_index_;
    src_offset = cursor_offset_;
  }
  while (i < list.size() && src_offset + list[i].length <= byte_offset) {
    src_offset += list[i].length;  // wholly before the read
    ++i;
  }

  size_t pos = byte_offset;  // the file offset of out
  size_t remaining = length;
  size_t done = 0;
  while (remaining && i < list.size()) {
    const auto& first = list[i];
    const size_t in_first = pos - src_offset;
    // The bytes from pos to the end of record i, and of the records after it that continue it in the same host file.
    size_t run = first.length - in_first;
    for (size_t k = i; run < remaining && k + 1 < list.size() && list[k + 1].file == list[k].file &&
                       list[k + 1].offset == list[k].offset + list[k].length;
         ++k) {
      run += list[k + 1].length;
    }
    const size_t read_length = std::min(run, remaining);

    // rr6-nx (2026-11-21): through the device (its stand-in when the package is read through one: peggle1-nx's
    // P.xmop.lz4b, P.xmop or P.lz4b).
    const size_t num_read = static_cast<StfsContainerDevice*>(entry_->device())
                                ->ReadAt(first.file, first.offset + in_first, out + done, read_length);

    done += num_read;
    pos += num_read;
    remaining -= num_read;
    while (i < list.size() && src_offset + list[i].length <= pos) {
      src_offset += list[i].length;
      ++i;
    }
    if (num_read != read_length) {
      break;
    }
  }
  cursor_index_ = i;
  cursor_offset_ = src_offset;
  return done;
}
// ds3-nx (2026-10-24, sdk-patches/0010; port 57): the data files are the device's shared FILE*s, read with a seek
// then a read: two threads reading at once (games stream on several) could each read from where the other one
// seeked. One read at a time, for every package.
static std::mutex g_stfs_read_mutex;

X_STATUS StfsContainerFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                     size_t* out_bytes_read) {
  std::lock_guard<std::mutex> lock(g_stfs_read_mutex);
  if (byte_offset >= entry_->size()) {
    return X_STATUS_END_OF_FILE;
  }
  // rr6-nx (2026-11-18, DLC read in place) had its own lock here for the same reason: since the base sync (2026-11-22)
  // the one above (port 57) covers the read-ahead too.

  const size_t length = std::min(buffer.size(), entry_->size() - byte_offset);
  if (length >= kReadAhead) {
    *out_bytes_read = ReadRuns(buffer.data(), byte_offset, length);
  } else {
    // rr6-nx (2026-11-18): a small read from the read-ahead, filled from its start when it does not hold the whole
    // read (a music track is read front to back, 8 KB at a time: one host read in 32).
    const bool held = byte_offset >= read_ahead_offset_ &&
                      byte_offset + length <= read_ahead_offset_ + read_ahead_.size();
    if (!held) {
      read_ahead_.resize(std::min(kReadAhead, entry_->size() - byte_offset));
      read_ahead_.resize(ReadRuns(read_ahead_.data(), byte_offset, read_ahead_.size()));
      read_ahead_offset_ = byte_offset;
    }
    const size_t from = byte_offset - read_ahead_offset_;
    const size_t n = from < read_ahead_.size() ? std::min(length, read_ahead_.size() - from) : 0;
    std::memcpy(buffer.data(), read_ahead_.data() + from, n);
    *out_bytes_read = n;
  }

  static const bool verify = REXCVAR_GET(stfs_read_verify);
  if (verify) {
    const auto& list = entry_->block_list();
    std::vector<uint8_t> old(*out_bytes_read);
    size_t old_src = 0, old_pos = 0, left = old.size();
    for (size_t r = 0; r < list.size() && left; ++r) {
      const auto& record = list[r];
      if (old_src + record.length <= byte_offset) {
        old_src += record.length;
        continue;
      }
      const size_t read_offset = byte_offset > old_src ? byte_offset - old_src : 0;
      const size_t n = std::min(record.length - read_offset, left);
      old_pos += static_cast<StfsContainerDevice*>(entry_->device())
                     ->ReadAt(record.file, record.offset + read_offset, old.data() + old_pos, n);
      old_src += record.length;
      left -= n;
    }
    static uint64_t checked = 0, bytes = 0, differed = 0;
    ++checked;
    bytes += old.size();
    if (old_pos != old.size() || std::memcmp(old.data(), buffer.data(), old.size()) != 0) {
      if (++differed <= 8) {
        REXFS_ERROR("[stfs] read of {} B at {} in '{}' DIFFERS from the block-by-block read", old.size(), byte_offset,
                    entry_->name());
      }
    }
    if ((checked & (checked - 1)) == 0) {
      REXFS_INFO("[stfs] stfs_read_verify: {} reads ({} B) checked, {} differed", checked, bytes, differed);
    }
  }

  return X_STATUS_SUCCESS;
}

}  // namespace rex::filesystem
