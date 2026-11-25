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

#pragma once

#include <span>

#include <vector>

#include <rex/filesystem/file.h>

namespace rex::filesystem {

class StfsContainerEntry;

class StfsContainerFile : public File {
 public:
  StfsContainerFile(uint32_t file_access, StfsContainerEntry* entry);
  ~StfsContainerFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> /*buffer*/, size_t /*byte_offset*/,
                     size_t* /*out_bytes_written*/) override {
    return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t /*length*/) override { return X_STATUS_ACCESS_DENIED; }

 private:
  // rr6-nx (2026-11-18, DLC read in place): 'length' bytes from 'byte_offset' into 'out', run by run (blocks next to
  // each other in the package in one host read), from the cursor when it is at or before the read. Returns the bytes
  // read.
  size_t ReadRuns(uint8_t* out, size_t byte_offset, size_t length);

  StfsContainerEntry* entry_;
  // rr6-nx (2026-11-18, DLC read in place): the block record holding the last read's end, and the file offset that
  // record starts at: a read that goes on from there starts there (a music track is read front to back).
  size_t cursor_index_ = 0;
  size_t cursor_offset_ = 0;
  // rr6-nx (2026-11-18): small reads are served from here: up to 256 KB of the file from read_ahead_offset_.
  std::vector<uint8_t> read_ahead_;
  size_t read_ahead_offset_ = 0;
};

}  // namespace rex::filesystem
