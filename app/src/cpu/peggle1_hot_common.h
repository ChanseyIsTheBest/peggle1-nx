// Peggle 2 - native replacements of hot guest functions: shared definitions (from sa2-nx's sa2_hot_common.h, which
// took it from masseffect-nx's me_hot_common.h).
//
// A "hot hook" replaces one recompiled guest function (sub_X) by a native version with the same observable behaviour.
// Each lives in src/cpu/hot/n_<addr>.h or app/generated/hot/n_<addr>.h as
//
//   namespace peggle1::hot::n_<addr> {
//     inline bool/void Native(PPCContext& ctx, uint8_t* base);   // the replacement (bool: false = declined, the
//                                                              // original runs; nothing may have been modified)
//   }
//
// and is registered in peggle1_hot_guest.cpp with its own key, the hash of the guest code it was written from, the guest
// memory it writes (for the guard) and the registers the guard compares.
//
// What "exact" means here: memory effects (every guest range the original writes, except the callee-owned stack
// scratch below r1), every register the original leaves defined for its callers, and the FPSCR flush-mode state.
// Xbox 360 titles are compiled with MSVC's interprocedural register allocation (sa2-nx found it in SA2): callers keep
// values in volatile registers across a call to a function known not to touch them (tools/call_site_liveness.py shows
// it per call site).
// So a native must never write a register the original does not write. The hand-written CRT natives only write their
// return register; the localized copies (tools/localize_native.py) write back the same values the original leaves.
#pragma once

#include <cstdint>
#include <cstring>

#include <rex/platform.h>
#include <rex/ppc/context.h>

namespace peggle1::hot {

// Guest memory accessors with the generated code's semantics (big-endian; the +0x1000 physical-range offset of the
// Windows / Apple-silicon hosts, none on the Switch; no volatile).
inline uint32_t PhysOff(uint32_t a) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return a >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)a;
  return 0u;
#endif
}
inline uint8_t* Raw(uint8_t* base, uint32_t a) { return base + a + PhysOff(a); }
inline const uint8_t* Raw(const uint8_t* base, uint32_t a) { return base + a + PhysOff(a); }
inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a), 4);
  return __builtin_bswap32(v);
}

struct Region {
  uint32_t addr;
  uint32_t len;
};

// Guest ranges a replacement may write, computed from the inputs BEFORE the call. The guard snapshots them, runs the
// replacement, rolls the memory back, runs the original and compares. `overflow` = too many/too large to verify.
struct Writes {
  static constexpr int kMax = 8;
  static constexpr uint32_t kMaxBytes = 1u << 20;
  Region r[kMax];
  int n = 0;
  bool overflow = false;
  void Add(uint32_t addr, uint32_t len) {
    if (!len) return;
    if (n >= kMax || len > kMaxBytes || uint64_t(addr) + len > 0x100000000ull) {
      overflow = true;
      return;
    }
    r[n++] = {addr, len};
  }
};

// What the guard compares besides memory: kReturn = r3, f1 and the non-volatile registers (the CRT natives, which
// write nothing else); kAll = every register, CR field, CTR and XER (the leaf localized copies); kCalls = as kReturn,
// for localized copies that call other functions (their callees' scratch registers depend on whether a nested call
// was itself checked).
enum class Cmp : uint8_t { kReturn, kAll, kCalls };

}  // namespace peggle1::hot
