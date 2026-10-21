// Peggle 2 hooks for the native renderer (src/me, from masseffect-nx through banjo-nx and sa2-nx), with Peggle 2's
// addresses (docs/peggle1-notes.md, "Direct3D": found by tools/d3d_match.py against SA2's confirmed functions, every
// one identical in shape, score 1.00).
//
// The renderer reads the game's PM4 ring by itself; these hooks only feed it what the ring cannot say:
//
//   - Shader objects. NoteShaderObject reads the container copy inside a D3D shader object and its microcode pointer.
//     The draw-time shader load is sub_8262B248 (r3 = device; SA2's sub_82556A60, the same code): it reads the bound
//     VS from device+13072 and the PS from device+13068 (the same offsets as SA2's 2012 XDK), so the objects are
//     noted there, before the original patches the VS for the vertex declaration.
//   - Draw records, a hint for pairing (NoteDrawCall: primitive type, then r5..r7 as D3D received them). Peggle 2
//     does not link D3DDevice_DrawVertices; its draws go through
//       D3DDevice_BeginVertices       sub_826221A0 (dev, type, vertex count, stride): the game's own batches
//                                     (sub_824A3EB0), D3D's sub_8261F310, and DrawVerticesUP (sub_82622660), which
//                                     calls it, so DrawVerticesUP needs no hook of its own
//       D3DDevice_DrawIndexedVertices sub_826226A8 (dev, type, base, start, count)
//     The third caller of the shader load, sub_82620D88, is Direct3D's resolve draw (only called by the Resolve,
//     sub_82620FE8); the ring handles resolves itself.
//   - Shader containers for the library (peggle1_dump_shaders = true): the blob D3DDevice_CreateVertexShader
//     (sub_82613480) and D3DDevice_CreatePixelShader (sub_82613298) receive in r3. It describes itself: [0] flags,
//     [1] header size (the microcode starts there), [2] microcode size. Written whole to shaders/vs_<hash>.bin /
//     ps_<hash>.bin next to the executable, once each; tools/build_shader_library.sh turns the folder into the shader
//     package.
//
//   - The D3D GPU wait, sub_8261D378 (Mass Effect's sub_8222FA98, the same code): 32 db16cyc delays, then it compares
//     the read-pointer word the GPU writes back ([[device+11024]]) with the value it saw last time ([r3+8]) and,
//     after a timeout with no change, calls the "GPU is hung" handler sub_82628988. Its callers (D3DDevice_Swap,
//     the ring-space and fence waits) loop on it: a busy core on the console. With masseffect_wait_blocking_ring the
//     hook sleeps until the ring thread writes the read pointer back (me::native::WaitRingProgress), then calls the
//     original. Not while the GPU is already declared hung (bit 0x02 of [device+11069], the original's own test).
//
//   - The native Direct3D layer (peggle1_api_draws, peggle1_api_clears; sa2-nx's, src/me/peggle1_api_*.cpp, mapped to
//     Peggle 2's addresses in out/api_map.md): BeginVertices, DrawVerticesUP, Clear and the window-scissor writer
//     (sub_82617E58: SetViewport, SetScissorRect, the surface setters) are native code that keeps the Direct3D device
//     as the game sees it and writes the renderer's records instead of PM4. A call in a state the native code does not
//     cover runs the old path below (counted in the [api] line). Verify mode runs the original on a device copy first
//     and compares (the inline-data allocator sub_826233C8 answers such a shadow run at once).
//   - The direct renderer, step 2 (peggle1_cpu_direct_flush, sa2-nx's): inside a converted draw call (below), Direct3D's
//     three flush functions write the call's record themselves: the shader constants sub_8262A4B0, the fetch constants
//     sub_8262A358 and the register groups sub_8262A118 (SA2's sub_82555CC8 / sub_82555B70 / sub_82555930).
//   - The direct renderer, step 1 (peggle1_cpu_direct_state, from sa2-nx): around both draw functions,
//     BeginDrawDirect and EndDrawDirect turn the state packets the draw function wrote into a host record the ring
//     applies (one NOP packet covers them; the draw packet and every address Direct3D keeps stay where they were). The
//     only guest memory a hook writes: the NOP's three words over the packets' first three, which no guest code reads
//     back. The device fields are SA2's (+48 write pointer, last word written; +52 / +56 the segment's limits, which
//     sub_826233C8 pulls in to force a submission), except BeginVertices' end (+13844, see its hook).
//
// The game frame counter (D3DDevice_Swap) lives in src/peggle1_frame_hooks.cpp. Every hook here calls the original,
// or does natively what the original does (the native layer, the native flush), registers and memory included.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "me/peggle1_api.h"

#include "me/me_native_system.h"
#if PEGGLE1_RENUT_RENDERER
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/banjo_shared_hooks.h"
#endif

REXCVAR_DEFINE_BOOL(peggle1_dump_shaders, false, "Peggle1",
                    "Write every shader container the game creates to shaders/ next to the executable "
                    "(vs_<hash>.bin / ps_<hash>.bin), for tools/build_shader_library.sh");

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

// The same host offset as the recompiled code's loads (on Windows the 0xE0000000 view is shifted by 0x1000).
inline uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
#if defined(_WIN32)
  if (address >= 0xE0000000u) address += 0x1000u;
#endif
  return Load32(base, address);
}

constexpr uint32_t kDeviceReadPointerWord = 11024;  // pointer to the word the GPU writes its read pointer back to
constexpr uint32_t kDeviceGpuState = 11069;         // byte; 0x02 = the GPU was declared hung

// BeginVertices' end of packets (the last word written), published to the write pointer +48 by EndVertices.
constexpr uint32_t kDeviceVerticesEnd = 13844;  // 0x3614

constexpr uint32_t kDeviceVertexShader = 13072;  // 0x3310
constexpr uint32_t kDevicePixelShader = 13068;   // 0x330C

// The draw records (NoteDrawCall) read the bound shaders from the same device fields.
const bool g_device_layout_set = [] {
  me::native::SetDeviceShaderOffsets(kDeviceVertexShader, kDevicePixelShader);
  return true;
}();

std::mutex g_dump_mutex;
std::unordered_set<uint64_t> g_dumped;

void DumpShaderBlob(const uint8_t* base, uint32_t blob_address, bool vertex) {
  if (!REXCVAR_GET(peggle1_dump_shaders) || !blob_address) {
    return;
  }
  const uint32_t header = Load32(base, blob_address + 4);
  const uint32_t ucode = Load32(base, blob_address + 8);
  if (header < 12 || header > (1u << 20) || !ucode || ucode > (1u << 20)) {
    REXLOG_WARN("[shaders] implausible {} blob at {:08X} (header {}, microcode {}), skipped", vertex ? "VS" : "PS",
                blob_address, header, ucode);
    return;
  }
  const uint8_t* blob = base + blob_address;
  const size_t size = size_t(header) + ucode;
  const uint64_t hash = XXH3_64bits(blob, size);
  {
    std::lock_guard lock(g_dump_mutex);
    if (!g_dumped.insert(hash).second) {
      return;
    }
  }
  const auto folder = rex::filesystem::GetExecutableFolder() / "shaders";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const auto path = folder / fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", hash);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(blob), std::streamsize(size));
}

}  // namespace

// Draw-time shader load (FlushState's shader part, called from BeginVertices and DrawIndexedVertices).
REX_EXTERN(__imp__sub_8262B248);
REX_HOOK_RAW(sub_8262B248) {
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD(sub_8262B248)  // Renut's renderer chosen: not the native renderer's
#endif
  const uint32_t device = ctx.r3.u32;
  if (device >= 0x40000000 && device < 0x7F000000) {
    const uint32_t vs = Load32(base, device + kDeviceVertexShader), ps = Load32(base, device + kDevicePixelShader);
    if (!me::native::ShaderObjectsFinished(vs, ps)) {  // peggle1_cpu_note_shader_pair: both finished = two loads
      me::native::NoteShaderObject(base, vs, true);
      me::native::NoteShaderObject(base, ps, false);
    }
  }
  __imp__sub_8262B248(ctx, base);
}

// Each draw hook also hands the state packets the draw function writes to the ring as a host record
// (peggle1_cpu_direct_state: BeginDrawDirect before the original, EndDrawDirect right after it).
REX_EXTERN(__imp__sub_826221A0);
REX_HOOK_RAW(sub_826221A0) {  // D3DDevice_BeginVertices
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD_SHARED(sub_826221A0)  // Renut's renderer chosen: not the native renderer's
#endif
  if (peggle1::api::BeginVertices(ctx, base)) return;  // peggle1_api_draws: natively (no PM4 for the draw)
  auto direct = me::native::BeginDrawDirect(base, ctx.r3.u32);
  me::native::NoteDrawCall(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, &direct);
  __imp__sub_826221A0(ctx, base);
  // BeginVertices returns the vertex space (0: none, nothing written) and keeps the last word it wrote in
  // device+13844 until EndVertices (sub_82622190: +48 = +13844); +48 stays where it was.
  if (!ctx.r3.u32) direct.device = 0;
  direct.end_field = kDeviceVerticesEnd;
  me::native::EndDrawDirect(base, direct);
}

REX_EXTERN(__imp__sub_826226A8);
REX_HOOK_RAW(sub_826226A8) {  // D3DDevice_DrawIndexedVertices (not native in sa2-nx's layer: step 1 + 2 here)
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD_SHARED(sub_826226A8)  // Renut's renderer chosen: not the native renderer's
#endif
  auto direct = me::native::BeginDrawDirect(base, ctx.r3.u32);
  me::native::NoteDrawCall(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, &direct);
  __imp__sub_826226A8(ctx, base);
  me::native::EndDrawDirect(base, direct);
}

// D3DDevice_DrawVerticesUP (dev, type, count, data, stride): natively with peggle1_api_draws; otherwise the original,
// whose BeginVertices call comes through the hook above.
REX_EXTERN(__imp__sub_82622660);
REX_HOOK_RAW(sub_82622660) {
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD_SHARED(sub_82622660)  // Renut's renderer chosen: not the native renderer's
#endif
  if (peggle1::api::DrawVerticesUP(ctx, base)) return;
  __imp__sub_82622660(ctx, base);
}

// peggle1_cpu_direct_flush (the direct renderer, step 2): Direct3D's state flush functions, called by the draw
// functions' inlined FlushState. Inside a converted draw call they write the call's direct record themselves
// (FlushRunsDirect); otherwise they run as before. Arguments: r3 = device, r4 = dirty mask (bit 63 first), r5 = the
// first register, r6 = the shadow in the device (the fetch flush has its own: device+1152, registers 0x4800).
REX_EXTERN(__imp__sub_8262A4B0);
REX_HOOK_RAW(sub_8262A4B0) {  // shader constants: 16 registers a bit (r5 = 0x4000 VS / 0x4400 PS)
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD(sub_8262A4B0)  // Renut's renderer chosen: not the native renderer's
#endif
  if (me::native::FlushRunsDirect(ctx, base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                  me::native::FlushKind::kConstants, &__imp__sub_8262A4B0))
    return;
  __imp__sub_8262A4B0(ctx, base);
}
REX_EXTERN(__imp__sub_8262A358);
REX_HOOK_RAW(sub_8262A358) {  // fetch constants: 6 registers a bit from 0x4800, then the 0x5000 end packet
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD(sub_8262A358)  // Renut's renderer chosen: not the native renderer's
#endif
  if (me::native::FlushRunsDirect(ctx, base, ctx.r3.u32, ctx.r4.u64, 0x4800, ctx.r3.u32 + 1152,
                                  me::native::FlushKind::kFetch, &__imp__sub_8262A358))
    return;
  __imp__sub_8262A358(ctx, base);
}
REX_EXTERN(__imp__sub_8262A118);
REX_HOOK_RAW(sub_8262A118) {  // register groups: 1 register a bit
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD(sub_8262A118)  // Renut's renderer chosen: not the native renderer's
#endif
  if (me::native::FlushRunsDirect(ctx, base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                  me::native::FlushKind::kGroups, &__imp__sub_8262A118))
    return;
  __imp__sub_8262A118(ctx, base);
}

// peggle1_api_draws, verify mode: Direct3D's inline-data allocator answers at once inside a shadow run of the original.
REX_EXTERN(__imp__sub_826233C8);
REX_HOOK_RAW(sub_826233C8) {
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD_SHARED(sub_826233C8)  // Renut's renderer chosen: not the native renderer's
#endif
  if (peggle1::api::ShadowInlineAlloc(ctx, base)) return;
  __imp__sub_826233C8(ctx, base);
}

// peggle1_api_clears: D3DDevice_Clear as a record.
REX_EXTERN(__imp__sub_8261EE00);
REX_HOOK_RAW(sub_8261EE00) {
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD_SHARED(sub_8261EE00)  // Renut's renderer chosen: not the native renderer's
#endif
  if (peggle1::api::Clear(ctx, base)) return;
  __imp__sub_8261EE00(ctx, base);
}

// peggle1_api_draws: Direct3D's window offset/scissor writer (registers 0x2080-0x2082) as a record.
REX_EXTERN(__imp__sub_82617E58);
REX_HOOK_RAW(sub_82617E58) {
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD_SHARED(sub_82617E58)  // Renut's renderer chosen: not the native renderer's
#endif
  if (peggle1::api::WindowScissor(ctx, base)) return;
  __imp__sub_82617E58(ctx, base);
}

REX_EXTERN(__imp__sub_82613480);
REX_HOOK_RAW(sub_82613480) {  // D3DDevice_CreateVertexShader (r3 = the container blob)
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD_SHARED(sub_82613480)  // Renut's renderer chosen: not the native renderer's
#endif
  DumpShaderBlob(base, ctx.r3.u32, true);
  __imp__sub_82613480(ctx, base);
}

REX_EXTERN(__imp__sub_82613298);
REX_HOOK_RAW(sub_82613298) {  // D3DDevice_CreatePixelShader (r3 = the container blob)
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD_SHARED(sub_82613298)  // Renut's renderer chosen: not the native renderer's
#endif
  DumpShaderBlob(base, ctx.r3.u32, false);
  __imp__sub_82613298(ctx, base);
}

REX_EXTERN(__imp__sub_8261D378);
REX_HOOK_RAW(sub_8261D378) {  // D3D GPU wait: has the ring read pointer moved since the last iteration?
#if PEGGLE1_RENUT_RENDERER
  BANJO_RENUT_YIELD_SHARED(sub_8261D378)  // Renut's renderer chosen: not the native renderer's
#endif
  const uint32_t wait_state = ctx.r3.u32;
  const uint32_t device = LoadGuest32(base, wait_state);
  if (device >= 0x40000000 && device < 0x7F000000 && !(base[device + kDeviceGpuState] & 0x02)) {
    const uint32_t word = LoadGuest32(base, device + kDeviceReadPointerWord);
    if (word) {
      me::native::WaitRingProgress(
          [&] { return LoadGuest32(base, word) == LoadGuest32(base, wait_state + 8); });
    }
  }
  __imp__sub_8261D378(ctx, base);
}
