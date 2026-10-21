// Peggle 2: render resolution per console mode (peggle1_render_height_handheld, peggle1_render_height_docked;
// docs/peggle1-notes.md "Docked 1080p"). From sa2-nx (sa2_render_mode.h, 2026-10-07), without its MSAA option.
//
// The native renderer asks WantedScaleQuarters() once per Swap (ring thread) and, when it changed, rescales every
// render target and resolved texture at that frame boundary (masseffect_native_targets.cpp, RescaleAll). On the Switch
// the mode is the console's (docked / handheld, Reverse-NX respected), read from libnx's cached operation mode, which
// the SDK's applet loop keeps current. On the PC it is simulated: peggle1_render_docked picks the starting mode, the
// bind_peggle1_docked key (F9) toggles it, and peggle1_render_test_toggle_at / _every toggle it at presented swaps.
#pragma once

#include <cstdint>

namespace peggle1::render {

// The render scale for the current mode, in quarters of the game's 1280x720 (q = height / 180: 4 = 720p, 6 = 1080p).
// 4 when peggle1_gpu_render_scale (or the non-CPU master) is off. Cheap: called once per Swap.
uint32_t WantedScaleQuarters();

// The current mode (the console's on the Switch, the simulated one on the PC).
bool Docked();

// PC simulation of docking: flips the simulated mode (the hotkey). No effect on the Switch.
void ToggleDocked();

// Ring thread, after each presented swap: the PC test toggles (peggle1_render_test_toggle_at / _every).
void OnPresented(uint64_t presented);

// Counted in the enhancement report: a rescale (live switch or start-up).
void NoteRescale();

// PC: registers the dock-toggle key (bind_peggle1_docked). Called once by the app after setup.
void RegisterKeys();

}  // namespace peggle1::render
