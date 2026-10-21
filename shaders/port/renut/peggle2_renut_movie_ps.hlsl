// peggle2-nx (2026-10-20): Renut's pixel shaders for the Bink movies, port-written: the two pixel shaders Peggle 2
// compiles at boot (D3DXCompileShader in sub_8248D7C0) and no game file holds, so neither the web installer nor any
// other tool can translate them from the player's files. Without alpha (container XXH3 23fb686cb21303f8: three planes)
// and with alpha (RENUT_MOVIE_ALPHA, b79e9877de15d6db: four planes). No game shader code: what the shaders have to do
// (as shaders/port/peggle2_movie_ps.hlsl for the native renderer), in Renut's translator's interface.
//
// tools/renut_port_shaders.py puts the translator's shader_common.h and its pixel shader prologue (the constant and
// descriptor-index macros, by their pattern) in front of this file and writes the result where the translator takes
// a shader's HLSL from (BANJO_HLSL_OVERRIDE): the signature and the epilogue (alpha test, blend skip, output trim)
// are the ones the translator writes for every pixel shader.
//
// A movie frame is three single-channel planes (luma Y, chroma Cr and Cb; with alpha a fourth, A), each a texture read
// in its alpha channel at TEXCOORD0. The colour is an affine function of (Y, Cr, Cb) whose coefficients the game sets
// per frame as constants, then tinted (fades) by another:
//     rgb = Y * c4.rgb + Cr * c1.rgb + Cb * c2.rgb + c3.rgb
//     colour = (rgb, A or 1) * c0
// Each channel is evaluated as Y * c4 + (Cr * c1 + (Cb * c2 + c3)), the order (and so the rounding) of the shader the
// game compiles. Registers: textures s0 Y, s1 Cr, s2 Cb, s3 A; constants c0 to c4.

#ifndef __spirv__
[shader("pixel")]
#endif
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
	in float4 iNormal0 : NORMAL0,
	in float4 iNormal1 : NORMAL1,
	in float4 iNormal2 : NORMAL2,
	in float4 iTangent0 : TANGENT0,
	in float4 iBinormal0 : BINORMAL0,
	in float2 iPointCoord : POINTCOORD,
#ifdef __spirv__
	in bool iFace : SV_IsFrontFace
#else
	in uint iFace : SV_IsFrontFace
#endif
,
	out float4 oC0 : SV_Target0)
{
	const float2 uv = iTexCoord0.xy;
	const float luma = tfetch2D(s0_Texture2DDescriptorIndex, s0_SamplerDescriptorIndex, uv, float2(0, 0)).w;
	const float red_chroma = tfetch2D(s1_Texture2DDescriptorIndex, s1_SamplerDescriptorIndex, uv, float2(0, 0)).w;
	const float blue_chroma = tfetch2D(s2_Texture2DDescriptorIndex, s2_SamplerDescriptorIndex, uv, float2(0, 0)).w;
	const float3 rgb = luma * g_C(4).xyz + (red_chroma * g_C(1).xyz + (blue_chroma * g_C(2).xyz + g_C(3).xyz));
#ifdef RENUT_MOVIE_ALPHA
	const float alpha = tfetch2D(s3_Texture2DDescriptorIndex, s3_SamplerDescriptorIndex, uv, float2(0, 0)).w;
	oC0 = float4(rgb, alpha) * g_C(0);
#else
	oC0 = float4(rgb * g_C(0).xyz, g_C(0).w);
#endif
	alphaTest(oC0.w, g_AlphaTestFunc, g_AlphaThreshold);
	banjoBlendSkip(oC0);
	oC0 = banjoTrim(oC0, 0u);
}
