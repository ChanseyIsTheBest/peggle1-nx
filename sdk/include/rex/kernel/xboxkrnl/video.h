/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>

namespace rex::runtime {
class ExportResolver;
}

namespace rex::system {
class KernelState;
}

namespace rex::kernel::xboxkrnl {

void VdQueryVideoMode(system::X_VIDEO_MODE* video_mode);

// From sa2-nx's SDK (deterministic runs to compare renderers, cvar vblank_per_swap): the guest's VdSwap calls so far.
uint64_t VdSwapCount();
// vblank_per_swap: the vblank thread delivers vblank_per_swap_count vblanks per frame the game swaps instead of one per
// 1/60 s, so game logic that ticks per vblank no longer depends on the host's speed.
bool VblankPerSwap();
// For VblankPerSwap(): waits until the next vblank is due (vblank_per_swap_count of them per swap since `last_swap`,
// updated) or the timeout passed (a guest that stopped swapping, e.g. while loading); false once `running` clears.
bool WaitSwapForVblank(uint64_t& last_swap, const std::atomic<bool>& running);

// Register video variable exports (VdGlobalDevice, VdHSIOCalibrationLock, etc.)
// Must be called during kernel initialization before XEX modules are loaded.
void RegisterVideoExports(rex::runtime::ExportResolver* export_resolver,
                          system::KernelState* kernel_state);

}  // namespace rex::kernel::xboxkrnl
