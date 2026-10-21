// Peggle's enhancements and their switches (see peggle1_enhancements.cpp).

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

namespace peggle1::enhancements {

// Applies the two group master switches (a group that is off has the keys of its members turned off, before anything
// reads them) and logs the [enhancements] line. Called once, from Peggle1App::OnPostInitLogging.
void Apply();

// A native replacement of a guest function (src/cpu/peggle1_hot_guest.cpp, from sa2-nx): a CPU enhancement with its
// own key, registered at static initialisation by PEGGLE1_NATIVE. `active` (own key and CPU master) is resolved by
// Apply(), before any guest thread runs; a hot path pays one byte load for it.
struct Native {
  const char* key;
  const char* what;
  bool (*own)();
  // false: a guest function replacement (under peggle1_cpu_natives too); true: a renderer enhancement registered by
  // PEGGLE1_RING_ENHANCEMENT (src/me, from sa2-nx's SA2_ENHANCEMENT): under the CPU master only.
  bool ring = false;
  // A renderer enhancement of the non-CPU group (PEGGLE1_RING_GPU_ENHANCEMENT: sa2-nx's kNonCpu keys, GPU work and
  // fidelity fixes): under the non-CPU master instead of the CPU one.
  bool gpu = false;
  bool active = false;
  std::atomic<uint64_t> uses{0};
  void Use(uint64_t n = 1) { uses.fetch_add(n, std::memory_order_relaxed); }
  // For per-draw / per-call hot paths (from sa2-nx): a plain relaxed load and store (no read-modify-write; racing
  // threads may lose a count, it is only a report), and one shared add per 256 uses.
  std::atomic<uint32_t> sampled{0};
  void UseSampled() {
    const uint32_t n = sampled.load(std::memory_order_relaxed) + 1;
    sampled.store(n, std::memory_order_relaxed);
    if ((n & 255) == 0) Use(256);
  }
};
bool RegisterNative(Native* n);

// Verify mode (peggle1_cpu_verify): every call of a native replacement is checked. The checks and differences of the
// natives are counted here; a difference also counts into the renderer's [verify] line (me/me_verify.h).
bool VerifyMode();
// The same as a plain byte (set by Apply() before any guest thread runs), for checks on per-call hot paths: the
// localized copies' smart-sync check (tools/localize_native.py) reads it at every call site.
inline bool g_verify_mode = false;
void VerifyCheck(uint64_t n = 1);
void VerifyDifference();
// Time spent in the native replacements' guarded calls (peggle1::hot::Check), for the 10 s "[hot]" report.
void CheckTime(uint64_t ns);

// True when the loaded guest code at `address` (`bytes` long) has the FNV-1a 64 hash a native was written from
// (tools/guest_code_hash.py). Logs a warning naming `what` when it does not.
bool GuestCodeMatches(const uint8_t* base, uint32_t address, uint32_t bytes, uint64_t fnv, const char* what);

// sa2-nx's CPU rounds (2026-10-13). The PC side of the generated code's self-checks and the indirect call census
// (src/cpu/peggle1_codegen_checks.cpp; on the Switch only the compare-and-swap key). Init: once, from Apply(). Report:
// with every "[hot] 10 s" report.
void CodegenChecksInit();
void CodegenChecksReport(int seconds);
// Runs every helper that has a form for one processor only (the conversions to integer, the divisions, the float
// copies, the small VMX helpers) on fixed edge inputs against plain C++ references and logs the result
// (src/cpu/peggle1_forms_selfcheck.cpp). Once, from Apply(): on the console it is the first time the AArch64 forms run.
void FormsSelfCheck();
// The handheld GPU profile (peggle1_gpu_boost, src/cpu/peggle1_gpu_boost.cpp; a no-op on the PC and when docked).
// Once, from Peggle1App::OnPreSetup, after Apply().
void ApplyGpuBoost();

// The 10-second "[hot]" report: uses of every native since the last report, and the verify counts. Started by the
// app after setup.
void StartReport();
void StopReport();

}  // namespace peggle1::enhancements

#define PEGGLE1_NATIVE(VAR, KEY, DESC)                                                     \
  REXCVAR_DEFINE_BOOL(KEY, true, "Peggle1 natives", DESC);                                 \
  ::peggle1::enhancements::Native VAR{#KEY, DESC, [] { return REXCVAR_GET(KEY); }};       \
  [[maybe_unused]] static const bool VAR##_registered_ = ::peggle1::enhancements::RegisterNative(&VAR)

// A renderer CPU enhancement with its own key (sa2-nx's SA2_ENHANCEMENT for the files of src/me taken from sa2-nx):
// `VAR.active` = its key and the CPU master, resolved by Apply() before the renderer starts; listed in the
// [enhancements] line and the configs (tools/make_enhancement_configs.py reads these macros).
#define PEGGLE1_RING_ENHANCEMENT(VAR, KEY, DEFAULT, DESC)                                     \
  REXCVAR_DEFINE_BOOL(KEY, DEFAULT, "Peggle1 ring enhancements", DESC);                       \
  ::peggle1::enhancements::Native VAR{#KEY, DESC, [] { return REXCVAR_GET(KEY); }, true};    \
  [[maybe_unused]] static const bool VAR##_registered_ = ::peggle1::enhancements::RegisterNative(&VAR)

// The same for a renderer enhancement of the non-CPU group (sa2-nx's SA2_ENHANCEMENT with kNonCpu: GPU work, fidelity
// fixes): `VAR.active` = its key and the non-CPU master.
#define PEGGLE1_RING_GPU_ENHANCEMENT(VAR, KEY, DEFAULT, DESC)                                       \
  REXCVAR_DEFINE_BOOL(KEY, DEFAULT, "Peggle1 ring enhancements", DESC);                             \
  ::peggle1::enhancements::Native VAR{#KEY, DESC, [] { return REXCVAR_GET(KEY); }, true, true};    \
  [[maybe_unused]] static const bool VAR##_registered_ = ::peggle1::enhancements::RegisterNative(&VAR)
