// Peggle 2: data checks along the path of a texture (peggle1_diag_textures, off by default).
//
// Console run of 2026-10-13: large atlases showed garbage in parts while the PC was right, and nothing in the logs
// said at which stage the bytes went wrong. With the key on, three stages write what they saw:
//   - the game's own inflate: every stream's length, CRC-32 and Adler-32, taken where zlib hands the decoded bytes
//     to its check function ("[texdiag] stream" lines);
//   - the renderer: every texture of 1 MB or more whose content is new, the CRC-32 of the guest bytes it is about to
//     upload, whole and in 64 slices ("[texdiag] texture" lines), and the first few large ones written to
//     <executable folder>/texdiag/ as they were in guest memory;
//   - the file reads are the SDK's part (io_diag_crc, sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp).
// tools/texdiag_check.py compares all three with the archive on the PC.
#pragma once

#include <cstddef>
#include <cstdint>

namespace peggle1::texdiag {

bool Enabled();

// Once, after the guest image is loaded and its functions are registered.
void Install();

// The renderer found new content at a texture's guest address (first use or changed): `bytes` of guest memory at
// `raw`, before any untiling or byte order change.
void NoteTexture(uint32_t physical_address, uint32_t width, uint32_t height, uint32_t format, const uint8_t* raw,
                 size_t bytes, bool was_uploaded_before, uint32_t pool_block);

// The renderer copied a texture's new content out of guest memory in ring frame `frame`; `changed_meanwhile` if
// the guest bytes no longer had the fingerprint the copy was started for (the game wrote them during the read).
void NoteRead(uint32_t physical_address, uint32_t width, uint32_t height, uint32_t format, uint64_t frame,
              bool changed_meanwhile);

// After a read during which the guest bytes changed: the renderer's copy against the guest memory now.
void NoteDifference(uint32_t physical_address, uint32_t width, uint32_t height, const uint8_t* copy,
                    const uint8_t* guest, size_t bytes);

}  // namespace peggle1::texdiag
