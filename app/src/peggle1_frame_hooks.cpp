// Counts the frames the game presents, for the video recorder (peggle1_capture.cpp waits for each new one).
//
// D3DDevice_Swap is sub_824430C8: the only function of default.xex that calls the kernel's VdSwap (found by searching
// the generated code for __imp__VdSwap, 2026-10-21; Peggle 2's was sub_8261FE50). The hook calls the original and
// counts; it changes no PPC register.
//
// How the wrapping works: each recompiled function sub_XXXXXXXX is a weak alias of __imp__sub_XXXXXXXX. The strong
// definition here wins for every caller (direct, and indirect through the function table), and __imp__ is still the
// original.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "peggle1_capture.h"
#include "peggle1_pc_profiler.h"
#if PEGGLE1_NATIVE_RENDERER
#include "me/me_native_system.h"
#endif
#if PEGGLE1_RENUT_RENDERER
#include <thread>

#include "renut_engine/display_sync.h"  // 2026-10-20: banjo_display_sync (PaceRenutSwap)
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/banjo_shared_hooks.h"
#include "me/peggle1_render_mode.h"
#endif

// peggle1_fps = 60 (docs/peggle1-notes.md, "60 frames a second").
//
// The Xbox 360 game presents every second vblank (its Direct3D device's presentation interval is 2), and its frame
// step (the function at 0x82 42F8A8) runs two logic updates of 1/60 s for every frame it draws. Presenting every
// vblank alone would therefore run the game at twice its speed. With the key at 60 the port sets the interval to
// one vblank and the frame step (through tools/fps_updates.py) asks peggle1_fps_updates how many updates the frame
// gets: one for each 1/60 s that passed since the last frame, never fewer than one or more than the game's own two.
// So a frame that took one vblank gets one update, a frame that took two gets two (as on the Xbox 360), and the
// game's speed is the same at 60, at 30 and at anything between; below 30 it slows down as the Xbox 360 game does.
// 2026-10-20 (the user): 60 by default in every build (the deko3d renderer leaves the room); 30 was the default before.
REXCVAR_DEFINE_INT32(peggle1_fps, 0, "Peggle1",
                     "Frames a second the game presents: 60 (every vblank, one logic update per 1/60 s that passed: "
                     "the default) or 30 (the Xbox 360 game: a frame every second vblank, two logic updates per "
                     "frame). The game speed is the same either way");

namespace {

// peggle1-nx (2026-10-21): the device word that holds the presentation interval: +13596 in XDK 7776 as in N&B's 7645
// (out/renut_port/device_offsets.md, kPresentInterval: SynchronizeToPresentationInterval and InitializeApiState read
// it there); Peggle 2's XDK 21256 had it at +14000.
constexpr uint32_t kDeviceIntervalOffset = 13596;

bool Fps60() {
  static const bool on = REXCVAR_GET(peggle1_fps) >= 60 && kDeviceIntervalOffset != 0;
  return on;
}

}  // namespace

// Called by the game's frame step before its second logic update (tools/fps_updates.py): 2 = run it, 1 = skip it.
// Game thread only.
extern "C" int peggle1_fps_updates(void) {
  if (!Fps60()) return 2;
  using Clock = std::chrono::steady_clock;
  constexpr double kUpdate = 1.0 / 60.0;
  static Clock::time_point last = Clock::now();
  static double owed = 0.0;  // game time not yet run, in seconds
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[fps] peggle1_fps = 60: a frame every vblank, one logic update per 1/60 s that passed");
  }
  const Clock::time_point now = Clock::now();
  owed += std::chrono::duration<double>(now - last).count();
  last = now;
  // The frame has run its first update already. A second one when one and a half updates or more are owed (the
  // frame took two vblanks); what stays owed is kept small either way, so a long pause is not caught up later.
  const int updates = owed >= 1.5 * kUpdate ? 2 : 1;
  owed -= updates * kUpdate;
  if (owed > kUpdate) owed = kUpdate;
  if (owed < -kUpdate) owed = -kUpdate;
  // Every 10 s: the frames and the updates of the window (60 updates a second = the game's speed is held).
  static Clock::time_point window = now;
  static uint32_t frames = 0, ran = 0;
  ++frames;
  ran += uint32_t(updates);
  if (const double seconds = std::chrono::duration<double>(now - window).count(); seconds >= 10.0) {
    REXLOG_INFO("[fps] last {:.1f} s: {:.1f} frames and {:.1f} logic updates a second ({} frames with two updates)",
                seconds, frames / seconds, ran / seconds, ran - frames);
    window = now;
    frames = ran = 0;
  }
  return updates;
}

REXCVAR_DEFINE_INT32(peggle1_diag_swap_stack, 0, "Peggle1",
                     "PC diagnostic: log the host call stack (the game's call chain) of this swap; 0 = off");

#if PEGGLE1_RENUT_RENDERER
REXCVAR_DEFINE_BOOL(peggle1_renut_swap_pacing, true, "Peggle1",
                    "Renut's renderer: the Swap waits for the presentation interval the game set (60 Hz vblanks), as "
                    "the console's does; false = frames as fast as they come (timing tests)");

namespace {

// Renut's renderer (peggle1_renderer = "renut") presents without the console's vertical blank, and its Swap does not
// wait for one: the game would run as fast as the host allows (600-1,200 frames a second on the PC with a minimised
// window). The console's Swap waits for the flip, every 'interval' vblanks of 60 Hz (the device word at +14000: the
// game sets 2, peggle1_fps = 60 makes it 1). So this waits for the previous release plus the interval; a frame that is
// already late goes at once (no catching up), as a late flip does.
void PaceRenutSwap(uint32_t interval) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point previous{};
  if (!REXCVAR_GET(peggle1_renut_swap_pacing) || interval == 0) return;
  const auto frame = std::chrono::nanoseconds(16'666'667LL * std::min<uint32_t>(interval, 4));
  const auto now = Clock::now();
  // 2026-10-20 (banjo-nx's banjo_display_sync, as its frame limiter does it): with the display's refresh known (the
  // Switch, the display's vsync event), the release is a refresh plus the measured phase; a late frame goes at once.
  // Otherwise (the key off, the PC, the refresh not known yet) the timer below.
  banjo::display_sync::Start();
  {
    static uint64_t previous_release = 0;
    const auto to_ns = [](Clock::time_point t) {
      return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
    };
    bool anchored = false;
    const uint64_t release = banjo::display_sync::NextRelease(to_ns(now), previous_release, uint64_t(frame.count()),
                                                              &anchored);
    if (release) {
      previous_release = release;
      const Clock::time_point at{std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(release))};
      for (;;) {
        const auto t = Clock::now();
        if (t >= at) break;
        if (at - t > std::chrono::microseconds(400))
          std::this_thread::sleep_for(at - t - std::chrono::microseconds(400));
        else
          std::this_thread::yield();
      }
      banjo::display_sync::NoteRelease(release, anchored);
      previous = std::max(at, now);
      return;
    }
    previous_release = 0;
  }
  const auto due = previous + frame;
  if (previous == Clock::time_point{} || due <= now) {
    previous = now;
    return;
  }
  // The last part is yielded away instead of slept: Windows' sleeps overshoot by up to ~1 ms; Horizon's
  // svcSleepThread is precise, so the console spins only the last 0.4 ms (banjo-nx's frame limiter's tail).
#if REX_PLATFORM_SWITCH
  constexpr auto kTail = std::chrono::microseconds(400);
#else
  constexpr auto kTail = std::chrono::microseconds(1500);
#endif
  for (;;) {
    const auto t = Clock::now();
    if (t >= due) break;
    const auto left = due - t;
    if (left > kTail)
      std::this_thread::sleep_for(left - kTail);
    else
      std::this_thread::yield();
  }
  previous = due;
}

}  // namespace
#endif

extern "C" REX_FUNC(__imp__sub_824430C8);
REX_HOOK_RAW(sub_824430C8) {  // D3DDevice_Swap
  // Which presentation interval the game asks for (1 = every vblank, 2 = every second one), when it changes.
  {
    static uint32_t last_interval = ~0u;
    uint32_t interval_be;
    std::memcpy(&interval_be, base + ctx.r3.u32 + kDeviceIntervalOffset, 4);
    const uint32_t interval = __builtin_bswap32(interval_be);
    if (interval != last_interval) {
      last_interval = interval;
      REXLOG_INFO("[fps] the game's presentation interval is {} (frame {})", interval,
                  g_peggle1_game_frames.load(std::memory_order_relaxed));
    }
  }
  if (const int32_t at = REXCVAR_GET(peggle1_diag_swap_stack);
      at > 0 && g_peggle1_game_frames.load(std::memory_order_relaxed) == static_cast<uint64_t>(at)) {
    peggle1::pc_profiler::LogHostStack("D3DDevice_Swap");
  }
  // The device's presentation interval (the word at +14000: 2 = every second vblank; the swap reads it when it
  // queues the flip, in the function at 0x82 61F990).
  if (Fps60()) {
    uint8_t* const interval = base + ctx.r3.u32 + kDeviceIntervalOffset;
    const uint8_t two[4] = {0, 0, 0, 2}, one[4] = {0, 0, 0, 1};
    if (!std::memcmp(interval, two, 4)) std::memcpy(interval, one, 4);
  }
#if PEGGLE1_RENUT_RENDERER
  if (::banjo::renut_gpu::Active()) {
    uint8_t interval_bytes[4];
    std::memcpy(interval_bytes, base + ctx.r3.u32 + kDeviceIntervalOffset, 4);
    if (!kDeviceIntervalOffset) interval_bytes[3] = 1;
    renut_native_sub_824430C8(ctx, base);  // Renut's Swap (presents)
    const uint64_t frames = g_peggle1_game_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    PaceRenutSwap(interval_bytes[3]);
    peggle1::render::OnPresented(frames);  // PC: the docked / handheld test toggles (the native ring calls it there)
    return;
  }
#endif
  __imp__sub_824430C8(ctx, base);
  g_peggle1_game_frames.fetch_add(1, std::memory_order_relaxed);
#if PEGGLE1_NATIVE_RENDERER
  // The native ring thread waits in a WAIT_REG_MEM for the game's frame pacing word most of each frame
  // (docs/peggle1-notes.md): check it again now rather than at the end of masseffect_native_wait_reg_mem_us.
  me::native::SignalGuestProgress();
#endif
}
