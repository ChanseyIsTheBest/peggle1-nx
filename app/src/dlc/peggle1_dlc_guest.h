// Peggle 2's PS4-only content in the Xbox 360 build: host code among the game's own objects.
//
// The two powers the Xbox 360 executable lacks (Windy's PurplePegs, the final form of Jimmy's MultiBall) are written
// as host C++ (src/dlc). They are classes of the game's own reflection system: the game creates them by name, calls
// them through their vtables and they call the game's functions back. This header is the plumbing for that:
//
//   Guest        big-endian loads and stores, and calls of game functions with the PowerPC calling convention
//                (integer arguments r3..r10, float arguments f1.., result r3 / f1)
//   Frame        stack space of the calling game thread for temporaries the game writes into
//   GuestString  a temporary std::string of the game (MSVC 2010: 16 bytes inline or a pointer, size +16, capacity +20)
//   Thunk()      a new game-side function address for a host function, to store in a vtable
//
// A game function is always named through its __imp__ symbol here: tools/direct_calls.py treats every other mention
// of an 82xxxxxx address in the sources as a hook and keeps the generated calls to it indirect.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>

#include <rex/ppc.h>

namespace peggle1::dlc {

struct Guest {
  PPCContext& ctx;
  uint8_t* base;

  uint8_t u8(uint32_t a) const { return base[a]; }
  void set8(uint32_t a, uint8_t v) const { base[a] = v; }
  uint16_t u16(uint32_t a) const {
    uint16_t v;
    std::memcpy(&v, base + a, 2);
    return __builtin_bswap16(v);
  }
  uint32_t u32(uint32_t a) const {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return __builtin_bswap32(v);
  }
  int32_t i32(uint32_t a) const { return int32_t(u32(a)); }
  void set32(uint32_t a, uint32_t v) const {
    v = __builtin_bswap32(v);
    std::memcpy(base + a, &v, 4);
  }
  uint64_t u64(uint32_t a) const { return (uint64_t(u32(a)) << 32) | u32(a + 4); }
  void set64(uint32_t a, uint64_t v) const {
    set32(a, uint32_t(v >> 32));
    set32(a + 4, uint32_t(v));
  }
  float f32(uint32_t a) const { return std::bit_cast<float>(u32(a)); }
  void setf(uint32_t a, float v) const { set32(a, std::bit_cast<uint32_t>(v)); }
  // Two floats the game passes and stores as one 64-bit value (a Vec2 in a register: x in the high half).
  uint64_t Pack(float x, float y) const {
    return (uint64_t(std::bit_cast<uint32_t>(x)) << 32) | std::bit_cast<uint32_t>(y);
  }

  // Guest addresses this may read: the image and the heaps (not null, not the MMIO range).
  static bool Plausible(uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u; }

  PPCFunc* Lookup(uint32_t address) const;

  // fn(args...): integer / pointer arguments; float arguments are set by the caller (ctx.f1.f64, ...) before.
  uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args = {}) const {
    PPCRegister* const reg[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    int i = 0;
    for (const uint64_t a : args) reg[i++]->u64 = a;
    fn(ctx, base);
    return ctx.r3.u32;
  }
  uint32_t CallAt(uint32_t address, std::initializer_list<uint64_t> args = {}) const {
    return Call(Lookup(address), args);
  }
  // object->vtable[offset / 4](object, args...)
  uint32_t Virtual(uint32_t object, uint32_t offset, std::initializer_list<uint64_t> args = {}) const {
    PPCRegister* const reg[] = {&ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    int i = 0;
    for (const uint64_t a : args) reg[i++]->u64 = a;
    ctx.r3.u64 = object;
    Lookup(u32(u32(object) + offset))(ctx, base);
    return ctx.r3.u32;
  }
  void Floats(double a, double b = 0, double c = 0, double d = 0) const {
    ctx.f1.f64 = a;
    ctx.f2.f64 = b;
    ctx.f3.f64 = c;
    ctx.f4.f64 = d;
  }
  float FloatResult() const { return float(ctx.f1.f64); }

  // A game std::string's characters (empty when the object is not readable).
  std::string String(uint32_t object) const {
    if (!Plausible(object)) return {};
    const uint32_t size = u32(object + 16), capacity = u32(object + 20);
    const uint32_t chars = capacity >= 16 ? u32(object) : object;
    if (!Plausible(chars) || size > 4096) return {};
    return std::string(reinterpret_cast<const char*>(base + chars), size);
  }
};

// Stack space of the game thread, below the caller's frame: the linkage area the game's functions expect above
// their own frame (back chain, the parameter save area they may write to) and then the temporaries.
class Frame {
 public:
  Frame(const Guest& g, uint32_t bytes) : g_(g), saved_(g.ctx.r1.u32) {
    const uint32_t size = (kLinkage + bytes + 15) & ~15u;
    const uint32_t sp = saved_ - size;
    std::memset(g.base + sp, 0, size);
    g.set32(sp, saved_);
    g.ctx.r1.u64 = sp;
    next_ = sp + kLinkage;
    end_ = sp + size;
  }
  ~Frame() { g_.ctx.r1.u64 = saved_; }
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;

  // `bytes` of zeroed space (8-byte aligned). The frame must have been made large enough.
  uint32_t Alloc(uint32_t bytes) {
    const uint32_t a = next_;
    next_ += (bytes + 7) & ~7u;
    if (next_ > end_) __builtin_trap();
    return a;
  }
  uint32_t Chars(std::string_view text) {
    const uint32_t a = Alloc(uint32_t(text.size()) + 1);
    std::memcpy(g_.base + a, text.data(), text.size());
    return a;
  }

 private:
  static constexpr uint32_t kLinkage = 96;
  const Guest& g_;
  uint32_t saved_, next_ = 0, end_ = 0;
};

// A temporary std::string of the game, built from host text and destroyed with the scope. Its destructor calls the
// game (and so overwrites r3): read a call's result before the string goes out of scope.
class GuestString {
 public:
  GuestString(const Guest& g, Frame& frame, std::string_view text);
  ~GuestString();
  GuestString(const GuestString&) = delete;
  GuestString& operator=(const GuestString&) = delete;
  uint32_t address() const { return address_; }

 private:
  const Guest& g_;
  uint32_t address_;
};

// A game-side address that calls `fn` (0 when the table is full: logged).
uint32_t Thunk(PPCFunc* fn);

// Memory of the game's heap that is never freed (vtables, names), zeroed.
uint32_t Permanent(const Guest& g, uint32_t bytes);
uint32_t PermanentChars(const Guest& g, std::string_view text);

// A copy of a game vtable with the word before it (the RTTI locator); returns the address of slot 0.
uint32_t CopyVtable(const Guest& g, uint32_t vtable, uint32_t slots);

}  // namespace peggle1::dlc
