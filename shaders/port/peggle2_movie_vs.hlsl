// peggle2-nx's own vertex shader for the quads Peggle 2 draws with the vertex shader it compiles at boot
// (D3DXCompileShader in sub_8248D7C0; container XXH3 3caebedd6edf56e6, and the same container with Direct3D's fetch
// patch for its 20-byte vertices, 3df8254d4b8024c4 being its loaded microcode). That shader passes a position and one
// texture coordinate through; this one does the same, written for the port, so the shader library does not need the
// game's run-time compiler. No game shader code: only what the shader has to do, in XenosRecomp's interface.
//
// Interface (what XenosRecomp generates for that container, which has an empty constant table, so everything is linked
// by register): the fetch into r1 (the position) arrives at location 5 as TEXCOORD1, the fetch into r0 (the texture
// coordinate) at location 4 as TEXCOORD0; the texture coordinate leaves as TEXCOORD0; every other output is written
// with XenosRecomp's default (0, 0, 0, 1) so any pixel shader links. Inputs go through tfetchTexcoord and remapInput
// (the renderer's vertex format and swizzle fix-ups), the position through the guest viewport transform, as in every
// translated vertex shader (shaders/XenosRecomp/shader_common.h).
//
// Build (SPIR-V only): dxc -spirv -T vs_6_6 -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout
//                      -Werror=parameter-usage -fvk-invert-y -I shaders/XenosRecomp peggle2_movie_vs.hlsl
#ifndef __spirv__
#error peggle2_movie_vs.hlsl is written for SPIR-V only
#endif
#define Sample(s, c) SampleLevel(s, c, 0)
#include "shader_common.h"

void main(
	[[vk::location(5)]] in float4 iTexCoord1 : TEXCOORD1,
	[[vk::location(4)]] in float4 iTexCoord0 : TEXCOORD0,
	in uint iVertexId : SV_VertexID,
	out float4 oPos : SV_Position,
	out float4 oTexCoord0 : TEXCOORD0,
	out float4 oTexCoord1 : TEXCOORD1,
	out float4 oTexCoord2 : TEXCOORD2,
	out float4 oTexCoord3 : TEXCOORD3,
	out float4 oTexCoord4 : TEXCOORD4,
	out float4 oTexCoord5 : TEXCOORD5,
	out float4 oTexCoord6 : TEXCOORD6,
	out float4 oTexCoord7 : TEXCOORD7,
	out float4 oTexCoord8 : TEXCOORD8,
	out float4 oTexCoord9 : TEXCOORD9,
	out float4 oTexCoord10 : TEXCOORD10,
	out float4 oTexCoord11 : TEXCOORD11,
	out float4 oTexCoord12 : TEXCOORD12,
	out float4 oTexCoord13 : TEXCOORD13,
	out float4 oTexCoord14 : TEXCOORD14,
	out float4 oTexCoord15 : TEXCOORD15,
	out float4 oColor0 : COLOR0,
	out float4 oColor1 : COLOR1,
	out float4 oColor2 : COLOR2,
	out float4 oColor3 : COLOR3,
	out float4 oColor4 : COLOR4,
	out float4 oColor5 : COLOR5)
{
	const float4 unwritten = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord1 = unwritten; oTexCoord2 = unwritten; oTexCoord3 = unwritten; oTexCoord4 = unwritten;
	oTexCoord5 = unwritten; oTexCoord6 = unwritten; oTexCoord7 = unwritten; oTexCoord8 = unwritten;
	oTexCoord9 = unwritten; oTexCoord10 = unwritten; oTexCoord11 = unwritten; oTexCoord12 = unwritten;
	oTexCoord13 = unwritten; oTexCoord14 = unwritten; oTexCoord15 = unwritten;
	oColor0 = unwritten; oColor1 = unwritten; oColor2 = unwritten; oColor3 = unwritten; oColor4 = unwritten;
	oColor5 = unwritten;

	// The vertex as the game's vertex buffer has it (the renderer turns the fetch format into the attribute format;
	// a float3 position reads w = 1).
	const float4 position = remapInput(float4(tfetchTexcoord(g_SwappedTexcoords, iTexCoord1, 1)), g_InputRemap(5));
	const float4 uv = remapInput(float4(tfetchTexcoord(g_SwappedTexcoords, iTexCoord0, 0)), g_InputRemap(4));

	oTexCoord0 = float4(uv.xy, 0.0, 1.0);
	// Clip space as the guest produced it, then the guest viewport transform the renderer passes in.
	// (the same two steps, in the same order, as every translated vertex shader, so positions round the same way)
	oPos = position;
	oPos.xy = oPos.xy * g_NdcScale + g_NdcOffset * oPos.w;
	oPos.xy += g_HalfPixelOffset * oPos.w;
}
