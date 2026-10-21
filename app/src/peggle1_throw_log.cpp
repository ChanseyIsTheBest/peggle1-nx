// Logs every C++ exception the game throws, before the throw happens.
//
// The game throws through _CxxThrowException = sub_8279AAB0 (688 call sites, tools/callers.py, 2026-10-21); it copies
// the exception record template (32 bytes at 0x820D4DD0) and calls RaiseException (sub_827CB950). The SDK's
// RtlRaiseException handles only SetThreadName: any C++ throw breaks into the debugger on Windows (a silent crash
// without one) and on the Switch returns into the code after the call with garbage registers. In Peggle 2 every throw but
// one was already fatal on a real Xbox 360, so a throw in the port means something failed before it (an allocation,
// a missing resource): this hook names it in the log.
//
// _CxxThrowException(object r3, ThrowInfo* r4). MSVC's 32-bit layout:
//   ThrowInfo          { attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray }
//   CatchableTypeArray { count, CatchableType* [count] }
//   CatchableType      { properties, TypeDescriptor*, thisDisplacement (3 words), sizeOrOffset, copyFunction }
//   TypeDescriptor     { pVFTable, spare, char name[] }   e.g. ".?AVbad_alloc@std@@"
// A rethrow (throw;) passes r4 = 0. For a std::exception the message pointer is the word after the vtable.

#include <cstdint>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/logging/api.h>

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

// Guest addresses this reads must be in the main image or the heaps (not null, not the MMIO range).
inline bool Plausible(uint32_t address) { return address >= 0x40000000u && address < 0xA0000000u; }

std::string GuestString(const uint8_t* base, uint32_t address, size_t max) {
  std::string s;
  if (!Plausible(address)) return s;
  for (size_t i = 0; i < max; ++i) {
    const char c = char(base[address + i]);
    if (!c) break;
    if (c < 0x20 || c > 0x7E) return {};
    s.push_back(c);
  }
  return s;
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_8279AAB0);
REX_HOOK_RAW(sub_8279AAB0) {  // _CxxThrowException
  const uint32_t object = ctx.r3.u32, info = ctx.r4.u32, from = uint32_t(ctx.lr);
  if (!info) {
    REXLOG_ERROR("[throw] C++ rethrow from {:08X} (object {:08X})", from, object);
  } else {
    std::string type = "?";
    if (Plausible(info)) {
      const uint32_t array = Load32(base, info + 12);
      if (Plausible(array) && Load32(base, array) > 0) {
        const uint32_t catchable = Load32(base, array + 4);
        if (Plausible(catchable)) {
          const uint32_t descriptor = Load32(base, catchable + 4);
          if (Plausible(descriptor)) type = GuestString(base, descriptor + 8, 120);
        }
      }
    }
    // std::exception and most library exceptions keep a char* message right after the vtable.
    std::string what = Plausible(object) ? GuestString(base, Load32(base, object + 4), 200) : std::string();
    REXLOG_ERROR("[throw] C++ exception {} thrown from {:08X} (object {:08X}){}{}", type, from, object,
                 what.empty() ? "" : ": ", what);
  }
  rex::FlushLogging();
  __imp__sub_8279AAB0(ctx, base);
}
