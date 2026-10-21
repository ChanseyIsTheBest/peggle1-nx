// Peggle (from sa2-nx, 2026-10-13): the PC side of the post-codegen steps of the second CPU round (docs/enhancements.md, "The
// second CPU round of 2026-10-12"). The Switch build has no code at any of these places: the macros of the generated
// pch are the bare operation there. Off the Switch each of them keeps the old operation next to the new one:
//   - tools/ro_constants.py: a load from read-only data folded to its constant still loads, counts itself
//     (g_rex_ro_checked) and calls RexRoMismatch when the memory does not hold the constant;
//   - tools/indirect_calls.py: a promoted indirect call counts itself (g_rex_promoted_calls) and calls
//     RexPromotedMismatch when the dispatch table's entry for the guarded target is not the function called;
//   - the census of indirect calls (peggle1_diag_indirect_census = a file): per call site how often it ran and where to,
//     the input of tools/indirect_promotions.py;
//   - the SDK's kernel_critsec_pcr_owner: every critical section call compares the owner id it took from the PCR
//     with the looked-up thread's (rex/sys_counters.h, kCensusCritSecOwnerMismatch);
//   - banjo-nx's codegen round (2026-10-19): tools/lr_preserve.py's link-register write-backs proven redundant
//     (REX_LR_WRITE_SAME_CHECK: compared with ctx.lr, RexLrWriteMismatch) and tools/r1_local.py's r1 mirror after
//     assumed calls (REX_R1_CHECK: compared with ctx.r1, RexR1Mismatch); ro_constants_cfg.py's folds use RexRoMismatch.
// So every PC run tests what it executes; the '[verify]' lines of the 10 s report carry the counts.
#include "peggle1_enhancements.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/sys_counters.h>

#include <cstring>

REXCVAR_DEFINE_STRING(peggle1_diag_indirect_census, "", "Peggle1",
                      "Diagnostic (PC): a file that gets, with every '[hot] 10 s' report, the census of the game's "
                      "indirect calls (per call site: calls and targets). tools/indirect_promotions.py makes "
                      "app/indirect_promotions.txt from it. Empty = no census")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Diagnostic, from sasrt-nx's CPU round through sa2-nx (PC builds: the SDK's sys_counters.h counts nothing of this on
// the Switch): how often the guest switches the host's float flush mode (an `msr fpcr` on the A57) and reaches the
// runtime's lock-and-syscall paths, per second, with each "[hot] 10 s" report. It tells what is worth making cheaper
// on the console, which a PC profile cannot show.
REXCVAR_DEFINE_BOOL(peggle1_diag_runtime_census, false, "Peggle1",
                    "Diagnostic (PC): flush-mode switches and calls of the host runtime's clock, critical section, "
                    "event, semaphore, wait and handle-lookup paths per second, in [census] log lines with each "
                    "[hot] 10 s report")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx's CPU round (2026-10-12, through sa2-nx): build-time work with a run-time side.
PEGGLE1_RING_ENHANCEMENT(g_guest_cas_no_barrier, peggle1_cpu_guest_cas_no_barrier, true,
                         "Compare-and-swap (the game's interlocked operations, tools/guest_cas.py, and the SDK's lock "
                         "words, rex/thread/atomic.h) without the legacy builtin's trailing dmb ish on the A57; off "
                         "puts the barrier back at run time");
#if defined(__aarch64__)
extern "C" unsigned char g_rex_atomic_legacy_barrier;  // sdk/src/core/atomic_posix.cpp
#endif

#if !REX_PLATFORM_SWITCH

extern "C" {
// tools/flush_mode_flow.py: off the Switch every flush-mode check the control flow proved redundant keeps its test
// (REX_FLUSH_KNOWN_ON / _OFF in the generated pch), counts itself here and calls RexFlushFlowMismatch when the proof
// was wrong, before the mode is switched after all. So every PC run checks the proofs it executes.
volatile unsigned long long g_rex_flush_flow_checked = 0;  // a plain lossy count: "at least this many"
// rex/ppc/diet_ops.h, REX_DIET_CENSUS_ON: executions of the any-mode float helpers in the game code.
volatile unsigned long long g_rex_diet_census[6] = {};
volatile unsigned long long g_rex_ro_checked = 0;      // plain lossy counts: "at least this many"
volatile unsigned long long g_rex_promoted_calls = 0;
unsigned char g_rex_indirect_census = 0;
// banjo-nx's codegen round (2026-10-19): tools/r1_local.py (the r1 mirror after kept or assumed calls, REX_R1_CHECK)
// and tools/lr_preserve.py --rewrite (link-register write-backs proven redundant, REX_LR_WRITE_SAME_CHECK).
volatile unsigned long long g_rex_r1_checked = 0;
volatile unsigned long long g_rex_lr_write_checked = 0;
}

namespace {

std::atomic<uint64_t> g_flush_flow_mismatches{0};
std::atomic<uint64_t> g_ro_mismatches{0};
std::atomic<uint64_t> g_promoted_mismatches{0};
std::atomic<uint64_t> g_r1_mismatches{0};
std::atomic<uint64_t> g_lr_write_mismatches{0};

// One call site: (function address << 32) | ordinal. Up to four targets are told apart; the rest is "other".
struct Site {
  uint64_t total = 0;
  uint32_t target[4] = {};
  uint64_t count[4] = {};
  uint64_t other = 0;
};
constexpr size_t kStripes = 64;
struct Stripe {
  std::mutex mutex;
  std::unordered_map<uint64_t, Site> sites;
};
Stripe g_stripes[kStripes];

}  // namespace

extern "C" void RexFlushFlowMismatch(int on) noexcept {
  ::peggle1::enhancements::VerifyDifference();
  if (g_flush_flow_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enh] DIFFERENCE: flush-mode flow: a check proven redundant had to switch the mode {} (return "
                 "address {})",
                 on ? "on" : "off", static_cast<const void*>(__builtin_return_address(0)));
  }
}

extern "C" void RexRoMismatch(unsigned address) noexcept {
  ::peggle1::enhancements::VerifyDifference();
  if (g_ro_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enh] DIFFERENCE: read-only constants: guest memory at {:08X} does not hold the constant the "
                 "generated code uses",
                 address);
  }
}

// tools/r1_local.py (from banjo-nx): after a call whose callee is only assumed (or proven) to return with r1 unchanged,
// the local mirror differs from ctx.r1: a callee moved the guest stack pointer. Counted, reported; the mirror is
// reloaded by the macro.
extern "C" void RexR1Mismatch(unsigned long long expected, unsigned long long got) noexcept {
  ::peggle1::enhancements::VerifyDifference();
  if (g_r1_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enh] DIFFERENCE: r1 mirror: a callee returned with r1 {:#x}, the caller's mirror holds {:#x} "
                 "(return address {})",
                 got, expected, static_cast<const void*>(__builtin_return_address(0)));
  }
}

// tools/lr_preserve.py --rewrite (from banjo-nx): a link-register write-back proven to store the value ctx.lr already
// holds did not. The macro writes it after the report, as the original does.
extern "C" void RexLrWriteMismatch(unsigned long long held, unsigned long long written) noexcept {
  ::peggle1::enhancements::VerifyDifference();
  if (g_lr_write_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enh] DIFFERENCE: link-register write-back proven redundant: ctx.lr {:#x}, the epilogue writes "
                 "{:#x} (return address {})",
                 held, written, static_cast<const void*>(__builtin_return_address(0)));
  }
}

extern "C" void RexPromotedMismatch(unsigned target) noexcept {
  ::peggle1::enhancements::VerifyDifference();
  if (g_promoted_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enh] DIFFERENCE: promoted indirect call: the dispatch table's entry for {:08X} is not the "
                 "function the generated code calls",
                 target);
  }
}

extern "C" void RexIndirectCensus(unsigned function, unsigned site, unsigned target) noexcept {
  const uint64_t key = (uint64_t(function) << 32) | site;
  Stripe& stripe = g_stripes[(key * 0x9E3779B97F4A7C15ull) >> 58];
  std::lock_guard<std::mutex> lock(stripe.mutex);
  Site& s = stripe.sites[key];
  ++s.total;
  for (int i = 0; i < 4; ++i) {
    if (s.target[i] == target || s.count[i] == 0) {
      s.target[i] = target;
      ++s.count[i];
      return;
    }
  }
  ++s.other;
}

namespace peggle1::enhancements {

void CodegenChecksInit() {
#if defined(__aarch64__)
  // peggle1_cpu_guest_cas_no_barrier: off puts the legacy dmb ish back after every compare-and-swap.
  g_rex_atomic_legacy_barrier = g_guest_cas_no_barrier.active ? 0 : 1;
#endif
  g_rex_indirect_census = REXCVAR_GET(peggle1_diag_indirect_census).empty() ? 0 : 1;
  if (g_rex_indirect_census) {
    REXLOG_INFO("[census] indirect calls: every tagged call site reports its target; the table goes to {} with each "
                "report",
                REXCVAR_GET(peggle1_diag_indirect_census));
  }
}

void CodegenChecksReport(int seconds) {
  // tools/flush_mode_flow.py's self-check: the proven checks this run executed, and those that were wrong.
  if (const uint64_t proven = g_rex_flush_flow_checked) {
    const uint64_t wrong = g_flush_flow_mismatches.load(std::memory_order_relaxed);
    if (wrong) {
      REXLOG_ERROR("[enh] DIFFERENCE: flush-mode flow: {} of {} checks proven redundant had to switch the mode "
                   "(cumulative)",
                   wrong, proven);
    } else {
      REXLOG_INFO("[verify] flush-mode flow: {} checks proven redundant executed so far, 0 had to switch the mode",
                  proven);
    }
  }
  if (REXCVAR_GET(peggle1_diag_runtime_census)) {
    // The SDK's counters (rex/sys_counters.h), as calls per second.
    using namespace rex::syscount;
    static uint64_t last[kCensusCount] = {};
    const auto per_s = [&](unsigned id) {
      const uint64_t now = CensusRead(id);
      const uint64_t d = now - last[id];
      last[id] = now;
      return double(d) / double(seconds);
    };
    const double clock = per_s(kClockCall), clock_blocked = per_s(kClockBlocked);
    const double enter = per_s(kCensusCritSecEnter), recursive = per_s(kCensusCritSecRecursive);
    const double try_enter = per_s(kCensusTryEnter), cs_wait = per_s(kCritSecWait);
    const double set = per_s(kCensusEventSet), sem = per_s(kCensusSemRelease), wait = per_s(kCensusWait);
    const double lookup = per_s(kObjLookup), lookup_blocked = per_s(kObjBlocked);
    const double native = per_s(kCensusNativeObject), stamp = per_s(kTimestampTick);
    const double flush = per_s(kCensusFlushWrite), flush_same = per_s(kCensusFlushSame);
    REXLOG_INFO("[census] guest code per second: {:.0f} flush-mode switches (each an msr fpcr on the console; "
                "{:.0f} of them unconditional writes that left the mode as it was)",
                flush, flush_same);
    // The rounding mode of the guest's FPSCR writes, cumulative (from banjo-nx: tools/fp_sign_ops.py is exact only
    // while the game never selects round-up / round-down; rex/ppc/context.h counts each write's mode).
    REXLOG_INFO("[census] FPSCR rounding mode written by the guest (cumulative): to nearest {}, toward zero {}, up or "
                "down {}",
                CensusRead(kCensusRoundNearest), CensusRead(kCensusRoundZero), CensusRead(kCensusRoundUpDown));
    {
      static uint64_t last_proven = 0;
      const uint64_t proven_now = g_rex_flush_flow_checked;
      const double proven_s = double(proven_now - last_proven) / double(seconds);
      last_proven = proven_now;
      REXLOG_INFO("[census] proven flush checks executed per second: {:.0f}", proven_s);
      // rex/ppc/diet_ops.h, REX_DIET_CENSUS_ON (the PC build): what the any-mode forms of the generator's
      // fp_compares_modeless / fp_mode_lookahead / fp_copy_pairs options run instead of a mode switch.
      static uint64_t last_diet[6] = {};
      double diet[6];
      for (int i = 0; i < 6; ++i) {
        const uint64_t now = g_rex_diet_census[i];
        diet[i] = double(now - last_diet[i]) / double(seconds);
        last_diet[i] = now;
      }
      REXLOG_INFO("[census] any-mode float helpers per second: conversions {:.0f}, compares {:.0f}, arithmetic "
                  "{:.0f}, cold paths {:.0f}; float copy forms (fp_copy_pairs): loads {:.0f}, stores {:.0f}",
                  diet[0], diet[1], diet[2], diet[3], diet[4], diet[5]);
    }
    {
      const double logs = per_s(kCensusLogCall), heaps = per_s(kCensusLookupHeap);
      const double refs = per_s(kCensusRetainRelease), states = per_s(kCensusKernelState);
      const double pcr_owner = per_s(kCensusCritSecPcrOwner);
      REXLOG_INFO("[census] runtime overhead per second: {:.0f} log statements reached (written or not), {:.0f} "
                  "heap lookups for a pointer translation, {:.0f} object retains and releases, {:.0f} kernel "
                  "state reads; {:.0f} critical section owners from the PCR",
                  logs, heaps, refs, states, pcr_owner);
    }
    {
      // The kernel exports the game calls most (rex/hook.h counts every REX_EXPORT in PC builds).
      static uint64_t last_exports = 0;
      const char* names[16];
      uint64_t counts[16];
      const size_t n = ExportCensus(names, counts, 16);
      uint64_t shown = 0;
      std::string list;
      for (size_t i = 0; i < n; ++i) {
        list += fmt::format(" {}:{}", names[i] + (std::strncmp(names[i], "__imp__", 7) == 0 ? 7 : 0), counts[i]);
        shown += counts[i];
      }
      if (n) {
        REXLOG_INFO("[census] kernel exports by calls (cumulative, the 16 most called; {:.0f} a second of them):{}",
                    double(shown - last_exports) / double(seconds), list);
      }
      last_exports = shown;
    }
    REXLOG_INFO("[census] runtime calls per second: guest clock {:.0f} ({:.0f} found its mutex taken); critical "
                "sections entered {:.0f} ({:.0f} already owned, {:.0f} tried, {:.0f} waited); events set {:.0f}, "
                "semaphores released {:.0f}, waits {:.0f}; handle lookups {:.0f} ({:.0f} found the global lock "
                "taken), dispatcher objects looked up {:.0f}; timestamp ticks {:.0f}",
                clock, clock_blocked, enter, recursive, try_enter, cs_wait, set, sem, wait, lookup, lookup_blocked,
                native, stamp);
  }
  // The self-checks: what this run executed, and what was wrong (must stay 0).
  const uint64_t ro = g_rex_ro_checked, promoted = g_rex_promoted_calls;
  const uint64_t pcr = rex::syscount::CensusRead(rex::syscount::kCensusCritSecPcrOwner);
  const uint64_t ro_wrong = g_ro_mismatches.load(std::memory_order_relaxed);
  const uint64_t promoted_wrong = g_promoted_mismatches.load(std::memory_order_relaxed);
  const uint64_t pcr_wrong = rex::syscount::CensusRead(rex::syscount::kCensusCritSecOwnerMismatch);
  if (ro || promoted || pcr) {
    if (ro_wrong || promoted_wrong || pcr_wrong) {
      REXLOG_ERROR("[enh] DIFFERENCE: generated-code self-checks (cumulative): read-only constants {} of {} wrong, "
                   "promoted indirect calls {} of {} wrong, critical section owners from the PCR {} of {} wrong",
                   ro_wrong, ro, promoted_wrong, promoted, pcr_wrong, pcr);
    } else {
      REXLOG_INFO("[verify] generated-code self-checks so far: {} loads folded to read-only constants, {} promoted "
                  "indirect calls, {} critical section owners from the PCR; 0 differences",
                  ro, promoted, pcr);
    }
  }
  {  // banjo-nx's codegen round: tools/r1_local.py and tools/lr_preserve.py
    const uint64_t r1 = g_rex_r1_checked, lr = g_rex_lr_write_checked;
    const uint64_t r1_wrong = g_r1_mismatches.load(std::memory_order_relaxed);
    const uint64_t lr_wrong = g_lr_write_mismatches.load(std::memory_order_relaxed);
    if (r1_wrong || lr_wrong) {
      REXLOG_ERROR("[enh] DIFFERENCE: generated-code self-checks (cumulative): r1 mirror {} of {} wrong, redundant "
                   "link-register write-backs {} of {} wrong",
                   r1_wrong, r1, lr_wrong, lr);
    } else if (r1 || lr) {
      REXLOG_INFO("[verify] r1 mirror checked after {} calls, {} link-register write-backs proven redundant checked; "
                  "0 differences",
                  r1, lr);
    }
  }
  if (!g_rex_indirect_census) return;
  // The census: the whole table, most called sites first.
  struct Row {
    uint64_t key;
    Site site;
  };
  std::vector<Row> rows;
  for (Stripe& stripe : g_stripes) {
    std::lock_guard<std::mutex> lock(stripe.mutex);
    for (const auto& [key, site] : stripe.sites) rows.push_back({key, site});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return a.site.total != b.site.total ? a.site.total > b.site.total : a.key < b.key;
  });
  uint64_t calls = 0, single = 0, single_calls = 0, top_calls = 0;
  for (const Row& r : rows) {
    calls += r.site.total;
    const uint64_t top = *std::max_element(r.site.count, r.site.count + 4);
    top_calls += top;
    if (top == r.site.total) {
      ++single;
      single_calls += r.site.total;
    }
  }
  static uint64_t last_calls = 0;
  REXLOG_INFO("[census] indirect calls: {:.0f} per second; {} sites seen so far with {} calls, {} sites ({} calls) "
              "always went to one target, {:.1f} % of all calls went to their site's most frequent target",
              double(calls - last_calls) / double(seconds), rows.size(), calls, single, single_calls,
              calls ? 100.0 * double(top_calls) / double(calls) : 0.0);
  last_calls = calls;
  const std::string path = REXCVAR_GET(peggle1_diag_indirect_census);
  const std::string temp = path + ".tmp";
  if (FILE* f = std::fopen(temp.c_str(), "w")) {
    std::fprintf(f, "# peggle2-nx indirect call census (cumulative): <function> <site> <calls> <target>:<calls>... "
                    "other:<calls>\n");
    for (const Row& r : rows) {
      std::fprintf(f, "%08X %u %llu", unsigned(r.key >> 32), unsigned(r.key & 0xFFFFFFFFu),
                   static_cast<unsigned long long>(r.site.total));
      for (int i = 0; i < 4; ++i) {
        if (r.site.count[i]) {
          std::fprintf(f, " %08X:%llu", r.site.target[i], static_cast<unsigned long long>(r.site.count[i]));
        }
      }
      std::fprintf(f, " other:%llu\n", static_cast<unsigned long long>(r.site.other));
    }
    std::fclose(f);
    std::remove(path.c_str());
    std::rename(temp.c_str(), path.c_str());
  } else {
    REXLOG_WARN("[census] indirect calls: cannot write {}", temp);
  }
}

}  // namespace peggle1::enhancements

#else  // the Switch build: nothing to check, nothing to count

namespace peggle1::enhancements {
void CodegenChecksInit() {
#if defined(__aarch64__)
  // peggle1_cpu_guest_cas_no_barrier: off puts the legacy dmb ish back after every compare-and-swap.
  g_rex_atomic_legacy_barrier = g_guest_cas_no_barrier.active ? 0 : 1;
#endif
}
void CodegenChecksReport(int) {}
}  // namespace peggle1::enhancements

#endif
