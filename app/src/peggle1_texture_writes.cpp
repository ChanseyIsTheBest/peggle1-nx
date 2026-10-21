// Peggle 2: the game's texture loader tells the renderer which texture it has just written. See the header.

#include "peggle1_texture_writes.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include "me/me_native_system.h"
#if PEGGLE1_RENUT_RENDERER
#include "renut_engine/gpu/banjo_renut_select.h"
#endif

// The loader's row copy (r3 = its job: +40 -> the texture wrapper, whose +4 -> the Direct3D texture; +44 first row,
// +48 rows, +52 the source; +1080 bytes per row). It has no direct caller: the function table's entry is what runs.
extern "C" REX_FUNC(__imp__sub_8245E890);
// The Bink video handle's update (r3 = the handle: +32 the BINK, +76 its two sets of plane textures). When a new video
// frame is due it starts the decode of the next one, into the plane set the frame just submitted has drawn. A virtual:
// no direct caller.
extern "C" REX_FUNC(__imp__sub_8248CA60);

#if PEGGLE1_RENUT_RENDERER
// render/textures.h (Renut's renderer): the CPU has written the texture at the address; read again at its next use.
namespace renut::gpu {
void NoteTextureUnlocked(uint32_t resource_va);
namespace render_thread {
void Drain();  // backend/render_thread.h: waits until everything queued so far has run
}
}
#endif

namespace peggle1::texture_writes {

namespace {

// Host address of a guest address (the recompiled code's REX_RAW_ADDR).
const uint8_t* Host(const uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Host(base, address), 4);
  return __builtin_bswap32(v);
}

bool Pointer(uint32_t v) { return v >= 0x10000u && v < 0xFFFF0000u; }

void RowCopy(PPCContext& ctx, uint8_t* base) {
  const uint32_t job = ctx.r3.u32;
  // Read before the call: the routine does not change them, and the job may be gone when it returns.
  uint32_t data = 0, texture = 0;
  if (Pointer(job)) {
    const uint32_t holder = Load32(base, job + 40);
    const uint32_t wrapper = Pointer(holder) ? Load32(base, holder + 4) : 0;
    texture = Pointer(wrapper) ? Load32(base, wrapper) : 0;
    // The Direct3D texture's base address word: the page number the GPU is given, with flag bits below it.
    if (Pointer(texture)) data = Load32(base, texture + 32) & 0xFFFFF000u;
  }
  __imp__sub_8245E890(ctx, base);
#if PEGGLE1_RENUT_RENDERER
  // Renut's renderer: the texture is read again at its next use (render/textures.cpp, peggle1_renut_texture_backoff).
  if (::banjo::renut_gpu::Active()) {
    if (Pointer(texture)) renut::gpu::NoteTextureUnlocked(texture);
    return;
  }
#endif
  // The address the GPU is given for it: Direct3D's own conversion (the 4 KB-page view at E0000000 starts one page
  // into physical memory).
  if (data) me::native::NoteTextureWritten((data & 0x1FFFFFFFu) + (((data >> 20) + 0x200u) & 0x1000u));
}

// peggle1_gpu_video_after_draws (see the key in src/me/me_native_system.cpp): the ring thread first.
void VideoUpdate(PPCContext& ctx, uint8_t* base) {
#if PEGGLE1_RENUT_RENDERER
  if (::banjo::renut_gpu::Active()) {
    // Renut's renderer reads a frame's texture data on its render thread after the Swap (one frame may still be
    // queued), and the decode writes the plane set the frame just submitted draws: the render thread finishes what is
    // queued first, as the native renderer waits for its ring (the spots in videos of 2026-10-13). Videos only.
    renut::gpu::render_thread::Drain();
    __imp__sub_8248CA60(ctx, base);
    return;
  }
#endif
  me::native::WaitRingRecordedDraws();
  __imp__sub_8248CA60(ctx, base);
}

}  // namespace

void Install() {
  static std::atomic<bool> installed{false};
  if (installed.load(std::memory_order_acquire)) return;
  // Written as a sum: tools/direct_calls.py takes a function address spelled out in app code for a hooked function.
  const uint32_t row_copy = (0x4122F000u << 1) + 0x890u;
  auto* const runtime = rex::Runtime::instance();
  auto* const dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  if (!dispatcher || !dispatcher->HasAnyFunctionTable() || !dispatcher->GetFunction(row_copy)) return;
  if (installed.exchange(true)) return;
  const bool set = dispatcher->SetFunction(row_copy, &RowCopy);
  REXLOG_INFO("[native] texture writes: the loader's row copy (function table entry {:08X}) {}", row_copy,
              set ? "reports the texture it wrote to the renderer (peggle1_gpu_lock_rechecks)"
                  : "could NOT be replaced: a reloaded atlas waits for its recheck interval");
  const uint32_t video_update = (0x41246000u << 1) + 0xA60u;
  const bool set_video = dispatcher->GetFunction(video_update) && dispatcher->SetFunction(video_update, &VideoUpdate);
  REXLOG_INFO("[native] texture writes: the video update (function table entry {:08X}) {}", video_update,
              set_video ? "waits for the ring thread before it starts a decode (peggle1_gpu_video_after_draws)"
                        : "could NOT be replaced: video planes may be read while they are decoded");
}

}  // namespace peggle1::texture_writes
