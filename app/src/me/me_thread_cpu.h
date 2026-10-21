// The calling thread's own CPU time in nanoseconds (0 when the platform cannot say). See me_thread_cpu.cpp.
#pragma once

#include <cstdint>

namespace me::native {

uint64_t ThreadCpuNs();

}  // namespace me::native
