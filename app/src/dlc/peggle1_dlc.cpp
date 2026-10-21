// Peggle 2's PS4-only content in the Xbox 360 build: the port's side of it.
//
// The PS4 release (Magical Masters Edition) has two masters the Xbox 360 release never got: Windy the fairy (power
// class PurplePegs, "Fairy Flock") and Jimmy Lightning (power class MultiBall, "Multiball"). Their art, levels and
// data are converted into the 360 archive by tools/ps4_dlc; the code is not in the 360 executable:
//   PurplePegs  only its data file exists (PACKAGES\PURPLEPEGS.RTON names a class the executable does not register)
//   MultiBall   an early version exists: it spawns the balls after a delay, with no slow motion, lightning, peg
//               removal, sounds or style shots
// This module adds both to the running game as classes of its own reflection system, written from the behaviour of
// the PS4 executable (docs/peggle1-notes.md, "PS4 content in the Xbox 360 build"):
//   peggle1_dlc_windy.cpp   class PurplePegs, registered next to the game's own classes
//   peggle1_dlc_jimmy.cpp   class MultiBall, the game's class with a larger object and replaced methods
// With the stock archive nothing uses either class: no master names PurplePegs or MultiBall there.
//
// KEYS: peggle1_dlc_powers (default true; false = the executable as it shipped), peggle1_dlc_log (the powers'
// events and the game's own resource error messages in the log), peggle1_dlc_test (switches for unattended runs).
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include "dlc/peggle1_dlc_game.h"

REXCVAR_DEFINE_BOOL(peggle1_dlc_powers, true, "Peggle1 DLC",
                    "The two powers of the PS4 release's extra masters (Windy's Fairy Flock, Jimmy's Multiball) as "
                    "classes of the game; false = the Xbox 360 executable as it shipped");
REXCVAR_DEFINE_BOOL(peggle1_dlc_log, false, "Peggle1 DLC",
                    "Log the two powers' events and the game's own error messages (missing resource groups, powers)");
REXCVAR_DEFINE_BOOL(peggle1_dlc_overlays, false, "Peggle1 DLC",
                    "Draw the two powers' screen layers that the PS4 does with pixel shaders (Windy's ultraviolet "
                    "screen with holes around the fairies, Jimmy's spotlights and the holes around the balls) as "
                    "meshes; false = the plain dimming");
REXCVAR_DEFINE_BOOL(peggle1_dlc_motion, false, "Peggle1 DLC",
                    "The two powers' motion as the PS4 code has it: Windy's fairies on wobbling Bezier paths and her "
                    "time curves evaluated by the game's own CurvedVal, Jimmy's look-ahead with bounces off pegs; "
                    "false = straight eased paths, three-point curves, free-flight look-ahead");
REXCVAR_DEFINE_STRING(peggle1_dlc_test, "", "Peggle1 DLC",
                      "Test switches for unattended runs, separated by , or +: anyhit = the first peg a ball hits counts "
                      "as a green peg for the power; anypeg = Windy's fairies also take blue pegs of levels that "
                      "have no purple pegs; greens = 14 blue pegs become green at the first shot; oneorange = "
                      "all orange pegs but one become blue at the first shot; firstorange = the same, and the one "
                      "orange peg counts as hit (the first shot wins the level)");

extern const rex::PPCImageInfo PPCImageConfig;

namespace peggle1::dlc {

bool Enabled() {
  static const bool on = REXCVAR_GET(peggle1_dlc_powers);
  return on;
}
bool Logging() {
  static const bool on = REXCVAR_GET(peggle1_dlc_log);
  return on;
}
bool Overlays() {
  static const bool on = REXCVAR_GET(peggle1_dlc_overlays);
  return on;
}
bool Motion() {
  static const bool on = REXCVAR_GET(peggle1_dlc_motion);
  return on;
}
bool Test(const char* word) {
  static const std::string all = [] {
    std::string s = "," + REXCVAR_GET(peggle1_dlc_test) + ",";
    for (char& c : s) {
      if (c == '+' || c == ':' || c == ' ') c = ',';  // any of these separates (a comma splits test scripts' lists)
    }
    return s;
  }();
  return all.find("," + std::string(word) + ",") != std::string::npos;
}

// The table the generated code's indirect calls read (REX_LOOKUP_FUNC), then the dispatcher's own map.
PPCFunc* Guest::Lookup(uint32_t address) const {
  const rex::PPCImageInfo& image = PPCImageConfig;
  if (address - image.code_base < image.code_size + rex::runtime::FunctionDispatcher::kThunkReserveSize) {
    PPCFunc* const fn = *reinterpret_cast<PPCFunc**>(base + uint64_t(image.image_base) + image.image_size +
                                                     uint64_t(address - image.code_base) * 2);
    if (fn) return fn;
  }
  return rex::runtime::ResolveIndirectFunction(address);
}

GuestString::GuestString(const Guest& g, Frame& frame, std::string_view text) : g_(g) {
  const uint32_t chars = frame.Chars(text);
  address_ = frame.Alloc(32);
  g.Call(__imp__sub_8236D6A0, {address_, chars});
}
GuestString::~GuestString() { g_.Call(__imp__sub_82382BB8, {address_}); }

uint32_t Thunk(PPCFunc* fn) {
  auto* const runtime = rex::Runtime::instance();
  const uint32_t address =
      runtime && runtime->function_dispatcher() ? runtime->function_dispatcher()->AllocateThunk(fn, 0) : 0;
  if (!address) REXLOG_ERROR("[dlc] no address for a host function: the powers will not work");
  return address;
}

uint32_t Permanent(const Guest& g, uint32_t bytes) {
  const uint32_t a = g.Call(__imp__sub_82369B48, {bytes});
  if (a) std::memset(g.base + a, 0, bytes);
  return a;
}
uint32_t PermanentChars(const Guest& g, std::string_view text) {
  const uint32_t a = Permanent(g, uint32_t(text.size()) + 1);
  if (a) std::memcpy(g.base + a, text.data(), text.size());
  return a;
}
uint32_t CopyVtable(const Guest& g, uint32_t vtable, uint32_t slots) {
  const uint32_t a = Permanent(g, 4 + slots * 4);
  if (!a) return 0;
  std::memcpy(g.base + a, g.base + vtable - 4, 4 + slots * 4);
  return a + 4;
}

uint32_t NewClass(const Guest& g) {
  const uint32_t cls = g.Call(__imp__sub_82369B48, {48});
  if (cls) g.Call(__imp__sub_8237FEC0, {cls});
  return cls;
}

void FinishClass(const Guest& g, uint32_t cls, const char* name, uint32_t factory, uint32_t symbols, uint32_t size) {
  Frame frame(g, 128);
  const uint32_t parent = g.Call(__imp__sub_821B22E8);
  g.Virtual(cls, 24, {PermanentChars(g, name), parent, factory});
  const uint32_t system = g.Call(__imp__sub_82419708);
  if (!system) return;
  const uint32_t builder = g.Call(__imp__sub_8241C5F8, {system});
  if (!builder) return;
  GuestString text(g, frame, name);
  g.Virtual(builder, 16, {text.address(), symbols, size, 0});
}

void DeclareProperties(const Guest& g, uint32_t builder, uint32_t type, uint32_t cls, bool parent,
                       std::span<const Property> properties) {
  Frame frame(g, 128 + 128 * uint32_t(properties.size()));
  if (parent) {
    g.set32(cls + 40, type);
    g.set32(type + 120, cls);
    uint32_t base_type;
    {
      GuestString text(g, frame, "MasterPower");
      const uint32_t found = g.Virtual(builder, 40, {text.address()});
      base_type = g.Virtual(found, 44);
    }
    g.Virtual(builder, 44, {type, base_type, 0});
  }
  for (const Property& p : properties) {
    const uint32_t kind = g.Virtual(builder, 24, {p.kind, p.size});
    GuestString text(g, frame, p.name);
    g.Virtual(builder, 48, {type, text.address(), p.offset, kind});
  }
}

uint32_t Simulation(const Guest& g, uint32_t self) { return g.Virtual(g.u32(self + power::kRules), 0); }

void PostSound(const Guest& g, uint32_t self, uint32_t name) {
  const uint32_t board = g.u32(self + power::kBoard);
  if (!board || !name) return;
  const uint32_t audio = g.Virtual(board, 4);
  if (audio) g.Virtual(audio, 8, {name});
}

std::vector<uint32_t> Pointers(const Guest& g, uint32_t vector) {
  std::vector<uint32_t> out;
  const uint32_t begin = g.u32(vector), end = g.u32(vector + 4);
  if (!Guest::Plausible(begin) || end < begin || end - begin > 0x10000) return out;
  out.reserve((end - begin) / 4);
  for (uint32_t a = begin; a < end; a += 4) out.push_back(g.u32(a));
  return out;
}

uint32_t Random(const Guest& g, uint32_t simulation, uint32_t n) {
  return g.Call(__imp__sub_82439218, {simulation + sim::kRandom, n});
}
float Random01(const Guest& g, uint32_t simulation) { return float(Random(g, simulation, 1u << 20)) / float(1u << 20); }

}  // namespace peggle1::dlc

using peggle1::dlc::Guest;

// The static initialiser that creates MultiBall's class (821B23D8) and stores it. It runs once, among the game's
// other class registrations: PurplePegs joins them here.
extern "C" REX_FUNC(__imp__sub_82874588);
REX_HOOK_RAW(sub_82874588) {
  __imp__sub_82874588(ctx, base);
  if (!peggle1::dlc::Enabled()) return;
  const uint64_t r3 = ctx.r3.u64;
  const Guest g{ctx, base};
  peggle1::dlc::RegisterJimmy(g);
  peggle1::dlc::RegisterWindy(g);
  REXLOG_INFO("[dlc] powers of the PS4 release registered: PurplePegs (Windy), MultiBall extended (Jimmy)");
  ctx.r3.u64 = r3;
}

// The game's two debug-message functions (a printf to a string that the shipped build throws away): with
// peggle1_dlc_log their format string and first argument go to the log. They name what a failed load was looking
// for, e.g. "Error loading master shared group: %s not found" (8228DE88), which is otherwise a null entry in a list
// and a crash later.
namespace {
std::string Narrow(const uint8_t* base, uint32_t address, bool wide) {
  std::string s;
  if (!Guest::Plausible(address)) return s;
  for (uint32_t i = 0; i < 200; ++i) {
    const unsigned c = wide ? (unsigned(base[address + 2 * i]) << 8 | base[address + 2 * i + 1]) : base[address + i];
    if (!c) break;
    s.push_back(c >= 0x20 && c < 0x7F ? char(c) : '?');
  }
  return s;
}
void LogGameMessage(PPCContext& ctx, const uint8_t* base, bool wide) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 400) return;
  const std::string format = Narrow(base, ctx.r3.u32, wide);
  std::string argument = Narrow(base, ctx.r4.u32, wide);
  if (argument.empty()) argument = Narrow(base, ctx.r4.u32, !wide);
  // Some messages get their strings by value (a short std::string is 16 characters in two registers).
  std::string inline_text;
  for (const uint64_t r : {ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64}) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      const unsigned c = unsigned(r >> shift) & 0xFF;
      inline_text.push_back(c >= 0x20 && c < 0x7F ? char(c) : '.');
    }
    inline_text.push_back('|');
    // ... and a long one is a pointer in the first word.
    const std::string pointed = Narrow(base, uint32_t(r >> 32), false);
    if (pointed.size() >= 4) inline_text += "->\"" + pointed + "\"|";
  }
  REXLOG_INFO("[dlc] game message from {:08X}: \"{}\" (first argument {:08X} \"{}\"; registers as text {})",
              uint32_t(ctx.lr), format, ctx.r4.u32, argument, inline_text);
}
}  // namespace

// The sound engine's source factory (Wwise, one call for every voice that starts): it picks the decoder by the
// codec id of the sound's media (1 PCM, 2 ADPCM, 3 XMA; above 3 the registered ones: 4 Vorbis). The converted
// archive's new media are PCM, which no stock sound is: with peggle1_dlc_log the first voices of each codec are
// logged, so a run shows that the game started such a voice (r3 = the playing instance, its sound's source record
// at +100: the codec word at +24, how it is stored in the bits the factory tests at +16).
extern "C" REX_FUNC(__imp__sub_8252FC18);
REX_HOOK_RAW(sub_8252FC18) {
  if (!peggle1::dlc::Logging()) {
    __imp__sub_8252FC18(ctx, base);
    return;
  }
  const Guest g{ctx, base};
  const uint32_t instance = ctx.r3.u32;
  const uint32_t source = Guest::Plausible(instance) ? g.u32(instance + 100) : 0;
  const uint32_t plugin = Guest::Plausible(source) ? g.u32(source + 24) : 0;
  const uint32_t stored = Guest::Plausible(source) ? (g.u32(source + 16) >> 25) & 0x1F : 0;
  __imp__sub_8252FC18(ctx, base);
  static std::atomic<int> seen[5];
  const uint32_t codec = plugin >> 16;
  const int kind = codec < 4 ? int(codec) : 4;
  const int n = seen[kind].fetch_add(1);
  if (n < (kind == 1 || kind == 2 ? 400 : 3) || (n % 500) == 0) {
    REXLOG_INFO("[dlc] sound: voice {} with codec {:08X} ({}), source object {}; source record words {:08X} {:08X} "
                "{:08X} {:08X}",
                n + 1, plugin,
                stored == 1 ? "streamed file" : stored == 3 ? "in a bank" : stored == 2 ? "source plug-in" : "?",
                ctx.r3.u32 ? "made" : "NOT made", Guest::Plausible(source) ? g.u32(source) : 0,
                Guest::Plausible(source) ? g.u32(source + 4) : 0, Guest::Plausible(source) ? g.u32(source + 8) : 0,
                Guest::Plausible(source) ? g.u32(source + 12) : 0);
  }
}

// The start of a voice whose media is PCM in a bank (the PCM source's slot 9, vtable 82069DA0): it reads the
// medium's RIFX header (big-endian fields, format tag FFFE or the voice is refused with 7) and returns 1 when the
// voice can play. With peggle1_dlc_log: the results.
extern "C" REX_FUNC(__imp__sub_8256FEB8);
REX_HOOK_RAW(sub_8256FEB8) {
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8256FEB8(ctx, base);
  if (!peggle1::dlc::Logging()) return;
  static std::atomic<int> started{0}, refused{0};
  const int result = int(ctx.r3.u32);
  const int n = (result == 1 ? started : refused).fetch_add(1) + 1;
  if (n <= 3 || n % 50 == 0 || (result != 1 && n <= 40)) {
    // The playing instance (+12 of the source object): its media in memory at +332 / +336, its sound's source
    // record at +100 (the source id is the word before the codec's).
    const uint32_t instance = Guest::Plausible(self) ? g.u32(self + 12) : 0;
    const uint32_t media = Guest::Plausible(instance) ? g.u32(instance + 332) : 0;
    const uint32_t bytes = Guest::Plausible(instance) ? g.u32(instance + 336) : 0;
    const uint32_t source = Guest::Plausible(instance) ? g.u32(instance + 100) : 0;
    REXLOG_INFO("[dlc] sound: a PCM voice from a bank: start result {} ({} started, {} refused so far); media {:08X} "
                "{} bytes; source record words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                result, started.load(), refused.load(), media, bytes, Guest::Plausible(source) ? g.u32(source) : 0,
                Guest::Plausible(source) ? g.u32(source + 4) : 0, Guest::Plausible(source) ? g.u32(source + 8) : 0,
                Guest::Plausible(source) ? g.u32(source + 12) : 0, Guest::Plausible(source) ? g.u32(source + 16) : 0,
                Guest::Plausible(source) ? g.u32(source + 20) : 0, Guest::Plausible(source) ? g.u32(source + 24) : 0);
  }
}

extern "C" REX_FUNC(__imp__sub_8236EF40);
REX_HOOK_RAW(sub_8236EF40) {
  if (peggle1::dlc::Logging()) LogGameMessage(ctx, base, false);
  __imp__sub_8236EF40(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_8236EFB8);
REX_HOOK_RAW(sub_8236EFB8) {
  if (peggle1::dlc::Logging()) LogGameMessage(ctx, base, true);
  __imp__sub_8236EFB8(ctx, base);
}
