// peggle2-nx (2026-10-19): banjo-nx's on-screen profiler rows that Renut's frame statistics feed
// (src/renut_engine/perf/frame_stats.cpp). Peggle 2 has no such window: nothing to feed.
#pragma once

namespace banjo::profiler {
inline void SetGpuFrameMs(double) {}
inline void RecordHostPresent() {}
inline void SetRenderThreadMs(double) {}
}  // namespace banjo::profiler
