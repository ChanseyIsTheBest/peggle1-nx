// The calling thread's own CPU time (peggle2-nx), for the ring timer (me_native_system.cpp, "[native] ring thread"):
// its wall-clock figure includes the waits for the host GPU and the presentation, so on the PC it hardly moves when
// the ring's CPU work does. Kept out of the renderer's file so windows.h stays here.
#include "me_thread_cpu.h"

#include <cstdint>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__SWITCH__)
#include <switch.h>
#else
#include <time.h>
#endif

namespace me::native {

uint64_t ThreadCpuNs() {
#if defined(_WIN32)
  // User + kernel time in 100 ns units (updated at each scheduler tick: over a 10 s report the error is a few ms).
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  const auto as64 = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return (as64(kernel) + as64(user)) * 100;
#elif defined(__SWITCH__)
  // The thread's tick count on all cores (19.2 MHz system ticks), as sdk/src/ui/switch_perf.cpp reads it.
  u64 ticks = 0;
  if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX))) return 0;
  return armTicksToNs(ticks);
#else
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

}  // namespace me::native
