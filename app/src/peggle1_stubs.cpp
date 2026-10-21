// The linker's jump stubs of Peggle's executable, in the function table (2026-10-21).
//
// default.xex holds 13 identical tables of 13,011 stubs, each `lis r11,hi; addi r11,r11,lo; mtctr r11; bctr` (16
// bytes) jumping to one function (docs/peggle1-notes.md, "Code generation"). Calls by `bl`/`b` reach the stubs the
// generator made functions of, but the game also keeps stub addresses in data (vtables, callbacks), and an indirect
// call to a stub the generator never saw ended the first boot: "Call to invalid or unregistered function at guest
// address 0x8214E720". Generating all 169,143 stubs would add as many functions; instead every stub without a function
// of its own gets its target's in the function table, once the image is loaded. The stub's only other effect, r11 and
// ctr holding the target on entry, is nothing a function reads.

#include "peggle1_stubs.h"

#include <cstdint>
#include <cstring>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>

namespace peggle1::stubs {
namespace {

// The executable sections (.xidata, .text and the nine .embsec_ sections), from the image's PE section table.
constexpr uint32_t kCodeBegin = 0x82140000;
constexpr uint32_t kCodeEnd = 0x82F778A0;

inline uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

}  // namespace

void Install(rex::Runtime* runtime) {
  if (!runtime || !runtime->memory() || !runtime->function_dispatcher()) return;
  auto* dispatcher = runtime->function_dispatcher();
  const uint8_t* code = runtime->memory()->TranslateVirtual<const uint8_t*>(kCodeBegin);
  uint32_t stubs = 0, had = 0, set = 0, no_target = 0;
  for (uint32_t offset = 0; offset + 16 <= kCodeEnd - kCodeBegin;) {
    const uint8_t* p = code + offset;
    const uint32_t w0 = Load32(p), w1 = Load32(p + 4);
    if ((w0 & 0xFFFF0000u) != 0x3D600000u || (w1 & 0xFFFF0000u) != 0x396B0000u || Load32(p + 8) != 0x7D6903A6u ||
        Load32(p + 12) != 0x4E800420u) {
      offset += 4;
      continue;
    }
    const uint32_t stub = kCodeBegin + offset;
    const uint32_t target = ((w0 & 0xFFFFu) << 16) + uint32_t(int32_t(int16_t(w1 & 0xFFFFu)));
    offset += 16;
    ++stubs;
    if (dispatcher->GetFunction(stub)) {
      ++had;
      continue;
    }
    if (auto* fn = dispatcher->GetFunction(target)) {
      dispatcher->SetFunction(stub, fn);
      ++set;
    } else {
      ++no_target;
    }
  }
  REXLOG_INFO("[stubs] {} jump stubs: {} have a generated function, {} now call their target directly, {} jump to an "
              "address with no function",
              stubs, had, set, no_target);
}

}  // namespace peggle1::stubs
