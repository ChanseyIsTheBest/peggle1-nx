// peggle2-nx (2026-10-19): the part of banjo-nx's enhancement API that Renut's renderer (src/renut_engine/gpu, taken
// from banjo-nx with its banjo_renut_* tuning) calls. Defined in src/renut_engine/peggle1_renut_glue.cpp on Peggle 2's
// own switches: an enhancement is active when its own key is on and the renderer's group is not switched off.
#pragma once

namespace banjo {

enum class EnhancementGroup { kCpu, kNonCpu };

// Own key AND the group's master switch (peggle1_cpu_enhancements / the non-CPU group's).
bool EnhancementActive(EnhancementGroup group, bool own_key);

// The PC side of the generated code's self-checks (src/cpu/peggle1_codegen_checks.cpp), with every 10 s report.
void CodegenChecksReport(int seconds);

}  // namespace banjo
