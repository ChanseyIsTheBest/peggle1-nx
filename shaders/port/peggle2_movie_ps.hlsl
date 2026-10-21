// peggle2-nx's own pixel shaders for the Bink movies, for the two pixel shaders Peggle 2 compiles at boot
// (D3DXCompileShader in sub_8248D7C0): without alpha (container XXH3 23fb686cb21303f8: three planes) and with alpha
// (PEGGLE2_MOVIE_ALPHA, b79e9877de15d6db: four planes). No game shader code: only what the shaders have to do, in
// XenosRecomp's interface.
//
// What they do: a movie frame is three single-channel planes (luma Y, chroma Cr and Cb; with alpha a fourth, A), each
// a texture read in its alpha channel. The colour is an affine function of (Y, Cr, Cb) whose coefficients the game
// supplies per frame as pixel shader constants, then tinted (fades) by another constant:
//     rgb = Y * c4.rgb + Cr * c1.rgb + Cb * c2.rgb + c3.rgb        (c3: the offsets of the YCrCb -> RGB matrix)
//     alpha = 1 without the alpha plane, A with it
//     colour = (rgb, alpha) * c0
// Registers: textures s0 (Y), s1 (Cr), s2 (Cb), s3 (A); constants c0..c4 as above (the game sets them by register).
//
// Interface (what XenosRecomp generates for those containers): constants read from the pixel constant block (UBO or
// buffer pointer, MASSEFFECT_UBO), textures through the shared constants' descriptor indices, register N's at the
// same byte offsets as the translator's sN_* macros; the interpolator TEXCOORD0 carries the texture coordinate; every
// interpolator is declared so it links with the translated vertex shaders; the alpha test and blend-skip discards the
// renderer selects with specialization constants come last, as in every translated pixel shader.
//
// Build (SPIR-V only): dxc -spirv -T ps_6_6 -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout
//                      -Werror=parameter-usage -I shaders/XenosRecomp [-D PEGGLE2_MOVIE_ALPHA] peggle2_movie_ps.hlsl
#ifndef __spirv__
#error peggle2_movie_ps.hlsl is written for SPIR-V only
#endif
#include "shader_common.h"

// Pixel shader constant register N (16 bytes each).
#define MOVIE_CONSTANT(N) (MASSEFFECT_UBO ? g_UboPixel.v[N] \
	: vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + (N) * 16, 0x10))
// Texture register N: 2D descriptor index, sampler descriptor index, 1 / size (the shared constants' layout).
#define MOVIE_TEXTURE(N) (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT((N) * 4) \
	: vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + (N) * 4))
#define MOVIE_SAMPLER(N) (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(192 + (N) * 4) \
	: vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 192 + (N) * 4))
#define MOVIE_INV_SIZE(N) (MASSEFFECT_UBO \
	? float2(MASSEFFECT_SHARED_FLOAT(360 + (N) * 8), MASSEFFECT_SHARED_FLOAT(364 + (N) * 8)) \
	: vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 360 + (N) * 8))

// One plane of the frame: the alpha channel of texture register N.
#define MOVIE_PLANE(N, UV) tfetch2D(MOVIE_TEXTURE(N), MOVIE_SAMPLER(N), UV, float2(0, 0), MOVIE_INV_SIZE(N)).w

void main(
	in float4 iPos : SV_Position,
	in float4 iTexCoord0 : TEXCOORD0,
	in float4 iTexCoord1 : TEXCOORD1,
	in float4 iTexCoord2 : TEXCOORD2,
	in float4 iTexCoord3 : TEXCOORD3,
	in float4 iTexCoord4 : TEXCOORD4,
	in float4 iTexCoord5 : TEXCOORD5,
	in float4 iTexCoord6 : TEXCOORD6,
	in float4 iTexCoord7 : TEXCOORD7,
	in float4 iTexCoord8 : TEXCOORD8,
	in float4 iTexCoord9 : TEXCOORD9,
	in float4 iTexCoord10 : TEXCOORD10,
	in float4 iTexCoord11 : TEXCOORD11,
	in float4 iTexCoord12 : TEXCOORD12,
	in float4 iTexCoord13 : TEXCOORD13,
	in float4 iTexCoord14 : TEXCOORD14,
	in float4 iTexCoord15 : TEXCOORD15,
	in float4 iColor0 : COLOR0,
	in float4 iColor1 : COLOR1,
	in float4 iColor2 : COLOR2,
	in float4 iColor3 : COLOR3,
	in float4 iColor4 : COLOR4,
	in float4 iColor5 : COLOR5,
	in bool iFace : SV_IsFrontFace,
	out float4 oC0 : SV_Target0)
{
	const float2 uv = iTexCoord0.xy;
	const float luma = MOVIE_PLANE(0, uv);
	const float cr = MOVIE_PLANE(1, uv);
	const float cb = MOVIE_PLANE(2, uv);
#ifdef PEGGLE2_MOVIE_ALPHA
	const float alpha = MOVIE_PLANE(3, uv);
#else
	const float alpha = 1.0;
#endif
	float3 rgb = MOVIE_CONSTANT(3).rgb;          // offsets
	rgb += cb * MOVIE_CONSTANT(2).rgb;          // Cb column
	rgb += cr * MOVIE_CONSTANT(1).rgb;          // Cr column
	rgb += luma * MOVIE_CONSTANT(4).rgb;        // Y column
	oC0 = float4(rgb, alpha) * MOVIE_CONSTANT(0);  // tint / fade

	// The renderer's per-pipeline alpha test and blend skip (shaders/XenosRecomp/shader_common.h), as every
	// translated pixel shader ends.
	[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)
	{
		clip(alphaTestValue(oC0.w));
	}
	if ((g_SpecConstants() & SPEC_CONSTANT_BLEND_SKIP_ALPHA) != 0 && oC0.w <= 0.0) discard;
	if ((g_SpecConstants() & SPEC_CONSTANT_BLEND_SKIP_ZERO) != 0 && all(oC0 <= 0.0)) discard;
}
