// Peggle 2 (from sa2-nx, 2026-10-07; SA2 -> Peggle 2 addresses from out/api_map.md: Peggle 2's Direct3D is SA2's
// instruction for instruction, XDK 21256 against 21076): the native Direct3D layer (the API-level renderer, docs/api-renderer.md).
//
// The game's Xbox 360 Direct3D (the XDK library linked into the executable) turned every call into PM4 packets for an
// emulated Xenos command processor. Here its entry points are native code: they keep the Direct3D device structure
// exactly as the game sees it (the game reads and writes parts of it inline), and hand the renderer host records of
// operations (src/me/me_native_system.cpp, the direct-state record format) instead of PM4. Each group of entry points
// has its own key (peggle1_api_*); verify mode (peggle1_cpu_verify) runs the original for real and the native code on an
// overlay of guest memory, and compares everything (device bytes, guest memory the native code wrote, and its ops
// against the original's packets decoded the same way).
#pragma once

#include <cstdint>

struct PPCContext;

namespace peggle1::api {

// Draws (peggle1_api_draws): Direct3D's inline-vertex draws with its draw-time state flush and shader load. Each returns
// false when it did not run (key off, native renderer off, a path the native code does not take): the hook then runs
// its old path. (sa2-nx's DrawVertices is not linked in Peggle 2; DrawIndexedVertices keeps the direct-state path.)
// D3DDevice_BeginVertices (returns the inline vertices' address in r3) and D3DDevice_DrawVerticesUP.
bool BeginVertices(PPCContext& ctx, uint8_t* base);
bool DrawVerticesUP(PPCContext& ctx, uint8_t* base);
// Verify mode: Direct3D's inline-data allocator inside a shadow run of the original (a scratch address, no waiting).
bool ShadowInlineAlloc(PPCContext& ctx, uint8_t* base);
// D3DDevice_Clear (peggle1_api_clears).
bool Clear(PPCContext& ctx, uint8_t* base);
// Direct3D's window offset/scissor writer (registers 0x2080-0x2082; SetViewport, SetScissorRect, the surface setters).
bool WindowScissor(PPCContext& ctx, uint8_t* base);

// The [api] report line (every 10 s, with the renderer's report).
void Report();

}  // namespace peggle1::api
