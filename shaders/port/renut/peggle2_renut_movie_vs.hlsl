// peggle2-nx (2026-10-20): Renut's vertex shader for the Bink movies, port-written: the vertex shader Peggle 2 compiles
// at boot with the movie pixel shaders (container XXH3 3caebedd6edf56e6), which no game file holds. No game shader code:
// what the shader has to do (as shaders/port/peggle2_movie_vs.hlsl for the native renderer), in Renut's translator's
// interface. tools/renut_port_shaders.py puts the translator's shader_common.h and vertex shader prologue in front.
//
// The movie quad's corners come as they are: POSITION0 is the clip-space position, TEXCOORD0.xy the frame's texture
// coordinate (TEXCOORD0.zw are 0). The rest (every interpolator declared and zeroed so the shader links with any pixel
// shader, the screen-space transform of pre-transformed vertices, no point size) is what the translator writes for
// every vertex shader.

#ifndef __spirv__
[shader("vertex")]
#endif
void main(
	[[vk::location(0)]] in float4 iPosition0 : POSITION0,
	[[vk::location(8)]] in float4 iTexCoord0 : TEXCOORD0,
	in uint iVertexId : SV_VertexID,
	in uint iInstanceId : SV_InstanceID,
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
	out float4 oNormal0 : NORMAL0,
	out float4 oNormal1 : NORMAL1,
	out float4 oNormal2 : NORMAL2,
	out float4 oTangent0 : TANGENT0,
	out float4 oBinormal0 : BINORMAL0,
	out float2 oPointCoord : POINTCOORD,
	out float oPointSize : POINTSIZE)
{
	oTexCoord1 = 0.0;
	oTexCoord2 = 0.0;
	oTexCoord3 = 0.0;
	oTexCoord4 = 0.0;
	oTexCoord5 = 0.0;
	oTexCoord6 = 0.0;
	oTexCoord7 = 0.0;
	oTexCoord8 = 0.0;
	oTexCoord9 = 0.0;
	oTexCoord10 = 0.0;
	oTexCoord11 = 0.0;
	oTexCoord12 = 0.0;
	oTexCoord13 = 0.0;
	oTexCoord14 = 0.0;
	oTexCoord15 = 0.0;
	oColor0 = 0.0;
	oColor1 = 0.0;
	oNormal0 = 0.0;
	oNormal1 = 0.0;
	oNormal2 = 0.0;
	oTangent0 = 0.0;
	oBinormal0 = 0.0;
	oPointCoord = 0.0;
	oPointSize = 0.0;

	oPos = iPosition0;
	oTexCoord0 = float4(iTexCoord0.xy, 0.0, 0.0);
	if (g_ScreenSpace != 0)
	{
		float2 pixel = oPos.xy * g_ScreenScale + g_ScreenOffset * oPos.w;
		oPos.xy = pixel * float2(2.0, -2.0) / g_ViewportSize + float2(-1.0, 1.0) * oPos.w;
	}
}
