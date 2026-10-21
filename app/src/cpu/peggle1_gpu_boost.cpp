// Peggle: the handheld GPU profile (non-CPU enhancement peggle1_gpu_boost; sa2-nx's sa2_gpu_boost).
//
// Asks the console's performance manager for the official handheld configuration 0x92220008 (GPU 460.8 MHz, memory
// 1331.2 MHz; the same as sa2-nx, UnleashedRecomp-NX, MarathonRecomp-NX and masseffect-nx), through the SDK's
// switch_apm.cpp, which checks with the real clocks for 1.5 s that the memory clock did not move and reverts if it
// did. Docked: nothing is requested.
// WHY IT IS EXACT: a clock, not a setting: the same frames are drawn, sooner.
// RISK: more power draw in handheld mode. Whether Peggle 2 needs it is not measured (no console profile yet): the
// "GPU per Swap" lines of a console run with and without the key say.
// KEY: peggle1_gpu_boost (with peggle1_noncpu_enhancements).
#include <rex/logging.h>
#include <rex/platform.h>

#include "peggle1_enhancements.h"

#if REX_PLATFORM_SWITCH
extern "C" {
void RexSwitchApmRequestGpuMhz(int mhz);
void RexSwitchApmApply(void);
}
#endif

PEGGLE1_RING_GPU_ENHANCEMENT(g_gpu_boost, peggle1_gpu_boost, true,
                             "Handheld: ask for the official 460.8 MHz GPU profile (memory clock checked unchanged, "
                             "else reverted)");

namespace peggle1::enhancements {

void ApplyGpuBoost() {
  if (!g_gpu_boost.active) return;
#if REX_PLATFORM_SWITCH
  RexSwitchApmRequestGpuMhz(460);
  RexSwitchApmApply();  // logs what it did on stderr (rex_stderr.log)
  g_gpu_boost.Use();
#endif
}

}  // namespace peggle1::enhancements
