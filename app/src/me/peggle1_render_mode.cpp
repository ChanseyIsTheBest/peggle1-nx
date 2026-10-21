// Peggle 2: render resolution per console mode (see peggle1_render_mode.h).
#include "peggle1_render_mode.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/switch_saltynx.h>

#include "../peggle1_enhancements.h"

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

REXCVAR_DEFINE_INT32(peggle1_render_height_handheld, 720, "Peggle1",
                     "Render height in handheld mode (the PC's starting mode unless peggle1_render_docked): 720 = the "
                     "game's own 1280x720. Any multiple of 180 from 360 to 2160 (others are rounded to the nearest); "
                     "the width follows at 16:9")
    .range(180, 4320)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(peggle1_render_height_docked, 1080, "Peggle1",
                     "Render height in docked mode (the TV): 1080 = 1920x1080, 720 = the game's own 1280x720. Any "
                     "multiple of 180 from 360 to 2160; switching between docked and handheld changes it at the next "
                     "frame")
    .range(180, 4320)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(peggle1_render_docked, false, "Peggle1",
                    "PC only: start in the simulated docked mode (peggle1_render_height_docked). The "
                    "bind_peggle1_docked key (F9) switches between docked and handheld while the game runs, like "
                    "docking the Switch. The Switch itself always follows the console");
REXCVAR_DEFINE_INT32(peggle1_render_test_toggle_at, 0, "Peggle1",
                     "Test (PC): toggle the simulated docked mode after this presented swap (0 = never)")
    .range(0, 2000000000);
REXCVAR_DEFINE_INT32(peggle1_render_test_toggle_every, 0, "Peggle1",
                     "Test (PC): after peggle1_render_test_toggle_at, toggle again every this many presented swaps "
                     "(0 = once)")
    .range(0, 2000000000);

// Non-CPU group: it changes the picture on purpose (more pixels), so it is a key like every other change.
PEGGLE1_RING_GPU_ENHANCEMENT(g_render_scale, peggle1_gpu_render_scale, true,
                             "GPU: render at peggle1_render_height_handheld / peggle1_render_height_docked (720 / 1080 "
                             "by default), following docked / handheld live; off = always the game's 1280x720");

namespace peggle1::render {
namespace {

std::atomic<bool> g_docked_pc{false};
std::atomic<bool> g_docked_pc_set{false};
std::atomic<uint32_t> g_last_logged_q{0};

// q = height / 180, from 2 (360p) to 12 (2160p); other heights go to the nearest multiple of 180.
uint32_t QuartersOf(int32_t height) {
  const int32_t q = (std::clamp(height, 360, 2160) + 90) / 180;
  return uint32_t(std::clamp(q, 2, 12));
}

[[maybe_unused]] bool DockedPc() {
  if (!g_docked_pc_set.load(std::memory_order_acquire)) {
    bool expected = false;
    if (g_docked_pc_set.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
      g_docked_pc.store(REXCVAR_GET(peggle1_render_docked), std::memory_order_release);
  }
  return g_docked_pc.load(std::memory_order_acquire);
}

}  // namespace

bool Docked() {
#if REX_PLATFORM_SWITCH
  // libnx keeps the operation mode cached; the SDK's applet loop processes the OperationModeChanged message that
  // updates it. Reverse-NX's "fake" mode is obeyed, as the window does.
  return rex::ui::switch_saltynx::BaseMode(appletGetOperationMode() == AppletOperationMode_Console);
#else
  return DockedPc();
#endif
}

uint32_t WantedScaleQuarters() {
  if (!g_render_scale.active) return 4;
  const bool docked = Docked();
  const int32_t height =
      docked ? REXCVAR_GET(peggle1_render_height_docked) : REXCVAR_GET(peggle1_render_height_handheld);
  const uint32_t q = QuartersOf(height);
  const uint32_t state = q | (docked ? 0x100u : 0u);
  if (g_last_logged_q.exchange(state, std::memory_order_relaxed) != state) {
    REXLOG_INFO("[res] {} mode: render {}x{} (peggle1_render_height_{} = {}{})", docked ? "docked" : "handheld",
                320 * q, 180 * q, docked ? "docked" : "handheld", height,
                int32_t(180 * q) != height ? std::string(", rounded to a multiple of 180") : std::string());
  }
  return q;
}

void ToggleDocked() {
#if !REX_PLATFORM_SWITCH
  const bool docked = !DockedPc();
  g_docked_pc.store(docked, std::memory_order_release);
  REXLOG_INFO("[res] simulated {} (PC): render height {}", docked ? "docking" : "undocking",
              docked ? REXCVAR_GET(peggle1_render_height_docked) : REXCVAR_GET(peggle1_render_height_handheld));
#endif
}

void OnPresented(uint64_t presented) {
#if !REX_PLATFORM_SWITCH
  const int32_t at = REXCVAR_GET(peggle1_render_test_toggle_at);
  if (at <= 0 || presented < uint64_t(at)) return;
  const int32_t every = REXCVAR_GET(peggle1_render_test_toggle_every);
  if (presented == uint64_t(at) || (every > 0 && (presented - uint64_t(at)) % uint64_t(every) == 0)) {
    REXLOG_INFO("[res] test toggle at presented swap {}", presented);
    ToggleDocked();
  }
#else
  (void)presented;
#endif
}

void NoteRescale() { g_render_scale.Use(); }

void RegisterKeys() {
#if !REX_PLATFORM_SWITCH
  rex::ui::RegisterBind("bind_peggle1_docked", "F9",
                        "Switch between the simulated docked and handheld modes (render resolution)",
                        [] { ToggleDocked(); });
#endif
}

}  // namespace peggle1::render
