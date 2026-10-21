// Peggle 2 - native replacements of hot guest functions (CPU enhancements peggle1_cpu_native_*; from sa2-nx's
// sa2_hot_guest.cpp, which took it from masseffect-nx's me_hot_guest.cpp).
//
// Every hook replaces one recompiled function by a native version (src/cpu/hot/n_<addr>.h, or a localized copy in
// app/generated/hot/n_<addr>.h) and checks itself: for the first peggle1_hot_guard_calls calls of each thread (every
// call with peggle1_cpu_verify) and every peggle1_hot_guard_period-th call afterwards, the native version runs on a copy
// of the registers with the guest ranges it writes snapshotted, the memory is rolled back, the original
// (__imp__sub_X) runs for real, and registers, written ranges and the FPU mode are compared. A difference logs
// "[hot] DIFFERENCE ..." and switches that hook off for the rest of the run (the original already produced the correct
// state). A hook also stays off when the loaded game code is not byte for byte the code the native was written from.
//
// WHY EXACT: the CRT natives reproduce the routines' memory effect and return register (n_825F0AE0.h etc. say why);
//   the others are localized copies of the recompiled functions themselves (tools/localize_native.py).
// RISK: a native that differs only on inputs the guard did not see. KEY: each peggle1_cpu_native_* (CPU group).

// The generated code's macros and helpers (the localized copies are made of generated code) ...
#include "../../generated/default/peggle1_pch.h"

// ... with plain guest memory: the recompiled code's accesses are volatile, which keeps GCC from pairing and
// scheduling them; a native replacement does not need that (a racing write from another thread is a race either way).
#undef REX_LOAD_U8
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#define REX_LOAD_U8(x) (*(u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U16(x) __builtin_bswap16(*(u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U32(x) __builtin_bswap32(*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U64(x) __builtin_bswap64(*(u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_STORE_U8(x, y) (*(u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))
#define REX_STORE_U16(x, y) (*(u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))

#include "cpu/peggle1_hot_guest.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <unordered_map>

#include "peggle1_enhancements.h"

namespace peggle1::hot {
// The store log of a checked call of a leaf localized copy (NativeLogged, tools/localize_native.py): every guest
// store first records the bytes it overwrites.
void LogBefore(uint8_t* base, uint32_t addr, uint32_t len);
}  // namespace peggle1::hot

#define PEGGLE1_LOG_STORE_U8(x, y) (::peggle1::hot::LogBefore(base, (u32)(x), 1), REX_STORE_U8(x, y))
#define PEGGLE1_LOG_STORE_U16(x, y) (::peggle1::hot::LogBefore(base, (u32)(x), 2), REX_STORE_U16(x, y))
#define PEGGLE1_LOG_STORE_U32(x, y) (::peggle1::hot::LogBefore(base, (u32)(x), 4), REX_STORE_U32(x, y))
#define PEGGLE1_LOG_STORE_U64(x, y) (::peggle1::hot::LogBefore(base, (u32)(x), 8), REX_STORE_U64(x, y))
// A register of the context for peggle1::hot::SmartCall (the localized copies' smart-sync check).
#define PEGGLE1_SPAN(r)   ::peggle1::hot::RegSpan { uint16_t(__builtin_offsetof(PPCContext, r)), uint16_t(sizeof(PPCContext::r)) }
// D-form stores (codegen dform_disp_split): the address is reg + disp; it differs from (u32)(reg + disp) only when the
// sum crosses 2^32, a guest fault either way.
#define PEGGLE1_LOG_STORE_D_U8(r, d, y) (::peggle1::hot::LogBefore(base, (u32)((u32)(r) + (d)), 1), REX_STORE_D_U8(r, d, y))
#define PEGGLE1_LOG_STORE_D_U16(r, d, y) (::peggle1::hot::LogBefore(base, (u32)((u32)(r) + (d)), 2), REX_STORE_D_U16(r, d, y))
#define PEGGLE1_LOG_STORE_D_U32(r, d, y) (::peggle1::hot::LogBefore(base, (u32)((u32)(r) + (d)), 4), REX_STORE_D_U32(r, d, y))
#define PEGGLE1_LOG_STORE_D_U64(r, d, y) (::peggle1::hot::LogBefore(base, (u32)((u32)(r) + (d)), 8), REX_STORE_D_U64(r, d, y))
#define PEGGLE1_LOG_STORE_V128(ea, value)                                  \
  do {                                                                     \
    ::peggle1::hot::LogBefore(base, (u32)(ea), 16);                        \
    simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), value);          \
  } while (0)
#define PEGGLE1_LOG_ZERO(ea, n)                                            \
  do {                                                                     \
    ::peggle1::hot::LogBefore(base, (u32)(ea), n);                         \
    memset((void*)REX_RAW_ADDR(ea), 0, n);                                 \
  } while (0)
// A compiler-only barrier at every loop header of a localized copy: guest memory is read without volatile there, and
// a load must not be hoisted out of a loop (the Mushihimesama port's console hang). Registers in locals stay put.
#define PEGGLE1_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")

// Hand-written natives (C runtime routines).
#include "cpu/hot/n_825F0AE0.h"
#include "cpu/hot/n_825F10B0.h"

// The guard is off by default since 2026-10-14 (before: the first 256 calls of each thread, then 1 in 4096). The user,
// on the stutter its checks of the Wwise mixing copies caused: "it's a release build do we really need self-checks?".
// The natives are proven with peggle1_cpu_verify (every call checked) and the PC's runs; a play build only keeps the
// one check of each counter's first call (c == 0 below), which is free. Set the two keys to check more.
REXCVAR_DEFINE_INT32(peggle1_hot_guard_calls, 0, "Peggle1 natives",
                     "Native replacements: the first N calls of each thread are checked against the recompiled original "
                     "(0 = none; peggle1_cpu_verify checks every call)")
    .range(0, 1000000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(peggle1_hot_guard_period, 0, "Peggle1 natives",
                     "Native replacements: after the first N calls every P-th call is checked (rounded to a power of "
                     "two; 0 = none)")
    .range(0, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace peggle1::hot {

// From sa2-nx (sa2_cpu_hook_counter_shared). A native-hook setting, so under peggle1_cpu_natives too.
PEGGLE1_NATIVE(g_counter_shared, peggle1_cpu_hook_counter_shared,
               "Native replacements: one relaxed call counter per hook shared by all threads instead of a thread_local "
               "one (on the Switch every thread_local access is a call to __aarch64_read_tp, on every hooked call)");

// sasrt-nx's, through sa2-nx (2026-10-12).
PEGGLE1_NATIVE(g_counter_ctx, peggle1_cpu_hook_counter_ctx,
               "Native replacements: each hook's call counter lives in the calling thread's own context "
               "(PPCContext::host_counters) instead of one counter shared by all threads: no store into a cache line "
               "every calling thread pulls to its core, and still no thread_local");

void TakeCtxSlot(std::atomic<uint8_t>& slot) {
  if (slot.load(std::memory_order_relaxed) != kNoSlot) return;
  static std::atomic<uint32_t> next_slot{0};
  const uint32_t s = next_slot.fetch_add(1, std::memory_order_relaxed);
  uint8_t none = kNoSlot;
  slot.compare_exchange_strong(none, s < kCtxSlots ? uint8_t(s) : kSlotsFull, std::memory_order_relaxed);
}

PEGGLE1_NATIVE(g_smart_sync, peggle1_cpu_natives_smart_sync,
               "Native replacements: a localized copy's call writes back and reloads only the registers its callee can "
               "reach (smart sync, from sa2-nx; verify mode checks every such call) instead of all of them");

void SmartChecked() {
  ::peggle1::enhancements::VerifyCheck();
  g_smart_sync.Use();
}

void SmartCall(PPCContext& ctx, uint8_t* base, PPCFunc* callee, const RegSpan* rest, int n, uint32_t copy,
               const char* name) {
  SmartChecked();
  const std::unique_ptr<PPCContext> before(new PPCContext(ctx));
  callee(ctx, base);
  const auto* b = reinterpret_cast<const uint8_t*>(before.get());
  const auto* c = reinterpret_cast<const uint8_t*>(&ctx);
  for (int i = 0; i < n; ++i) {
    if (std::memcmp(b + rest[i].offset, c + rest[i].offset, rest[i].size) != 0) {
      SmartDifference(copy, name);
      return;
    }
  }
}

void SmartDifference(uint32_t copy, const char* callee) {
  ::peggle1::enhancements::VerifyDifference();
  static std::mutex mutex;
  static std::set<std::pair<uint32_t, std::string>> logged;
  std::lock_guard<std::mutex> lock(mutex);
  if (logged.size() < 32 && logged.insert({copy, callee}).second)
    REXLOG_ERROR("[hot] DIFFERENCE: smart sync: in the copy of sub_{:08X}, {} wrote a register its computed reach "
                 "leaves out (tools/localize_native.py); peggle1_cpu_natives_smart_sync = false is the old path",
                 copy, callee);
}

void InitHook(Hook& h, uint8_t* base) {
  // May run on several threads at once for the first call: every writer stores the same values.
  bool on = h.key->active;
  if (on && !peggle1::enhancements::GuestCodeMatches(base, h.code_address, h.code_bytes, h.code_hash, h.key->key)) on = false;
  const bool verify = peggle1::enhancements::VerifyMode();
  const uint32_t guard_calls = verify ? 0xFFFFFFFFu : uint32_t(std::max(0, REXCVAR_GET(peggle1_hot_guard_calls)));
  const uint32_t period = uint32_t(std::max(0, REXCVAR_GET(peggle1_hot_guard_period)));
  uint32_t p2 = 1;
  while (p2 < period) p2 <<= 1;
  h.guard_calls = guard_calls;
  // period 0: no periodic checks; (c & mask) == 0 then holds only for a counter's first call (c == 0).
  h.period_mask = period == 0 ? 0xFFFFFFFFu : p2 - 1;
  TakeCtxSlot(h.slot);  // peggle1_cpu_hook_counter_ctx
  if (on && h.state.load(std::memory_order_relaxed) == kUninit) {
    if (verify) {
      REXLOG_INFO("[hot] {} ({}): native replacement ON, verify mode (every call checked)", h.name, h.key->key);
    } else if (guard_calls == 0 && period == 0) {
      REXLOG_INFO("[hot] {} ({}): native replacement ON (guard off: one check at a thread's first call)", h.name,
                  h.key->key);
    } else {
      REXLOG_INFO("[hot] {} ({}): native replacement ON (guard: first {} calls, then 1 in {})", h.name, h.key->key,
                  guard_calls, p2);
    }
  }
  h.state.store(on ? kOn : kOff, std::memory_order_release);
}

namespace {

const PPCRegister* Gpr(const PPCContext& c, int n) {
  switch (n) {
#define G(i) case i: return &c.r##i;
    G(0) G(1) G(2) G(3) G(4) G(5) G(6) G(7) G(8) G(9) G(10) G(11) G(12) G(13) G(14) G(15) G(16) G(17) G(18) G(19)
    G(20) G(21) G(22) G(23) G(24) G(25) G(26) G(27) G(28) G(29) G(30) G(31)
#undef G
  }
  return nullptr;
}
const PPCRegister* Fpr(const PPCContext& c, int n) { return &c.f0 + n; }       // f0..f31 are consecutive
const PPCVRegister* Vr(const PPCContext& c, int n) { return &c.v0 + n; }       // v0..v127 are consecutive
const PPCCRRegister* Cr(const PPCContext& c, int n) { return &c.cr0 + n; }     // cr0..cr7 are consecutive

// First difference between the native result (a) and the original (b), or an empty string.
std::string CompareRegs(Cmp cmp, const PPCContext& a, const PPCContext& b) {
  const bool all = cmp == Cmp::kAll;
  for (int i = 0; i < 32; ++i) {
    const bool want = all || i == 1 || i == 3 || i >= 13;
    if (want && Gpr(a, i)->u64 != Gpr(b, i)->u64)
      return fmt::format("r{} native={:#018x} original={:#018x}", i, Gpr(a, i)->u64, Gpr(b, i)->u64);
  }
  for (int i = 0; i < 32; ++i) {
    const bool want = all || i == 1 || i >= 14;
    if (want && Fpr(a, i)->u64 != Fpr(b, i)->u64)
      return fmt::format("f{} native={:#018x} original={:#018x}", i, Fpr(a, i)->u64, Fpr(b, i)->u64);
  }
  for (int i = 0; i < 128; ++i) {
    const bool want = all || i == 1 || (i >= 14 && i < 32) || i >= 64;
    if (want && std::memcmp(Vr(a, i)->u8, Vr(b, i)->u8, 16) != 0) {
      const auto* x = Vr(a, i)->u32;
      const auto* y = Vr(b, i)->u32;
      return fmt::format("v{} native={:08x}{:08x}{:08x}{:08x} original={:08x}{:08x}{:08x}{:08x}", i, x[0], x[1], x[2],
                         x[3], y[0], y[1], y[2], y[3]);
    }
  }
  if (all) {
    for (int i = 0; i < 8; ++i)
      if (Cr(a, i)->raw() != Cr(b, i)->raw())
        return fmt::format("cr{} native={:x} original={:x}", i, Cr(a, i)->raw(), Cr(b, i)->raw());
    if (a.ctr.u64 != b.ctr.u64) return fmt::format("ctr native={:#x} original={:#x}", a.ctr.u64, b.ctr.u64);
    if (a.xer.so != b.xer.so || a.xer.ov != b.xer.ov || a.xer.ca != b.xer.ca) return "xer";
    if (a.reserved.u64 != b.reserved.u64) return "reserved";
    if (a.lr != b.lr) return fmt::format("lr native={:#x} original={:#x}", a.lr, b.lr);
  }
  // Only the guest-controlled bits (rounding + flush): x86's MXCSR also holds sticky exception flags, which differ
  // when a copy skips an operation on a denormal (sasrt-nx's false DIFFERENCE).
  if ((a.fpscr.csr & decltype(PPCContext::fpscr)::GuestMask) != (b.fpscr.csr & decltype(PPCContext::fpscr)::GuestMask))
    return fmt::format("fpscr.csr native={:#x} original={:#x}", a.fpscr.csr, b.fpscr.csr);
  return {};
}

// The result of one check: counted, and a difference switches the hook off.
void Report(Hook& h, const std::string& diff, const PPCContext& in) {
  const uint64_t n = h.checks.fetch_add(1, std::memory_order_relaxed) + 1;
  peggle1::enhancements::VerifyCheck();
  if (!diff.empty()) {
    h.failed.store(true, std::memory_order_relaxed);
    peggle1::enhancements::VerifyDifference();
    REXLOG_ERROR(
        "[hot] DIFFERENCE {} ({}): {}; inputs r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} f1={:#x} f2={:#x} "
        "f3={:#x}; native replacement switched off for the rest of the run",
        h.name, h.key->key, diff, in.r3.u64, in.r4.u64, in.r5.u64, in.r6.u64, in.r7.u64, in.f1.u64, in.f2.u64,
        in.f3.u64);
  } else if (n == 4096 || n == (1u << 20)) {
    REXLOG_INFO("[hot] {} ({}): {} checks, no difference", h.name, h.key->key, n);
  }
}

// Peggle 2: the store log of a leaf localized copy's checked call (PEGGLE1_LOG_* macros). At most 32 bytes per
// store (dcbz writes 32).
struct UndoEntry {
  uint32_t addr;
  uint32_t len;
  uint8_t old[32];
};
thread_local std::vector<UndoEntry> t_undo;
thread_local std::vector<UndoEntry> t_undo_checked;  // the log of the call being compared (buffer kept)
thread_local std::vector<uint8_t> t_native_bytes;    // the native's bytes at each logged store (buffer kept)

// A checked call of a leaf localized copy: NativeLogged on a copy of the registers, its stores logged; the native's
// bytes at every address it wrote are kept, the stores rolled back in reverse order, the original runs for real, and
// the registers (all of them) and every byte the copy wrote are compared.
//
// The native's bytes are copied store by store into one buffer, AFTER the call: a byte stored twice then holds its
// last value in every entry that covers it, so comparing entry by entry compares the final state. (Until 2026-10-14
// they went into an unordered_map of single bytes: a node allocation per byte written. The Wwise mixing copies write
// kilobytes per call; their checks, the first 256 calls on a thread, took the console's audio thread to 50-70 % of a
// core for the first seconds of the title screen and its sound stuttered.)
void CheckLogged(Hook& h, PPCContext& ctx, uint8_t* base) {
  t_undo.clear();
  const uint32_t hw_before = ctx.fpscr.getcsr();
  const uint32_t csr_before = ctx.fpscr.csr;
  const PPCContext in = ctx;
  PPCContext nat = ctx;
  h.logged(nat, base);
  const uint32_t hw_native = nat.fpscr.getcsr() & decltype(PPCContext::fpscr)::GuestMask;
  std::vector<UndoEntry>& log = t_undo_checked;
  log.swap(t_undo);  // the original below is a leaf and logs nothing; the entries stay ours until the comparison
  t_undo.clear();
  std::vector<uint8_t>& native = t_native_bytes;
  size_t total = 0;
  for (const UndoEntry& e : log) total += e.len;
  native.resize(total);
  size_t off = 0;
  for (const UndoEntry& e : log) {
    std::memcpy(native.data() + off, Raw(base, e.addr), e.len);
    off += e.len;
  }
  for (auto it = log.rbegin(); it != log.rend(); ++it) std::memcpy(Raw(base, it->addr), it->old, it->len);
  ctx.fpscr.setcsr(hw_before);
  ctx.fpscr.csr = csr_before;
  h.orig(ctx, base);  // the call for real
  const uint32_t hw_orig = ctx.fpscr.getcsr() & decltype(PPCContext::fpscr)::GuestMask;
  std::string diff = CompareRegs(Cmp::kAll, nat, ctx);
  if (diff.empty() && hw_native != hw_orig) {
    diff = fmt::format("hardware FPCR native={:#x} original={:#x}", hw_native, hw_orig);
  }
  if (diff.empty()) {
    off = 0;
    for (const UndoEntry& e : log) {
      const uint8_t* now = Raw(base, e.addr);
      if (std::memcmp(now, native.data() + off, e.len) != 0) {
        uint32_t k = 0;
        while (k < e.len && now[k] == native[off + k]) ++k;
        diff = fmt::format("memory {:#010x} native={:#04x} original={:#04x} ({} bytes in {} stores)", e.addr + k,
                           native[off + k], now[k], total, log.size());
        break;
      }
      off += e.len;
    }
  }
  log.clear();
  Report(h, diff, in);
}

}  // namespace

void LogBefore(uint8_t* base, uint32_t addr, uint32_t len) {
  UndoEntry e;
  e.addr = addr;
  e.len = len;
  std::memcpy(e.old, Raw(base, addr), len);
  t_undo.push_back(e);
}

namespace {
void CheckBody(Hook& h, PPCContext& ctx, uint8_t* base) {
  if (h.logged) {
    CheckLogged(h, ctx, base);
    return;
  }
  Writes w;
  h.writes(ctx, base, w);
  if (w.overflow) {  // cannot verify: do not trust the native version for this call
    h.orig(ctx, base);
    return;
  }
  size_t total = 0;
  for (int i = 0; i < w.n; ++i) total += w.r[i].len;
  std::vector<uint8_t> before(total), native_out(total);
  size_t off = 0;
  for (int i = 0; i < w.n; ++i) {
    std::memcpy(before.data() + off, Raw(base, w.r[i].addr), w.r[i].len);
    off += w.r[i].len;
  }
  const uint32_t hw_before = ctx.fpscr.getcsr();
  const uint32_t csr_before = ctx.fpscr.csr;
  const PPCContext in = ctx;  // inputs for the report
  PPCContext nat = ctx;
  if (!h.native(nat, base)) {  // declined: the original runs, nothing to compare
    h.orig(ctx, base);
    return;
  }
  const uint32_t hw_native = nat.fpscr.getcsr() & decltype(PPCContext::fpscr)::GuestMask;
  off = 0;
  for (int i = 0; i < w.n; ++i) {
    std::memcpy(native_out.data() + off, Raw(base, w.r[i].addr), w.r[i].len);
    std::memcpy(Raw(base, w.r[i].addr), before.data() + off, w.r[i].len);  // roll back
    off += w.r[i].len;
  }
  ctx.fpscr.setcsr(hw_before);
  ctx.fpscr.csr = csr_before;
  h.orig(ctx, base);  // the call for real
  const uint32_t hw_orig = ctx.fpscr.getcsr() & decltype(PPCContext::fpscr)::GuestMask;

  std::string diff = CompareRegs(h.cmp, nat, ctx);
  if (diff.empty() && hw_native != hw_orig) diff = fmt::format("hardware FPCR native={:#x} original={:#x}", hw_native, hw_orig);
  if (diff.empty()) {
    off = 0;
    for (int i = 0; i < w.n && diff.empty(); ++i) {
      const uint8_t* now = Raw(base, w.r[i].addr);
      for (uint32_t k = 0; k < w.r[i].len; ++k) {
        if (now[k] != native_out[off + k]) {
          diff = fmt::format("memory {:#010x} (+{} of range {:#010x}+{}) native={:#04x} original={:#04x}",
                             w.r[i].addr + k, k, w.r[i].addr, w.r[i].len, native_out[off + k], now[k]);
          break;
        }
      }
      off += w.r[i].len;
    }
  }
  Report(h, diff, in);
}
}  // namespace

void Check(Hook& h, PPCContext& ctx, uint8_t* base) {
  const auto started = std::chrono::steady_clock::now();
  CheckBody(h, ctx, base);
  const uint64_t ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
  ::peggle1::enhancements::CheckTime(ns);
  // A guarded call that takes long stalls its thread (the audio thread's mixing copies: the sound stutters). The
  // first few of them are named, with their thread.
  if (ns >= 1000000) [[unlikely]] {
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 40) {
      REXLOG_INFO("[hot] slow guarded call: {} ({}) took {:.2f} ms; check {} of this hook", h.name, h.key->key,
                  ns / 1e6, h.checks.load(std::memory_order_relaxed));
    }
  }
}

}  // namespace peggle1::hot

// ---------------------------------------------------------------------------------------------------------------
// The hooks. Hand-written natives first (Writes: the guest ranges each may write; Cmp::kReturn: r3, f1 and the
// non-volatile registers, as assembly routines leave their scratch registers undefined), then the localized copies
// (tools/localize_native.py --list app/src/cpu/hot/localized.txt writes their hook lines).

// C runtime: byte for byte SA2's and Peggle 2's routines (tools/p2_function_matches.py: Peggle 1's memcpy at 0x8279B350
// is Peggle 2's 0x825F0AE0, its memset at 0x8279DF80 Peggle 2's 0x825F10B0; sa2-nx verified them: 115-133 M calls,
// 0 differences). The natives keep Peggle 2's names (n_825F0AE0.h, n_825F10B0.h); the size and FNV-1a 64 of the guest
// code are the same, so the code check passes at Peggle 1's addresses.
PEGGLE1_HOT_HOOK(8279B350, peggle1_cpu_native_memcpy, "Native CRT memcpy (sub_8279B350): host memcpy/memmove",
                 peggle1::hot::n_825F0AE0, &peggle1::hot::n_825F0AE0::Writes, peggle1::hot::Cmp::kReturn, 1160,
                 0xEE0EAEB1E0584297ull)
PEGGLE1_HOT_HOOK(8279DF80, peggle1_cpu_native_memset, "Native CRT memset (sub_8279DF80): host memset",
                 peggle1::hot::n_825F10B0, &peggle1::hot::n_825F10B0::Writes, peggle1::hot::Cmp::kReturn, 160,
                 0x54D201DC8A21DABAull)

#include "../../generated/hot/localized_hooks.inc"

