#pragma once

// PC sampling profiler (cvar peggle1_pc_profile_s); see peggle1_pc_profiler.cpp. Does nothing on the Switch, where the
// SDK's sampler (sdk/src/ui/switch_perf.cpp) does the same job.
namespace peggle1::pc_profiler {

void Start();
void Stop();

// Logs the calling thread's host call stack (its functions by name: a recompiled function as sub_XXXXXXXX), for
// questions like "who calls the game's swap". PC only; slow (it loads the symbols at the first call).
void LogHostStack(const char* what);

}  // namespace peggle1::pc_profiler
