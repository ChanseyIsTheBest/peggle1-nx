// Peggle 2 - native replacements of hot guest functions: hook dispatch with a self-check guard (from sa2-nx's
// sa2_hot_guest.h, which took it from masseffect-nx's me_hot_guest.h; see peggle1_hot_common.h).
//
// Hot-path cost matters (memcpy is called from ~2,000 places): the Hook object is constant-initialised (no
// function-local static guard: an acquire load per call, measured at ~1.3 % of Mass Effect's main thread on one hook),
// the only per-call shared state is one byte load, and the call counter that decides which calls are checked is one
// relaxed counter per hook (peggle1_cpu_hook_counter_shared, from sa2-nx; thread-local without it).
#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include <rex/hook.h>

#include "peggle1_enhancements.h"
#include "cpu/peggle1_hot_common.h"

namespace peggle1::hot {

enum : uint8_t { kUninit = 0, kOff = 1, kOn = 2 };

struct Hook {
  const char* name;
  PPCFunc* orig;
  bool (*native)(PPCContext&, uint8_t*);  // false = declined: nothing was modified, the original must run
  void (*writes)(const PPCContext&, const uint8_t*, Writes&);
  Cmp cmp;
  peggle1::enhancements::Native* key;
  uint32_t code_address;  // the guest code the native was written from (tools/guest_code_hash.py)
  uint32_t code_bytes;
  uint64_t code_hash;
  // Peggle 2: a leaf localized copy's variant that records every guest store it makes (tools/localize_native.py).
  // With it the guard needs no `writes` list: it rolls the logged stores back and compares every written byte.
  void (*logged)(PPCContext&, uint8_t*) = nullptr;
  // Runtime state. Until the first call initialises it the zeros are harmless: kUninit goes to the slow path, and a
  // stale guard_calls/period_mask of 0 on another core only means "check this call".
  std::atomic<uint8_t> state{kUninit};
  uint32_t guard_calls = 0;         // the first N calls of each thread are always checked ...
  uint32_t period_mask = 0;         // ... afterwards every (mask+1)-th call
  std::atomic<bool> failed{false};  // a check found a difference: the original runs from now on
  std::atomic<uint64_t> checks{0};
  // peggle1_cpu_hook_counter_ctx: this hook's counter in every thread's context (PPCContext::host_counters), given at
  // the first call (InitHook); kNoSlot until then, kSlotsFull when the slots ran out.
  std::atomic<uint8_t> slot{0xFF};
};

// PPCContext::host_counters: the hooks' slots (first called, first served); the last four are left to the native
// Direct3D layer, as in sa2-nx.
inline constexpr uint8_t kCtxSlots = 60;
inline constexpr uint8_t kNoSlot = 0xFF, kSlotsFull = 0xFE;
// A slot of PPCContext::host_counters for one hook (kSlotsFull when none is left). Two threads at a hook's first call
// each take a number and one of them stays unused. Defined in peggle1_hot_guest.cpp.
void TakeCtxSlot(std::atomic<uint8_t>& slot);

// First call of a hook (any thread): key, code check, guard settings. Defined in peggle1_hot_guest.cpp.
void InitHook(Hook& h, uint8_t* base);

// One guarded call: native on a copy of the registers with the written ranges snapshotted, rollback, original,
// compare. Leaves the state exactly as the original would. Defined in peggle1_hot_guest.cpp.
void Check(Hook& h, PPCContext& ctx, uint8_t* base);

// Use counter for the 10 s report: one shared add per 256 calls of a thread.
inline void CountUse(Hook& h, uint32_t c) {
  if ((c & 255) == 0) h.key->Use(256);
}

template <auto Native>
inline bool NativeB(PPCContext& ctx, uint8_t* base) {
  if constexpr (std::is_void_v<decltype(Native(ctx, base))>) {
    Native(ctx, base);
    return true;
  } else {
    return Native(ctx, base);
  }
}

extern ::peggle1::enhancements::Native g_counter_shared;  // peggle1_cpu_hook_counter_shared (peggle1_hot_guest.cpp)
extern ::peggle1::enhancements::Native g_counter_ctx;     // peggle1_cpu_hook_counter_ctx (peggle1_hot_guest.cpp)
// peggle1_cpu_natives_smart_sync (from sa2-nx's localizer, round 3): a localized copy's call writes back and reloads only
// the registers its callee can reach (tools/localize_native.py); off = every register, as before.
extern ::peggle1::enhancements::Native g_smart_sync;
// Verify mode found a callee that wrote a register its computed reach leaves out (tools/localize_native.py's check at
// every smart call site): counted as a verify difference and logged once per (copy, callee).
void SmartDifference(uint32_t copy, const char* callee);
// A smart call site checked in verify mode: one verify check and one use of peggle1_cpu_natives_smart_sync.
void SmartChecked();
// Verify mode at a smart call site (out of line, so the copies stay small): snapshot the context, call `callee`, and
// compare the `n` localized registers outside its reach (a write there is a SmartDifference).
struct RegSpan {
  uint16_t offset, size;
};
void SmartCall(PPCContext& ctx, uint8_t* base, PPCFunc* callee, const RegSpan* rest, int n, uint32_t copy,
               const char* name);

template <auto Native>
inline void DispatchCount(Hook& h, uint32_t c, PPCContext& ctx, uint8_t* base) {
  CountUse(h, c);
  if (c < h.guard_calls || (c & h.period_mask) == 0) [[unlikely]] {
    Check(h, ctx, base);
    return;
  }
  if (!NativeB<Native>(ctx, base)) h.orig(ctx, base);
}

template <auto Native>
inline void Dispatch(Hook& h, uint32_t& n, PPCContext& ctx, uint8_t* base) {
  if (h.failed.load(std::memory_order_relaxed)) [[unlikely]] {
    h.orig(ctx, base);
    return;
  }
  DispatchCount<Native>(h, n++, ctx, base);
}

// peggle1_cpu_hook_counter_shared (sa2-nx's sa2_cpu_hook_counter_shared): one counter per hook for all threads (a
// relaxed load and store, no read-modify-write: racing threads may count a call twice, which only moves which calls the
// guard samples). On the Switch a thread_local costs a call to __aarch64_read_tp on every hooked call (memcpy alone
// comes from ~2,000 call sites). The guard then checks the first N calls of the process instead of the first N of each
// thread.
template <auto Native>
inline void DispatchShared(Hook& h, std::atomic<uint32_t>& n, PPCContext& ctx, uint8_t* base) {
  if (h.failed.load(std::memory_order_relaxed)) [[unlikely]] {
    h.orig(ctx, base);
    return;
  }
  const uint32_t c = n.load(std::memory_order_relaxed);
  n.store(c + 1, std::memory_order_relaxed);
  if ((c & 255) == 0) [[unlikely]] g_counter_shared.Use(256);
  DispatchCount<Native>(h, c, ctx, base);
}

// peggle1_cpu_hook_counter_ctx (sasrt-nx's, through sa2-nx, 2026-10-12): the call counter of each hook in the calling
// thread's own context, which the hook is handed anyway: the per-thread counter the guard was designed with (the first
// N calls of each thread checked, then every period-th), without the thread_local's call to __aarch64_read_tp and
// without the shared counter's store, whose cache line every thread calling the hook pulls to its core (memcpy and the
// hot copies are called from the game's main and audio threads alike). The counter only decides which calls the guard
// checks.
template <auto Native>
inline void DispatchCtx(Hook& h, uint32_t& n, PPCContext& ctx, uint8_t* base) {
  if (h.failed.load(std::memory_order_relaxed)) [[unlikely]] {
    h.orig(ctx, base);
    return;
  }
  const uint32_t c = n++;
  if ((c & 255) == 0) [[unlikely]] g_counter_ctx.Use(256);
  DispatchCount<Native>(h, c, ctx, base);
}

// The use counter of an unguarded copy (the 10 s report only): the thread's own slot with peggle1_cpu_hook_counter_ctx,
// else the lossy shared count the copies had (a relaxed load and store).
inline uint32_t CountCopyCall(uint8_t slot, std::atomic<uint32_t>& shared, PPCContext& ctx) {
  if (g_counter_ctx.active && slot < kCtxSlots) [[likely]] return ctx.host_counters[slot]++;
  const uint32_t c = shared.load(std::memory_order_relaxed);
  shared.store(c + 1, std::memory_order_relaxed);
  return c;
}

}  // namespace peggle1::hot

// The dispatch of a guarded hook's macro: the counter in the thread's context (peggle1_cpu_hook_counter_ctx), the
// shared counter (peggle1_cpu_hook_counter_shared) or a thread-local one.
#define PEGGLE1_HOT_DISPATCH(NATIVE)                                                       \
  do {                                                                                     \
    const uint8_t slot = h.slot.load(std::memory_order_relaxed);                           \
    if (peggle1::hot::g_counter_ctx.active && slot < peggle1::hot::kCtxSlots) [[likely]] { \
      peggle1::hot::DispatchCtx<NATIVE>(h, ctx.host_counters[slot], ctx, base);            \
    } else if (peggle1::hot::g_counter_shared.active) [[likely]] {                         \
      static std::atomic<uint32_t> n_shared;                                               \
      peggle1::hot::DispatchShared<NATIVE>(h, n_shared, ctx, base);                        \
    } else {                                                                               \
      static thread_local uint32_t n_calls;                                                \
      peggle1::hot::Dispatch<NATIVE>(h, n_calls, ctx, base);                               \
    }                                                                                      \
  } while (0)

// KEY: the enhancement key (peggle1_cpu_native_*); NS: namespace of the n_*.h header; ADDR: hex address without 0x;
// BYTES / HASH: tools/guest_code_hash.py ADDR; CMP: peggle1::hot::Cmp::kReturn or kAll.
#define PEGGLE1_HOT_HOOK(ADDR, KEY, DESC, NS, WRITES, CMP, BYTES, HASH)                                            \
  PEGGLE1_NATIVE(g_enh_##ADDR, KEY, DESC);                                          \
  REX_EXTERN(__imp__sub_##ADDR);                                                                           \
  REX_HOOK_RAW(sub_##ADDR) {                                                                               \
    static peggle1::hot::Hook h{"sub_" #ADDR, &__imp__sub_##ADDR, &peggle1::hot::NativeB<&NS::Native>, WRITES, CMP, \
                            &g_enh_##ADDR, 0x##ADDR##u, BYTES, HASH};                                       \
    const uint8_t st = h.state.load(std::memory_order_relaxed);                                            \
    if (st == peggle1::hot::kOn) [[likely]] {                                                                  \
      PEGGLE1_HOT_DISPATCH(&NS::Native);                                                                       \
      return;                                                                                              \
    }                                                                                                      \
    if (st == peggle1::hot::kUninit) [[unlikely]] {                                                            \
      peggle1::hot::InitHook(h, base);                                                                         \
      if (h.state.load(std::memory_order_relaxed) == peggle1::hot::kOn) {                                      \
        PEGGLE1_HOT_DISPATCH(&NS::Native);                                                                     \
        return;                                                                                            \
      }                                                                                                    \
    }                                                                                                      \
    __imp__sub_##ADDR(ctx, base);                                                                          \
  }

// Unguarded: for localized copies of functions that call other functions. The guard cannot run both versions there
// (the callees' side effects would happen twice), so the copy runs on every call once its key and code check pass.
// Exact by construction (tools/localize_native.py); checked end to end on the PC (lockstep capture).
#define PEGGLE1_HOT_HOOK_UNGUARDED(ADDR, KEY, DESC, NS, BYTES, HASH)                                    \
  PEGGLE1_NATIVE(g_enh_##ADDR, KEY, DESC);                               \
  REX_EXTERN(__imp__sub_##ADDR);                                                                \
  REX_HOOK_RAW(sub_##ADDR) {                                                                    \
    static std::atomic<uint8_t> state{peggle1::hot::kUninit};                                       \
    static std::atomic<uint32_t> n_calls; /* the use report only: a lossy relaxed count, no TLS */ \
    static std::atomic<uint8_t> slot{peggle1::hot::kNoSlot}; /* peggle1_cpu_hook_counter_ctx */    \
    uint8_t st = state.load(std::memory_order_relaxed);                                         \
    if (st == peggle1::hot::kUninit) [[unlikely]] {                                                 \
      const bool on = g_enh_##ADDR.active &&                                                    \
                      peggle1::enhancements::GuestCodeMatches(base, 0x##ADDR##u, BYTES, HASH, #KEY);         \
      peggle1::hot::TakeCtxSlot(slot);                                                          \
      st = on ? peggle1::hot::kOn : peggle1::hot::kOff;                                                 \
      state.store(st, std::memory_order_relaxed);                                               \
    }                                                                                           \
    if (st == peggle1::hot::kOn) [[likely]] {                                                       \
      const uint32_t c =                                                                        \
          peggle1::hot::CountCopyCall(slot.load(std::memory_order_relaxed), n_calls, ctx);      \
      if ((c & 255) == 0) g_enh_##ADDR.Use(256);                                                \
      NS::Native(ctx, base);                                                                    \
      return;                                                                                   \
    }                                                                                           \
    __imp__sub_##ADDR(ctx, base);                                                               \
  }

// Peggle 2: a leaf localized copy (tools/localize_native.py emits these lines into localized_hooks.inc), guarded like
// PEGGLE1_HOT_HOOK but without a hand-written list of written ranges: the checked calls run NS::NativeLogged, whose
// guest stores record the bytes they overwrite, and the guard rolls them back, runs the original and compares every
// register and every byte the copy wrote.
#define PEGGLE1_HOT_HOOK_LOGGED(ADDR, KEY, DESC, NS, BYTES, HASH)                                         \
  PEGGLE1_NATIVE(g_enh_##ADDR, KEY, DESC);                                                                \
  REX_EXTERN(__imp__sub_##ADDR);                                                                          \
  REX_HOOK_RAW(sub_##ADDR) {                                                                              \
    static peggle1::hot::Hook h{"sub_" #ADDR, &__imp__sub_##ADDR, &peggle1::hot::NativeB<&NS::Native>,   \
                                nullptr, peggle1::hot::Cmp::kAll, &g_enh_##ADDR, 0x##ADDR##u, BYTES, HASH, \
                                &NS::NativeLogged};                                                       \
    const uint8_t st = h.state.load(std::memory_order_relaxed);                                           \
    if (st == peggle1::hot::kOn) [[likely]] {                                                             \
      PEGGLE1_HOT_DISPATCH(&NS::Native);                                                                  \
      return;                                                                                             \
    }                                                                                                     \
    if (st == peggle1::hot::kUninit) [[unlikely]] {                                                       \
      peggle1::hot::InitHook(h, base);                                                                    \
      if (h.state.load(std::memory_order_relaxed) == peggle1::hot::kOn) {                                 \
        PEGGLE1_HOT_DISPATCH(&NS::Native);                                                                \
        return;                                                                                           \
      }                                                                                                   \
    }                                                                                                     \
    __imp__sub_##ADDR(ctx, base);                                                                         \
  }
