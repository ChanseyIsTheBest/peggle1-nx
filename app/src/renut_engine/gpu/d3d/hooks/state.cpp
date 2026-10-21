/**
 * @file    gpu/d3d/hooks/state.cpp
 * @brief   N&B's D3D state calls, hooked with the native ports (d3d_native.h).
 *
 * The render and sampler state setters are the ones N&B's state tables name
 * (D3D::InitializeApiState puts them into every device's dispatch table,
 * which the game's inline SetRenderState / SetSamplerState call through).
 * The rest are D3DDevice_Set* and the CDevice internals that write the ring.
 */
#include <cstring>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/guest_memory.h"

using namespace renut::gpu;

namespace {

template <u32 State>
void SetRenderStateHook(u32 device, u32 value) {
  native::SetRenderState(native::DeviceAt(device), State, value);
}

template <u32 State>
void SetSamplerStateHook(u32 device, u32 sampler, u32 value) {
  native::SetSamplerState(native::DeviceAt(device), sampler, State, value);
}

template <u32 State>
u32 GetRenderStateHook(u32 device) {
  return native::GetRenderState(native::DeviceAt(device), State);
}

template <u32 State>
u32 GetSamplerStateHook(u32 device, u32 sampler) {
  return native::GetSamplerState(native::DeviceAt(device), sampler, State);
}

// The device's AddRef of a resource it hands out (D3DResource_AddRef,
// resource.cpp).
u32 AddRefResource(u32 resource) {
  return native::AddRefResource(resource);
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x8264D8D0]
u32 D3DDevice_GetRenderTarget_hook(u32 device, u32 index) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d && index < 4 ? d.W(native::off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x8264D910]
u32 D3DDevice_GetDepthStencilSurface_hook(u32 device) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d ? d.W(native::off::DepthStencil) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber,
//     UINT *pOffsetInBytes, UINT *pStride) [0x8264D840]: the offset is the
// stream's fetch constant address less the buffer's.
u32 D3DDevice_GetStreamSource_hook(u32 device, u32 stream, mapped_u32 offset, mapped_u32 stride) {
  const native::Device d = native::DeviceAt(device);
  if (!d || stream >= 16)
    return 0;
  const u32 buffer = d.W(native::off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = d.W(native::off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - native::GpuAddress(GuestPtr<const be_u32>(buffer)[6]);
    AddRefResource(buffer);
  }
  if (offset)
    *offset = bytes;
  if (stride)
    *stride = u32(d.B(native::off::StreamStrides + stream)) << 2;
  return buffer;
}

// void D3DDevice_GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82276AC0]
void D3DDevice_GetViewport_hook(u32 device, mapped_u32 viewport) {
  const native::Device d = native::DeviceAt(device);
  if (!d || !viewport)
    return;
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    viewport[i] = u32(native::TruncateToI64(d.F(0x3168 + i * 4)));
  viewport[4] = d.W(0x3178);
  viewport[5] = d.W(0x317C);
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x8264DAE8]
void D3DDevice_SetClipPlane_hook(u32 device, u32 index, mapped_u32 plane) {
  native::Device d = native::DeviceAt(device);
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(0x2820 + index * 16 + i * 4, plane[i]);
  d.Dirty(4, u64(1) << (54 - index));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x8264DB38]: a vertex fetch slot of the
// 3 in a fetch constant, made a vertex fetch constant first.
void D3DDevice_SetVertexFetchConstant_hook(u32 device, u32 reg, u32 vertex_buffer, u32 offset) {
  native::Device d = native::DeviceAt(device);
  const auto *vb = vertex_buffer ? GuestPtr<const be_u32>(vertex_buffer) : nullptr;
  if (!d || !vb || reg > 0x5F)
    return;
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = native::off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.SetW(constant, native::Insert(d.W(constant), 1, 0, 2));
  d.Dirty(3, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, native::GpuAddress(u32(vb[6]) + offset));
  d.SetW(constant + part * 8 + 4, native::Insert(u32(vb[7]) - offset, 1, 28, 2));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0.
void SetShaderConstantsB(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = base + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((u32(data[i]) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = u32(data[i * 4]) & 0xFF, y = u32(data[i * 4 + 1]) & 0xFF;
    const u32 z = u32(data[i * 4 + 2]) & 0xFF;
    d.SetW(base + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// void D3DDevice_SetVertexShaderConstantB(D3DDevice*, UINT StartRegister,
//     const BOOL*, UINT BoolCount) [0x82221930], and the others alike.
void D3DDevice_SetVertexShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2780, start, data, count);
}
void D3DDevice_SetPixelShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2790, start, data, count);
}
void D3DDevice_SetVertexShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27A0, start, data, count);
}
void D3DDevice_SetPixelShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27E0, start, data, count);
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x826561F8]: the inline end
// of BeginIndexedVertices, back to the ring pointer it saved.
void D3DDevice_EndIndexedVertices_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (d)
    d.SetW(native::off::Ring, d.W(0x3484));
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x8222F410]; the mask is the sampler's fetch
// constant bit, which the port derives from Sampler.
void D3DDevice_SetTexture_hook(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, texture);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x8222A408]
void D3DDevice_SetStreamSource_hook(u32 device, u32 stream, u32 vertex_buffer, u32 offset,
                                    u32 stride) {
  native::SetStreamSource(native::DeviceAt(device), stream, vertex_buffer, offset, stride);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x8220EE38]
void D3DDevice_SetIndices_hook(u32 device, u32 index_buffer) {
  native::SetIndices(native::DeviceAt(device), index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x82205700]
void D3DDevice_SetVertexDeclaration_hook(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x82222EA0]
void D3DDevice_SetViewport_hook(u32 device, u32 viewport) {
  native::SetViewport(native::DeviceAt(device), viewport);
}

// void D3D::SetViewport(D3DDevice*, float X, float Y, float Width, float Height,
//     float MinZ, float MaxZ) [0x82222F20]
void D3D_SetViewport_hook(u32 device, float x, float y, float width, float height, float min_z,
                          float max_z) {
  native::SetViewport(native::DeviceAt(device), x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x82223178]
void D3DDevice_SetScissorRect_hook(u32 device, mapped_u32 rect) {
  if (!rect)
    return;
  native::SetScissorRect(native::DeviceAt(device), i32(u32(rect[0])), i32(u32(rect[1])),
                         i32(u32(rect[2])), i32(u32(rect[3])));
}

// void D3D::SetSurfaceClip(CDevice*, int, int, int, int) [0x82223270]: the
// window scissor and per-tile clip packets, nothing else.
void D3D_SetSurfaceClip_hook(u32 /*device*/, u32, u32, u32, u32) {}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x8227AB68]
void D3DDevice_SetRenderTarget_hook(u32 device, u32 index, u32 surface) {
  native::SetRenderTarget(native::DeviceAt(device), index, surface);
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x8227AF40]
void D3DDevice_SetDepthStencilSurface_hook(u32 device, u32 surface) {
  native::SetDepthStencilSurface(native::DeviceAt(device), surface);
}

// void D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x8227AEB0]
void D3D_SetSurfaceInfo_hook(u32 device, u32 surface) {
  native::SetSurfaceInfo(native::DeviceAt(device), surface);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x8222AA18]
void D3DDevice_SetPredication_hook(u32 device, u32 mask) {
  native::SetPredication(native::DeviceAt(device), mask);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x82658678]
void D3DDevice_SetCommandBufferPredication_hook(u32 device, u32 tile_predication,
                                                u32 run_predication) {
  native::SetCommandBufferPredication(native::DeviceAt(device), tile_predication,
                                      run_predication);
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x82220B20]
void D3DDevice_SetShaderGPRAllocation_hook(u32 device, u32 flags, u32 vertex_count,
                                           u32 pixel_count) {
  native::SetShaderGPRAllocation(native::DeviceAt(device), flags, vertex_count, pixel_count);
}

// void D3D::CleanupGprAllocationSetup(CDevice*) [0x8264E2C0]
void D3D_CleanupGprAllocationSetup_hook(u32 device) {
  native::CleanupGprAllocationSetup(native::DeviceAt(device));
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x82658988]
void D3DDevice_SetScreenExtentQueryMode_hook(u32 device, u32 mode) {
  native::SetScreenExtentQueryMode(native::DeviceAt(device), mode);
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x8264E1A8]
void D3DDevice_UnsetAll_hook(u32 device) {
  native::UnsetAll(native::DeviceAt(device));
}

// void D3D::ResetAllState(CDevice*, BOOL) [0x82287D38]
void D3D_ResetAllState_hook(u32 device, u32 /*skip_packets*/) {
  native::ResetAllState(native::DeviceAt(device));
}

// void D3D::InvalidateConstants(D3DDevice*) [0x8264E128]
void D3D_InvalidateConstants_hook(u32 device) {
  native::InvalidateConstants(native::DeviceAt(device));
}

// void D3DDevice_BlockUntilIdle(D3DDevice*) [0x82657488] (IDA: the second
// D3D::InvalidateConstants): waits for the device's current fence (N&B calls
// it before releasing a device).
void D3DDevice_BlockUntilIdle_hook(u32 device) {
  native::InvalidateConstantsAndWait(native::DeviceAt(device));
}

} // namespace

// Render states, by their index in N&B's table (off_82E506F0).
BANJO_RENUT_HOOK(sub_82434640, SetRenderStateHook<10>);  // D3DDevice_SetRenderState_ZEnable  // N&B sub_8264C140
BANJO_RENUT_HOOK(sub_824346B0, SetRenderStateHook<11>);  // D3DDevice_SetRenderState_ZFunc  // N&B sub_821FB7B0
BANJO_RENUT_HOOK(sub_82434680, SetRenderStateHook<12>);  // D3DDevice_SetRenderState_ZWriteEnable  // N&B sub_8264C180
BANJO_RENUT_HOOK(sub_82433ED0, SetRenderStateHook<13>);  // D3DDevice_SetRenderState_FillMode  // N&B sub_8228F330
BANJO_RENUT_HOOK(sub_82433EA0, SetRenderStateHook<14>);  // D3DDevice_SetRenderState_CullMode  // N&B sub_8228F350
BANJO_RENUT_HOOK(sub_82433FB0, SetRenderStateHook<15>);  // D3DDevice_SetRenderState_AlphaBlendEnable  // N&B sub_8222A718
BANJO_RENUT_HOOK(sub_82434340, SetRenderStateHook<16>);  // D3DDevice_SetRenderState_SeparateAlphaBlendEnable  // N&B sub_8222A690
BANJO_RENUT_HOOK(sub_82434468, SetRenderStateHook<17>);  // D3DDevice_SetRenderState_BlendFactor  // N&B sub_8226E8E0
BANJO_RENUT_HOOK(sub_824340D0, SetRenderStateHook<18>);  // D3DDevice_SetRenderState_SrcBlend  // N&B sub_82230090
BANJO_RENUT_HOOK(sub_82434160, SetRenderStateHook<19>);  // D3DDevice_SetRenderState_DestBlend  // N&B sub_822307B8
BANJO_RENUT_HOOK(sub_82434040, SetRenderStateHook<20>);  // D3DDevice_SetRenderState_BlendOp  // N&B sub_82226380
BANJO_RENUT_HOOK(sub_82434260, SetRenderStateHook<21>);  // D3DDevice_SetRenderState_SrcBlendAlpha  // N&B sub_822672B8
BANJO_RENUT_HOOK(sub_824342D0, SetRenderStateHook<22>);  // D3DDevice_SetRenderState_DestBlendAlpha  // N&B sub_822208E8
BANJO_RENUT_HOOK(sub_824341F0, SetRenderStateHook<23>);  // D3DDevice_SetRenderState_BlendOpAlpha  // N&B sub_8221F5F8
BANJO_RENUT_HOOK(sub_82433F00, SetRenderStateHook<24>);  // D3DDevice_SetRenderState_AlphaTestEnable  // N&B sub_8264BE98
BANJO_RENUT_HOOK(sub_824343D8, SetRenderStateHook<25>);  // D3DDevice_SetRenderState_AlphaRef  // N&B sub_8264BF50
BANJO_RENUT_HOOK(sub_82434438, SetRenderStateHook<26>);  // D3DDevice_SetRenderState_AlphaFunc  // N&B sub_8264BFB8
BANJO_RENUT_HOOK(sub_824346E8, SetRenderStateHook<27>);  // D3DDevice_SetRenderState_StencilEnable  // N&B sub_8264C1C0
BANJO_RENUT_HOOK(sub_82434728, SetRenderStateHook<28>);  // D3DDevice_SetRenderState_TwoSidedStencilMode  // N&B sub_8264C200
BANJO_RENUT_HOOK(sub_82434790, SetRenderStateHook<29>);  // D3DDevice_SetRenderState_StencilFail  // N&B sub_8264C268
BANJO_RENUT_HOOK(sub_824347C8, SetRenderStateHook<30>);  // D3DDevice_SetRenderState_StencilZFail  // N&B sub_8264C2A0
BANJO_RENUT_HOOK(sub_82434800, SetRenderStateHook<31>);  // D3DDevice_SetRenderState_StencilPass  // N&B sub_8264C2D8
BANJO_RENUT_HOOK(sub_82434760, SetRenderStateHook<32>);  // D3DDevice_SetRenderState_StencilFunc  // N&B sub_8264C238
BANJO_RENUT_HOOK(sub_82434900, SetRenderStateHook<33>);  // D3DDevice_SetRenderState_StencilRef  // N&B sub_8264C3D8
BANJO_RENUT_HOOK(sub_82434920, SetRenderStateHook<34>);  // D3DDevice_SetRenderState_StencilMask  // N&B sub_8264C3F8
BANJO_RENUT_HOOK(sub_82434940, SetRenderStateHook<35>);  // D3DDevice_SetRenderState_StencilWriteMask  // N&B sub_8264C418
BANJO_RENUT_HOOK(sub_82434860, SetRenderStateHook<36>);  // D3DDevice_SetRenderState_CCWStencilFail  // N&B sub_8264C338
BANJO_RENUT_HOOK(sub_82434898, SetRenderStateHook<37>);  // D3DDevice_SetRenderState_CCWStencilZFail  // N&B sub_8264C370
BANJO_RENUT_HOOK(sub_824348D0, SetRenderStateHook<38>);  // D3DDevice_SetRenderState_CCWStencilPass  // N&B sub_8264C3A8
BANJO_RENUT_HOOK(sub_82434830, SetRenderStateHook<39>);  // D3DDevice_SetRenderState_CCWStencilFunc  // N&B sub_8264C308
BANJO_RENUT_HOOK(sub_82434960, SetRenderStateHook<40>);  // D3DDevice_SetRenderState_CCWStencilRef  // N&B sub_8264C438
BANJO_RENUT_HOOK(sub_82434980, SetRenderStateHook<41>);  // D3DDevice_SetRenderState_CCWStencilMask  // N&B sub_8264C458
BANJO_RENUT_HOOK(sub_824349A0, SetRenderStateHook<42>);  // D3DDevice_SetRenderState_CCWStencilWriteMask  // N&B sub_8264C478
BANJO_RENUT_HOOK(sub_824349C0, SetRenderStateHook<43>);  // D3DDevice_SetRenderState_ClipPlaneEnable  // N&B sub_8228F370
BANJO_RENUT_HOOK(sub_82434CE8, SetRenderStateHook<44>);  // D3DDevice_SetRenderState_PointSize  // N&B sub_8264C6C0
BANJO_RENUT_HOOK(sub_82434D40, SetRenderStateHook<45>);  // D3DDevice_SetRenderState_PointSizeMin  // N&B sub_8264C720
BANJO_RENUT_HOOK(sub_82434CD8, SetRenderStateHook<46>);  // D3DDevice_SetRenderState_PointSpriteEnable  // N&B sub_8264C6B0
BANJO_RENUT_HOOK(sub_82434D90, SetRenderStateHook<47>);  // D3DDevice_SetRenderState_PointSizeMax  // N&B sub_822687D0
BANJO_RENUT_HOOK(sub_82434B88, SetRenderStateHook<48>);  // D3DDevice_SetRenderState_MultiSampleAntiAlias  // N&B sub_8264C598
BANJO_RENUT_HOOK(sub_82434BB8, SetRenderStateHook<49>);  // D3DDevice_SetRenderState_MultiSampleMask  // N&B sub_8264C5C8
BANJO_RENUT_HOOK(sub_82436FF8, SetRenderStateHook<50>);  // D3DDevice_SetRenderState_ScissorTestEnable  // N&B sub_8264C4A8
BANJO_RENUT_HOOK(sub_82434A18, SetRenderStateHook<51>);  // D3DDevice_SetRenderState_SlopeScaleDepthBias  // N&B sub_8264C4C0
BANJO_RENUT_HOOK(sub_82434AE0, SetRenderStateHook<52>);  // D3DDevice_SetRenderState_DepthBias  // N&B sub_821FB718
BANJO_RENUT_HOOK(sub_82434BD8, SetRenderStateHook<53>);  // D3DDevice_SetRenderState_ColorWriteEnable  // N&B sub_821FB6E0
BANJO_RENUT_HOOK(sub_82434C18, SetRenderStateHook<54>);  // D3DDevice_SetRenderState_ColorWriteEnable1  // N&B sub_8264C5F0
BANJO_RENUT_HOOK(sub_82434C58, SetRenderStateHook<55>);  // D3DDevice_SetRenderState_ColorWriteEnable2  // N&B sub_8264C630
BANJO_RENUT_HOOK(sub_82434C98, SetRenderStateHook<56>);  // D3DDevice_SetRenderState_ColorWriteEnable3  // N&B sub_8264C670
BANJO_RENUT_HOOK(sub_824355C0, SetRenderStateHook<57>);  // D3DDevice_SetRenderState_TessellationMode  // N&B sub_8264CDC0
BANJO_RENUT_HOOK(sub_82435550, SetRenderStateHook<58>);  // D3DDevice_SetRenderState_MinTessellationLevel  // N&B sub_8264CD50
BANJO_RENUT_HOOK(sub_82435588, SetRenderStateHook<59>);  // D3DDevice_SetRenderState_MaxTessellationLevel  // N&B sub_8264CD88
BANJO_RENUT_HOOK(sub_82434E60, SetRenderStateHook<60>);  // D3DDevice_SetRenderState_Wrap0  // N&B sub_8264C780
BANJO_RENUT_HOOK(sub_82434E80, SetRenderStateHook<61>);  // D3DDevice_SetRenderState_Wrap1  // N&B sub_8264C7A0
BANJO_RENUT_HOOK(sub_82434EA8, SetRenderStateHook<62>);  // D3DDevice_SetRenderState_Wrap2  // N&B sub_8264C7C8
BANJO_RENUT_HOOK(sub_82434ED0, SetRenderStateHook<63>);  // D3DDevice_SetRenderState_Wrap3  // N&B sub_8264C7F0
BANJO_RENUT_HOOK(sub_82434EF8, SetRenderStateHook<64>);  // D3DDevice_SetRenderState_Wrap4  // N&B sub_8264C818
BANJO_RENUT_HOOK(sub_82434F20, SetRenderStateHook<65>);  // D3DDevice_SetRenderState_Wrap5  // N&B sub_8264C840
BANJO_RENUT_HOOK(sub_82434F48, SetRenderStateHook<66>);  // D3DDevice_SetRenderState_Wrap6  // N&B sub_8264C868
BANJO_RENUT_HOOK(sub_82434F70, SetRenderStateHook<67>);  // D3DDevice_SetRenderState_Wrap7  // N&B sub_8264C890
BANJO_RENUT_HOOK(sub_82434F90, SetRenderStateHook<68>);  // D3DDevice_SetRenderState_Wrap8  // N&B sub_8264C8B0
BANJO_RENUT_HOOK(sub_82434FB0, SetRenderStateHook<69>);  // D3DDevice_SetRenderState_Wrap9  // N&B sub_8264C8D0
BANJO_RENUT_HOOK(sub_82434FD8, SetRenderStateHook<70>);  // D3DDevice_SetRenderState_Wrap10  // N&B sub_8264C8F8
BANJO_RENUT_HOOK(sub_82435000, SetRenderStateHook<71>);  // D3DDevice_SetRenderState_Wrap11  // N&B sub_8264C920
BANJO_RENUT_HOOK(sub_82435028, SetRenderStateHook<72>);  // D3DDevice_SetRenderState_Wrap12  // N&B sub_8264C948
BANJO_RENUT_HOOK(sub_82435050, SetRenderStateHook<73>);  // D3DDevice_SetRenderState_Wrap13  // N&B sub_8264C970
BANJO_RENUT_HOOK(sub_82435078, SetRenderStateHook<74>);  // D3DDevice_SetRenderState_Wrap14  // N&B sub_8264C998
BANJO_RENUT_HOOK(sub_824350A0, SetRenderStateHook<75>);  // D3DDevice_SetRenderState_Wrap15  // N&B sub_8264C9C0
BANJO_RENUT_HOOK(sub_824351C0, SetRenderStateHook<76>);  // D3DDevice_SetRenderState_ViewportEnable  // N&B sub_82205A80
BANJO_RENUT_HOOK(sub_824352F0, SetRenderStateHook<77>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable  // N&B sub_8264CAF0
BANJO_RENUT_HOOK(sub_82435388, SetRenderStateHook<78>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable1  // N&B sub_8264CB88
BANJO_RENUT_HOOK(sub_82435420, SetRenderStateHook<79>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable2  // N&B sub_8264CC20
BANJO_RENUT_HOOK(sub_824354B8, SetRenderStateHook<80>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable3  // N&B sub_8264CCB8
BANJO_RENUT_HOOK(sub_824355F8, SetRenderStateHook<81>);  // D3DDevice_SetRenderState_HalfPixelOffset  // N&B sub_8264CDF8
BANJO_RENUT_HOOK(sub_82435630, SetRenderStateHook<82>);  // D3DDevice_SetRenderState_PrimitiveResetEnable  // N&B sub_8264CE30
BANJO_RENUT_HOOK(sub_82435660, SetRenderStateHook<83>);  // D3DDevice_SetRenderState_PrimitiveResetIndex  // N&B sub_8264CE60
BANJO_RENUT_HOOK(sub_82435688, SetRenderStateHook<84>);  // D3DDevice_SetRenderState_AlphaToMaskEnable  // N&B sub_8264CE88
BANJO_RENUT_HOOK(sub_824356B8, SetRenderStateHook<85>);  // D3DDevice_SetRenderState_AlphaToMaskOffsets  // N&B sub_8264CEB8
BANJO_RENUT_HOOK(sub_824356F0, SetRenderStateHook<86>);  // D3DDevice_SetRenderState_GuardBand_X  // N&B sub_8264CEF0
BANJO_RENUT_HOOK(sub_82435720, SetRenderStateHook<87>);  // D3DDevice_SetRenderState_GuardBand_Y  // N&B sub_8264CF20
BANJO_RENUT_HOOK(sub_82435750, SetRenderStateHook<88>);  // D3DDevice_SetRenderState_DiscardBand_X  // N&B sub_8264CF50
BANJO_RENUT_HOOK(sub_82435778, SetRenderStateHook<89>);  // D3DDevice_SetRenderState_DiscardBand_Y  // N&B sub_8264CF78
BANJO_RENUT_HOOK(sub_82435840, SetRenderStateHook<90>);  // D3DDevice_SetRenderState_HiStencilEnable  // N&B sub_8264CFE8
BANJO_RENUT_HOOK(sub_82435870, SetRenderStateHook<91>);  // D3DDevice_SetRenderState_HiStencilWriteEnable  // N&B sub_8264D018
BANJO_RENUT_HOOK(sub_824358A0, SetRenderStateHook<92>);  // D3DDevice_SetRenderState_HiStencilFunc  // N&B sub_8264D048
BANJO_RENUT_HOOK(sub_824358D0, SetRenderStateHook<93>);  // D3DDevice_SetRenderState_HiStencilRef  // N&B sub_8264D078
BANJO_RENUT_HOOK(sub_824358F0, SetRenderStateHook<94>);  // D3DDevice_SetRenderState_PresentInterval  // N&B sub_8264D098
BANJO_RENUT_HOOK(sub_82435900, SetRenderStateHook<95>);  // D3DDevice_SetRenderState_PresentImmediateThreshold  // N&B sub_8264D0A8
BANJO_RENUT_HOOK(sub_824357A8, SetRenderStateHook<96>);  // D3DDevice_SetRenderState_HiZEnable  // N&B sub_8264CFA8
BANJO_RENUT_HOOK(sub_824357D8, SetRenderStateHook<97>);  // D3DDevice_SetRenderState_HiZWriteEnable  // N&B sub_822899D0
BANJO_RENUT_HOOK(sub_82434568, SetRenderStateHook<98>);  // D3DDevice_SetRenderState_LastPixel  // N&B sub_8264C060
BANJO_RENUT_HOOK(sub_824345A0, SetRenderStateHook<99>);  // D3DDevice_SetRenderState_LineWidth  // N&B sub_8264C098
BANJO_RENUT_HOOK(sub_82434610, SetRenderStateHook<100>);  // D3DDevice_SetRenderState_Buffer2Frames  // N&B sub_8264C110

// Sampler states, by their index in N&B's table (off_82E50BD0).
BANJO_RENUT_HOOK(sub_82436098, SetSamplerStateHook<0>);  // D3DDevice_SetSamplerState_AddressU  // N&B sub_8227D2D0
BANJO_RENUT_HOOK(sub_824360E8, SetSamplerStateHook<1>);  // D3DDevice_SetSamplerState_AddressV  // N&B sub_8220C4E0
BANJO_RENUT_HOOK(sub_82436138, SetSamplerStateHook<2>);  // D3DDevice_SetSamplerState_AddressW  // N&B sub_8264D630
BANJO_RENUT_HOOK(sub_82436028, SetSamplerStateHook<3>);  // D3DDevice_SetSamplerState_BorderColor  // N&B sub_8264D590
BANJO_RENUT_HOOK(sub_82435AC8, SetSamplerStateHook<4>);  // D3DDevice_SetSamplerState_MagFilter  // N&B sub_8222F1B8
BANJO_RENUT_HOOK(sub_82435920, SetSamplerStateHook<5>);  // D3DDevice_SetSamplerState_MinFilter  // N&B sub_8222F280
BANJO_RENUT_HOOK(sub_82435C68, SetSamplerStateHook<6>);  // D3DDevice_SetSamplerState_MipFilter  // N&B sub_8228F2F0
BANJO_RENUT_HOOK(sub_82435E88, SetSamplerStateHook<7>);  // D3DDevice_SetSamplerState_MipMapLodBias  // N&B sub_8264D460
BANJO_RENUT_HOOK(sub_82435F28, SetSamplerStateHook<8>);  // D3DDevice_SetSamplerState_MaxMipLevel  // N&B sub_8222F590
BANJO_RENUT_HOOK(sub_82435D68, SetSamplerStateHook<9>);  // D3DDevice_SetSamplerState_MaxAnisotropy  // N&B sub_8264D340
BANJO_RENUT_HOOK(sub_82435BC8, SetSamplerStateHook<10>);  // D3DDevice_SetSamplerState_MagFilterZ  // N&B sub_8264D1E0
BANJO_RENUT_HOOK(sub_82435A20, SetSamplerStateHook<11>);  // D3DDevice_SetSamplerState_MinFilterZ  // N&B sub_8264D100
BANJO_RENUT_HOOK(sub_82435CC0, SetSamplerStateHook<12>);  // D3DDevice_SetSamplerState_SeparateZFilterEnable  // N&B sub_8264D298
BANJO_RENUT_HOOK(sub_82435FA8, SetSamplerStateHook<13>);  // D3DDevice_SetSamplerState_MinMipLevel  // N&B sub_8264D510
BANJO_RENUT_HOOK(sub_82436188, SetSamplerStateHook<14>);  // D3DDevice_SetSamplerState_TrilinearThreshold  // N&B sub_8264D680
BANJO_RENUT_HOOK(sub_82435DE0, SetSamplerStateHook<15>);  // D3DDevice_SetSamplerState_AnisotropyBias  // N&B sub_8264D3B8
BANJO_RENUT_HOOK(sub_824361E0, SetSamplerStateHook<16>);  // D3DDevice_SetSamplerState_HGradientExpBias  // N&B sub_8264D6D8
BANJO_RENUT_HOOK(sub_82436238, SetSamplerStateHook<17>);  // D3DDevice_SetSamplerState_VGradientExpBias  // N&B sub_8264D730
BANJO_RENUT_HOOK(sub_82436290, SetSamplerStateHook<18>);  // D3DDevice_SetSamplerState_WhiteBorderColorW  // N&B sub_8264D788
BANJO_RENUT_HOOK(sub_824362E8, SetSamplerStateHook<19>);  // D3DDevice_SetSamplerState_PointBorderEnable  // N&B sub_8264D7E0

BANJO_RENUT_HOOK(sub_82429170, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture  // N&B sub_8222F410
#if 0  // peggle2-nx / peggle1-nx: N&B sub_82B96538 (D3DDevice_SetTexture_Inline) is linked in neither Peggle 2 nor Peggle 1
BANJO_RENUT_HOOK(sub_82B96538, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture_Inline
#endif
BANJO_RENUT_HOOK(sub_824364D0, D3DDevice_SetStreamSource_hook);  // D3DDevice_SetStreamSource  // N&B sub_8222A408
BANJO_RENUT_HOOK(sub_82436678, D3DDevice_SetIndices_hook);  // D3DDevice_SetIndices  // N&B sub_8220EE38
BANJO_RENUT_HOOK(sub_8242CCD8, D3DDevice_SetVertexDeclaration_hook);  // D3DDevice_SetVertexDeclaration  // N&B sub_82205700
BANJO_RENUT_HOOK(sub_82437008, D3DDevice_SetViewport_hook);  // D3DDevice_SetViewport  // N&B sub_82222EA0
BANJO_RENUT_HOOK(sub_82436DA0, D3D_SetViewport_hook);  // D3D_SetViewport  // N&B sub_82222F20
BANJO_RENUT_HOOK(sub_82436348, D3DDevice_SetScissorRect_hook);  // D3DDevice_SetScissorRect  // N&B sub_82223178
BANJO_RENUT_HOOK(sub_82433BB0, D3D_SetSurfaceClip_hook);  // D3D_SetSurfaceClip  // N&B sub_82223270
BANJO_RENUT_HOOK(sub_82437138, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget  // N&B sub_8227AB68
BANJO_RENUT_HOOK(sub_82437480, D3DDevice_SetDepthStencilSurface_hook);  // D3DDevice_SetDepthStencilSurface  // N&B sub_8227AF40
BANJO_RENUT_HOOK(sub_824370A8, D3D_SetSurfaceInfo_hook);  // D3D_SetSurfaceInfo  // N&B sub_8227AEB0
BANJO_RENUT_HOOK(sub_82439910, D3DDevice_SetPredication_hook);  // D3DDevice_SetPredication  // N&B sub_8222AA18
BANJO_RENUT_HOOK(sub_82446398, D3DDevice_SetCommandBufferPredication_hook);  // D3DDevice_SetCommandBufferPredication  // N&B sub_82658678
BANJO_RENUT_HOOK(sub_8242D220, D3DDevice_SetShaderGPRAllocation_hook);  // D3DDevice_SetShaderGPRAllocation  // N&B sub_82220B20
BANJO_RENUT_HOOK(sub_8242BD68, D3D_CleanupGprAllocationSetup_hook);  // D3D_CleanupGprAllocationSetup  // N&B sub_8264E2C0
BANJO_RENUT_HOOK(sub_8244B258, D3DDevice_SetScreenExtentQueryMode_hook);  // D3DDevice_SetScreenExtentQueryMode  // N&B sub_82658988
BANJO_RENUT_HOOK(sub_82444060, D3DDevice_UnsetAll_hook);  // D3DDevice_UnsetAll  // N&B sub_8264E1A8
BANJO_RENUT_HOOK(sub_82436CA8, D3D_ResetAllState_hook);  // D3D_ResetAllState  // N&B sub_82287D38
BANJO_RENUT_HOOK(sub_82443FC8, D3D_InvalidateConstants_hook);  // D3D_InvalidateConstants  // N&B sub_8264E128
BANJO_RENUT_HOOK(sub_82440610, D3DDevice_BlockUntilIdle_hook);  // D3DDevice_BlockUntilIdle  // N&B sub_82657488
// Through a thunk (j_D3DDevice_SetRenderTarget 0x8264D8C8).
BANJO_RENUT_HOOK(sub_82437D68, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget_Thunk  // N&B sub_8264D8C8
BANJO_RENUT_HOOK(sub_82436748, D3DDevice_GetRenderTarget_hook);  // D3DDevice_GetRenderTarget  // N&B sub_8264D8D0
BANJO_RENUT_HOOK(sub_82436790, D3DDevice_GetDepthStencilSurface_hook);  // D3DDevice_GetDepthStencilSurface  // N&B sub_8264D910
BANJO_RENUT_HOOK(sub_824365F0, D3DDevice_GetStreamSource_hook);  // D3DDevice_GetStreamSource  // N&B sub_8264D840
BANJO_RENUT_HOOK(sub_82436468, D3DDevice_GetViewport_hook);  // D3DDevice_GetViewport  // N&B sub_82276AC0
BANJO_RENUT_HOOK(sub_82436A20, D3DDevice_SetClipPlane_hook);  // D3DDevice_SetClipPlane  // N&B sub_8264DAE8
BANJO_RENUT_HOOK(sub_82436B20, D3DDevice_SetVertexFetchConstant_hook);  // D3DDevice_SetVertexFetchConstant  // N&B sub_8264DB38
BANJO_RENUT_HOOK(sub_8242C450, D3DDevice_SetVertexShaderConstantB_hook);  // D3DDevice_SetVertexShaderConstantB  // N&B sub_82221930
BANJO_RENUT_HOOK(sub_8242C4B0, D3DDevice_SetPixelShaderConstantB_hook);  // D3DDevice_SetPixelShaderConstantB  // N&B sub_82268810
BANJO_RENUT_HOOK(sub_8242C5A0, D3DDevice_SetVertexShaderConstantI_hook);  // D3DDevice_SetVertexShaderConstantI  // N&B sub_8264E570
BANJO_RENUT_HOOK(sub_8242C5F8, D3DDevice_SetPixelShaderConstantI_hook);  // D3DDevice_SetPixelShaderConstantI  // N&B sub_8264E5C8
BANJO_RENUT_HOOK(sub_8243B808, D3DDevice_EndIndexedVertices_hook);  // D3DDevice_EndIndexedVertices  // N&B sub_826561F8

// Render state getters, by their index in N&B's table (off_82E506F0, field 0).
BANJO_RENUT_HOOK(sub_82434678, GetRenderStateHook<10>);  // D3DDevice_GetRenderState_ZEnable  // N&B sub_8264C178
BANJO_RENUT_HOOK(sub_824346D8, GetRenderStateHook<11>);  // D3DDevice_GetRenderState_ZFunc  // N&B sub_8264C1B0
BANJO_RENUT_HOOK(sub_824346A0, GetRenderStateHook<12>);  // D3DDevice_GetRenderState_ZWriteEnable  // N&B sub_8264C1A0
BANJO_RENUT_HOOK(sub_82433EF0, GetRenderStateHook<13>);  // D3DDevice_GetRenderState_FillMode  // N&B sub_8264BE88
BANJO_RENUT_HOOK(sub_82433EC0, GetRenderStateHook<14>);  // D3DDevice_GetRenderState_CullMode  // N&B sub_8264BE78
BANJO_RENUT_HOOK(sub_82434030, GetRenderStateHook<15>);  // D3DDevice_GetRenderState_AlphaBlendEnable  // N&B sub_8264BED0
BANJO_RENUT_HOOK(sub_824343C8, GetRenderStateHook<16>);  // D3DDevice_GetRenderState_SeparateAlphaBlendEnable  // N&B sub_8264BF40
BANJO_RENUT_HOOK(sub_824344F8, GetRenderStateHook<17>);  // D3DDevice_GetRenderState_BlendFactor  // N&B sub_8264BFE8
BANJO_RENUT_HOOK(sub_82434150, GetRenderStateHook<18>);  // D3DDevice_GetRenderState_SrcBlend  // N&B sub_8264BEF0
BANJO_RENUT_HOOK(sub_824341E0, GetRenderStateHook<19>);  // D3DDevice_GetRenderState_DestBlend  // N&B sub_8264BF00
BANJO_RENUT_HOOK(sub_824340C0, GetRenderStateHook<20>);  // D3DDevice_GetRenderState_BlendOp  // N&B sub_8264BEE0
BANJO_RENUT_HOOK(sub_824342C0, GetRenderStateHook<21>);  // D3DDevice_GetRenderState_SrcBlendAlpha  // N&B sub_8264BF20
BANJO_RENUT_HOOK(sub_82434330, GetRenderStateHook<22>);  // D3DDevice_GetRenderState_DestBlendAlpha  // N&B sub_8264BF30
BANJO_RENUT_HOOK(sub_82434250, GetRenderStateHook<23>);  // D3DDevice_GetRenderState_BlendOpAlpha  // N&B sub_8264BF10
BANJO_RENUT_HOOK(sub_82433F28, GetRenderStateHook<24>);  // D3DDevice_GetRenderState_AlphaTestEnable  // N&B sub_8264BEC0
BANJO_RENUT_HOOK(sub_82434410, GetRenderStateHook<25>);  // D3DDevice_GetRenderState_AlphaRef  // N&B sub_8264BF88
BANJO_RENUT_HOOK(sub_82434458, GetRenderStateHook<26>);  // D3DDevice_GetRenderState_AlphaFunc  // N&B sub_8264BFD8
BANJO_RENUT_HOOK(sub_82434720, GetRenderStateHook<27>);  // D3DDevice_GetRenderState_StencilEnable  // N&B sub_8264C1F8
BANJO_RENUT_HOOK(sub_82434750, GetRenderStateHook<28>);  // D3DDevice_GetRenderState_TwoSidedStencilMode  // N&B sub_8264C228
BANJO_RENUT_HOOK(sub_824347B8, GetRenderStateHook<29>);  // D3DDevice_GetRenderState_StencilFail  // N&B sub_8264C290
BANJO_RENUT_HOOK(sub_824347F0, GetRenderStateHook<30>);  // D3DDevice_GetRenderState_StencilZFail  // N&B sub_8264C2C8
BANJO_RENUT_HOOK(sub_82434820, GetRenderStateHook<31>);  // D3DDevice_GetRenderState_StencilPass  // N&B sub_8264C2F8
BANJO_RENUT_HOOK(sub_82434780, GetRenderStateHook<32>);  // D3DDevice_GetRenderState_StencilFunc  // N&B sub_8264C258
BANJO_RENUT_HOOK(sub_82434918, GetRenderStateHook<33>);  // D3DDevice_GetRenderState_StencilRef  // N&B sub_8264C3F0
BANJO_RENUT_HOOK(sub_82434938, GetRenderStateHook<34>);  // D3DDevice_GetRenderState_StencilMask  // N&B sub_8264C410
BANJO_RENUT_HOOK(sub_82434958, GetRenderStateHook<35>);  // D3DDevice_GetRenderState_StencilWriteMask  // N&B sub_8264C430
BANJO_RENUT_HOOK(sub_82434888, GetRenderStateHook<36>);  // D3DDevice_GetRenderState_CCWStencilFail  // N&B sub_8264C360
BANJO_RENUT_HOOK(sub_824348C0, GetRenderStateHook<37>);  // D3DDevice_GetRenderState_CCWStencilZFail  // N&B sub_8264C398
BANJO_RENUT_HOOK(sub_824348F0, GetRenderStateHook<38>);  // D3DDevice_GetRenderState_CCWStencilPass  // N&B sub_8264C3C8
BANJO_RENUT_HOOK(sub_82434850, GetRenderStateHook<39>);  // D3DDevice_GetRenderState_CCWStencilFunc  // N&B sub_8264C328
BANJO_RENUT_HOOK(sub_82434978, GetRenderStateHook<40>);  // D3DDevice_GetRenderState_CCWStencilRef  // N&B sub_8264C450
BANJO_RENUT_HOOK(sub_82434998, GetRenderStateHook<41>);  // D3DDevice_GetRenderState_CCWStencilMask  // N&B sub_8264C470
BANJO_RENUT_HOOK(sub_824349B8, GetRenderStateHook<42>);  // D3DDevice_GetRenderState_CCWStencilWriteMask  // N&B sub_8264C490
BANJO_RENUT_HOOK(sub_82434A00, GetRenderStateHook<43>);  // D3DDevice_GetRenderState_ClipPlaneEnable  // N&B sub_8264C498
BANJO_RENUT_HOOK(sub_82434D30, GetRenderStateHook<44>);  // D3DDevice_GetRenderState_PointSize  // N&B sub_8264C710
BANJO_RENUT_HOOK(sub_82434D80, GetRenderStateHook<45>);  // D3DDevice_GetRenderState_PointSizeMin  // N&B sub_8264C760
BANJO_RENUT_HOOK(sub_82434CE0, GetRenderStateHook<46>);  // D3DDevice_GetRenderState_PointSpriteEnable  // N&B sub_8264C6B8
BANJO_RENUT_HOOK(sub_82434DD0, GetRenderStateHook<47>);  // D3DDevice_GetRenderState_PointSizeMax  // N&B sub_8264C770
BANJO_RENUT_HOOK(sub_82434BA8, GetRenderStateHook<48>);  // D3DDevice_GetRenderState_MultiSampleAntiAlias  // N&B sub_8264C5B8
BANJO_RENUT_HOOK(sub_82434BD0, GetRenderStateHook<49>);  // D3DDevice_GetRenderState_MultiSampleMask  // N&B sub_8264C5E0
BANJO_RENUT_HOOK(sub_82434A10, GetRenderStateHook<50>);  // D3DDevice_GetRenderState_ScissorTestEnable  // N&B sub_8264C4B8
BANJO_RENUT_HOOK(sub_82434AC0, GetRenderStateHook<51>);  // D3DDevice_GetRenderState_SlopeScaleDepthBias  // N&B sub_8264C568
BANJO_RENUT_HOOK(sub_82434B78, GetRenderStateHook<52>);  // D3DDevice_GetRenderState_DepthBias  // N&B sub_8264C588
BANJO_RENUT_HOOK(sub_82434C10, GetRenderStateHook<53>);  // D3DDevice_GetRenderState_ColorWriteEnable  // N&B sub_8264C5E8
BANJO_RENUT_HOOK(sub_82434C50, GetRenderStateHook<54>);  // D3DDevice_GetRenderState_ColorWriteEnable1  // N&B sub_8264C628
BANJO_RENUT_HOOK(sub_82434C90, GetRenderStateHook<55>);  // D3DDevice_GetRenderState_ColorWriteEnable2  // N&B sub_8264C668
BANJO_RENUT_HOOK(sub_82434CD0, GetRenderStateHook<56>);  // D3DDevice_GetRenderState_ColorWriteEnable3  // N&B sub_8264C6A8
BANJO_RENUT_HOOK(sub_824355E8, GetRenderStateHook<57>);  // D3DDevice_GetRenderState_TessellationMode  // N&B sub_8264CDE8
BANJO_RENUT_HOOK(sub_82435578, GetRenderStateHook<58>);  // D3DDevice_GetRenderState_MinTessellationLevel  // N&B sub_8264CD78
BANJO_RENUT_HOOK(sub_824355B0, GetRenderStateHook<59>);  // D3DDevice_GetRenderState_MaxTessellationLevel  // N&B sub_8264CDB0
BANJO_RENUT_HOOK(sub_824350C0, GetRenderStateHook<60>);  // D3DDevice_GetRenderState_Wrap0  // N&B sub_8264C9E0
BANJO_RENUT_HOOK(sub_824350D0, GetRenderStateHook<61>);  // D3DDevice_GetRenderState_Wrap1  // N&B sub_8264C9F0
BANJO_RENUT_HOOK(sub_824350E0, GetRenderStateHook<62>);  // D3DDevice_GetRenderState_Wrap2  // N&B sub_8264CA00
BANJO_RENUT_HOOK(sub_824350F0, GetRenderStateHook<63>);  // D3DDevice_GetRenderState_Wrap3  // N&B sub_8264CA10
BANJO_RENUT_HOOK(sub_82435100, GetRenderStateHook<64>);  // D3DDevice_GetRenderState_Wrap4  // N&B sub_8264CA20
BANJO_RENUT_HOOK(sub_82435110, GetRenderStateHook<65>);  // D3DDevice_GetRenderState_Wrap5  // N&B sub_8264CA30
BANJO_RENUT_HOOK(sub_82435120, GetRenderStateHook<66>);  // D3DDevice_GetRenderState_Wrap6  // N&B sub_8264CA40
BANJO_RENUT_HOOK(sub_82435130, GetRenderStateHook<67>);  // D3DDevice_GetRenderState_Wrap7  // N&B sub_8264CA50
BANJO_RENUT_HOOK(sub_82435140, GetRenderStateHook<68>);  // D3DDevice_GetRenderState_Wrap8  // N&B sub_8264CA60
BANJO_RENUT_HOOK(sub_82435150, GetRenderStateHook<69>);  // D3DDevice_GetRenderState_Wrap9  // N&B sub_8264CA70
BANJO_RENUT_HOOK(sub_82435160, GetRenderStateHook<70>);  // D3DDevice_GetRenderState_Wrap10  // N&B sub_8264CA80
BANJO_RENUT_HOOK(sub_82435170, GetRenderStateHook<71>);  // D3DDevice_GetRenderState_Wrap11  // N&B sub_8264CA90
BANJO_RENUT_HOOK(sub_82435180, GetRenderStateHook<72>);  // D3DDevice_GetRenderState_Wrap12  // N&B sub_8264CAA0
BANJO_RENUT_HOOK(sub_82435190, GetRenderStateHook<73>);  // D3DDevice_GetRenderState_Wrap13  // N&B sub_8264CAB0
BANJO_RENUT_HOOK(sub_824351A0, GetRenderStateHook<74>);  // D3DDevice_GetRenderState_Wrap14  // N&B sub_8264CAC0
BANJO_RENUT_HOOK(sub_824351B0, GetRenderStateHook<75>);  // D3DDevice_GetRenderState_Wrap15  // N&B sub_8264CAD0
BANJO_RENUT_HOOK(sub_82435200, GetRenderStateHook<76>);  // D3DDevice_GetRenderState_ViewportEnable  // N&B sub_8264CAE0
BANJO_RENUT_HOOK(sub_82435380, GetRenderStateHook<77>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable  // N&B sub_8264CB80
BANJO_RENUT_HOOK(sub_82435418, GetRenderStateHook<78>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable1  // N&B sub_8264CC18
BANJO_RENUT_HOOK(sub_824354B0, GetRenderStateHook<79>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable2  // N&B sub_8264CCB0
BANJO_RENUT_HOOK(sub_82435548, GetRenderStateHook<80>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable3  // N&B sub_8264CD48
BANJO_RENUT_HOOK(sub_82435620, GetRenderStateHook<81>);  // D3DDevice_GetRenderState_HalfPixelOffset  // N&B sub_8264CE20
BANJO_RENUT_HOOK(sub_82435650, GetRenderStateHook<82>);  // D3DDevice_GetRenderState_PrimitiveResetEnable  // N&B sub_8264CE50
BANJO_RENUT_HOOK(sub_82435680, GetRenderStateHook<83>);  // D3DDevice_GetRenderState_PrimitiveResetIndex  // N&B sub_8264CE80
BANJO_RENUT_HOOK(sub_824356A8, GetRenderStateHook<84>);  // D3DDevice_GetRenderState_AlphaToMaskEnable  // N&B sub_8264CEA8
BANJO_RENUT_HOOK(sub_824356D8, GetRenderStateHook<85>);  // D3DDevice_GetRenderState_AlphaToMaskOffsets  // N&B sub_8264CED8
BANJO_RENUT_HOOK(sub_82435718, GetRenderStateHook<86>);  // D3DDevice_GetRenderState_GuardBand_X  // N&B sub_8264CF18
BANJO_RENUT_HOOK(sub_82435748, GetRenderStateHook<87>);  // D3DDevice_GetRenderState_GuardBand_Y  // N&B sub_8264CF48
BANJO_RENUT_HOOK(sub_82435770, GetRenderStateHook<88>);  // D3DDevice_GetRenderState_DiscardBand_X  // N&B sub_8264CF70
BANJO_RENUT_HOOK(sub_824357A0, GetRenderStateHook<89>);  // D3DDevice_GetRenderState_DiscardBand_Y  // N&B sub_8264CFA0
BANJO_RENUT_HOOK(sub_82435860, GetRenderStateHook<90>);  // D3DDevice_GetRenderState_HiStencilEnable  // N&B sub_8264D008
BANJO_RENUT_HOOK(sub_82435890, GetRenderStateHook<91>);  // D3DDevice_GetRenderState_HiStencilWriteEnable  // N&B sub_8264D038
BANJO_RENUT_HOOK(sub_824358C0, GetRenderStateHook<92>);  // D3DDevice_GetRenderState_HiStencilFunc  // N&B sub_8264D068
BANJO_RENUT_HOOK(sub_824358E8, GetRenderStateHook<93>);  // D3DDevice_GetRenderState_HiStencilRef  // N&B sub_8264D090
BANJO_RENUT_HOOK(sub_824358F8, GetRenderStateHook<94>);  // D3DDevice_GetRenderState_PresentInterval  // N&B sub_8264D0A0
BANJO_RENUT_HOOK(sub_82435910, GetRenderStateHook<95>);  // D3DDevice_GetRenderState_PresentImmediateThreshold  // N&B sub_8264D0B8
BANJO_RENUT_HOOK(sub_824357C8, GetRenderStateHook<96>);  // D3DDevice_GetRenderState_HiZEnable  // N&B sub_8264CFC8
BANJO_RENUT_HOOK(sub_82435830, GetRenderStateHook<97>);  // D3DDevice_GetRenderState_HiZWriteEnable  // N&B sub_8264CFD8
BANJO_RENUT_HOOK(sub_82434590, GetRenderStateHook<98>);  // D3DDevice_GetRenderState_LastPixel  // N&B sub_8264C088
BANJO_RENUT_HOOK(sub_824345E0, GetRenderStateHook<99>);  // D3DDevice_GetRenderState_LineWidth  // N&B sub_8264C0D8
BANJO_RENUT_HOOK(sub_82434630, GetRenderStateHook<100>);  // D3DDevice_GetRenderState_Buffer2Frames  // N&B sub_8264C130

// Sampler state getters (off_82E50BD0, field 0).
BANJO_RENUT_HOOK(sub_824360D0, GetSamplerStateHook<0>);  // D3DDevice_GetSamplerState_AddressU  // N&B sub_8264D600
BANJO_RENUT_HOOK(sub_82436120, GetSamplerStateHook<1>);  // D3DDevice_GetSamplerState_AddressV  // N&B sub_8264D618
BANJO_RENUT_HOOK(sub_82436170, GetSamplerStateHook<2>);  // D3DDevice_GetSamplerState_AddressW  // N&B sub_8264D668
BANJO_RENUT_HOOK(sub_82436078, GetSamplerStateHook<3>);  // D3DDevice_GetSamplerState_BorderColor  // N&B sub_8264D5E0
BANJO_RENUT_HOOK(sub_82435B90, GetSamplerStateHook<4>);  // D3DDevice_GetSamplerState_MagFilter  // N&B sub_8264D1A8
BANJO_RENUT_HOOK(sub_824359E8, GetSamplerStateHook<5>);  // D3DDevice_GetSamplerState_MinFilter  // N&B sub_8264D0C8
BANJO_RENUT_HOOK(sub_82435CA8, GetSamplerStateHook<6>);  // D3DDevice_GetSamplerState_MipFilter  // N&B sub_8264D280
BANJO_RENUT_HOOK(sub_82435EE8, GetSamplerStateHook<7>);  // D3DDevice_GetSamplerState_MipMapLodBias  // N&B sub_8264D4C0
BANJO_RENUT_HOOK(sub_82435F98, GetSamplerStateHook<8>);  // D3DDevice_GetSamplerState_MaxMipLevel  // N&B sub_8264D500
BANJO_RENUT_HOOK(sub_82435DD0, GetSamplerStateHook<9>);  // D3DDevice_GetSamplerState_MaxAnisotropy  // N&B sub_8264D3A8
BANJO_RENUT_HOOK(sub_82435C58, GetSamplerStateHook<10>);  // D3DDevice_GetSamplerState_MagFilterZ  // N&B sub_8264D270
BANJO_RENUT_HOOK(sub_82435AB8, GetSamplerStateHook<11>);  // D3DDevice_GetSamplerState_MinFilterZ  // N&B sub_8264D198
BANJO_RENUT_HOOK(sub_82435D58, GetSamplerStateHook<12>);  // D3DDevice_GetSamplerState_SeparateZFilterEnable  // N&B sub_8264D330
BANJO_RENUT_HOOK(sub_82436018, GetSamplerStateHook<13>);  // D3DDevice_GetSamplerState_MinMipLevel  // N&B sub_8264D580
BANJO_RENUT_HOOK(sub_824361C8, GetSamplerStateHook<14>);  // D3DDevice_GetSamplerState_TrilinearThreshold  // N&B sub_8264D6C0
BANJO_RENUT_HOOK(sub_82435E40, GetSamplerStateHook<15>);  // D3DDevice_GetSamplerState_AnisotropyBias  // N&B sub_8264D418
BANJO_RENUT_HOOK(sub_82436220, GetSamplerStateHook<16>);  // D3DDevice_GetSamplerState_HGradientExpBias  // N&B sub_8264D718
BANJO_RENUT_HOOK(sub_82436278, GetSamplerStateHook<17>);  // D3DDevice_GetSamplerState_VGradientExpBias  // N&B sub_8264D770
BANJO_RENUT_HOOK(sub_824362D0, GetSamplerStateHook<18>);  // D3DDevice_GetSamplerState_WhiteBorderColorW  // N&B sub_8264D7C8
BANJO_RENUT_HOOK(sub_82436330, GetSamplerStateHook<19>);  // D3DDevice_GetSamplerState_PointBorderEnable  // N&B sub_8264D828
