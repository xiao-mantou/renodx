#include "./shared.h"
#include "./hdr_lut_bridge.hlsli"

// Experimental 4-LUT candidate translated from the extracted HlslDecompiler shader.
float4 AspectRatioAndInvAspectRatio : register(c12);
float BloomScreenBlendThreshold : register(c15);
float4 BloomTintAndScreenBlendThreshold : register(c8);
float4 BloomTintAndThreshold : register(c16);
sampler2D ColorGradingLUT : register(s4);
sampler2D DNEHexDofTexture2 : register(s3);
float4 DepthDistances : register(c18);
float4 DepthTransition : register(c17);
float DistanceFade : register(c14);
sampler2D FilterColor1Texture : register(s1);
float4 HalfResMaskRect : register(c10);
float4 ImageAdjustments2 : register(c9);
float4 LightShaftParameters : register(c11);
sampler2D LightShaftsTexture : register(s2);
float4 MinMaxBlurClamp : register(c7);
float4 MinZ_MaxZRatio : register(c2);
float4 PackedParameters : register(c0);
sampler2D SceneColorTexture : register(s0);
float2 TextureSpaceBlurOrigin : register(c13);

struct PS_IN
{
	float4 texcoord : TEXCOORD;
	float4 texcoord1 : TEXCOORD1;
};

half4 main(PS_IN i) : COLOR
{
	half4 o;

	float4 r0;
	float4 r1;
	float4 r2;
	float4 r3;
	float4 r4;
	float4 r5;
	r0.xy = max(i.texcoord1.zw, HalfResMaskRect.xy);
	r1.xy = min(HalfResMaskRect.zw, r0.xy);
	r0 = tex2D(DNEHexDofTexture2, r1.xy);
	r0.w = 1 / r0.w;
	r0 = saturate(r0.w * r0.zzxy);
	r1 = float4(1, 1, 0, 0) * i.texcoord1.xyxx;
	r1 = half4(tex2Dlod(SceneColorTexture, r1));
	r1.w = r1.w + -MinZ_MaxZRatio.y;
	r2.x = -r1.w + 1E-05;
	r1.w = 1 / r1.w;
	r1.w = (r2.x >= 0) ? 100000 : r1.w;
	r2.x = half(PackedParameters.x);
	r2.x = half(MinZ_MaxZRatio.x * r1.w + -r2.x);
	r1.w = half(r1.w * MinZ_MaxZRatio.x);
	r2.y = half(min(r1.w, 65504));
	r1.w = half(saturate(abs(r2.x) * PackedParameters.y));
	r2.z = half(pow(r1.w, PackedParameters.z));
	r3.xy = float2(1, 0);
	r1.w = half((r2.x >= 0) ? MinMaxBlurClamp.y : r3.x);
	r3.z = half(min(r2.z, r1.w));
	r1.w = half(max(PackedParameters.w, r3.z));
	r2.z = half((-r2.x >= 0) ? 0 : 1);
	r2.x = half((r2.x >= 0) ? -0 : -1);
	r2.x = half(r2.x + r2.z);
	r1.w = half(r1.w * r2.x);
	r1.w = saturate(r1.w * 10);
	r4 = half4(lerp(r1.zzxy, r0, r1.w));
	r0.x = half(dot(r4.zwy, 0.3));
	r0.x = half(r0.x * -3);
	r0.x = half(exp2(r0.x));
	r0.y = half(saturate(r0.x * BloomTintAndScreenBlendThreshold.w));
	r0.x = half(saturate(r0.x * BloomScreenBlendThreshold.x));
	r1 = tex2D(FilterColor1Texture, i.texcoord.zw);
	r1 = r1.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
	r1 = half4(r1 * 4);
	r1 = half4(r1 * r0.y + r4);
	r0.zw = AspectRatioAndInvAspectRatio.zw;
	r0.yz = i.texcoord.zw * -r0.zw + TextureSpaceBlurOrigin.xy;
	r0.y = dot(r0.yz, r0.yz) + 0;
	r0.y = rsqrt(r0.y);
	r0.y = 1 / r0.y;
	r0.y = saturate(r0.y * 0.5);
	r4.xy = float2(0.5, 1.5);
	r0.z = LightShaftParameters.w * -r4.x + r4.y;
	r4 = half4(tex2D(LightShaftsTexture, i.texcoord.zw));
	r0.w = half(r4.w * r4.w);
	r4 = half4(r4.zzxy * BloomTintAndThreshold.zzxy);
	r4 = r0.x * r4;
	r2.x = lerp(LightShaftParameters.w, r0.z, r0.w);
	r3.z = lerp(r2.x, 1, r0.y);
	r0.x = DistanceFade.x * DistanceFade.x;
	r0.x = r0.x * DistanceFade.x;
	r2.x = half(lerp(r3.z, 1, r0.x));
	r0 = half4(r1 * r2.x);
	r0 = half4(r4 * 4 + r0);
	float3 lis_hdr_untonemapped = float3(r0.z, r0.w, r0.y);
	float lis_hdr_scale = 1.f;
	r1.xyz = half3(r0.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
	r4.z = 1 / r1.x;
	r4.w = 1 / r1.y;
	r4.xy = 1 / r1.zz;
	r0 = half4(saturate(r0 * r4));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  float3 lis_hdr_extended = LifeIsStrangeExtendImageAdjustments2(
	      lis_hdr_untonemapped, ImageAdjustments2.xy, LIFEISSTRANGE_HDR_CURVE_PIVOT, LIFEISSTRANGE_HDR_CURVE_METHOD);
	  float3 lis_hdr_lut_proxy = LifeIsStrangeCompressForColorGradingLut(lis_hdr_extended, lis_hdr_scale);
	  r0.z = lis_hdr_lut_proxy.r;
	  r0.w = lis_hdr_lut_proxy.g;
	  r0.x = lis_hdr_lut_proxy.b;
	  r0.y = lis_hdr_lut_proxy.b;
	}
	r1.xyw = r0.xwz * float3(14.9999, 0.234375, 0.05859375);
	r0.x = frac(r1.x);
	r0.x = -r0.x + r1.x;
	r0.y = r0.y * 15 + -r0.x;
	r1.x = r0.x * 0.0625 + r1.w;
	r4 = r2.y + -DepthDistances;
	r4 = half4((r4 >= 0) ? 0 : 1);
	r3 = DepthDistances.xxyz * -r3.yxxx + r2.y;
	r0.xzw = half3(saturate(r2.yyy * DepthTransition.www + -DepthTransition.xyz));
	r2 = half4((r3 >= 0) ? r4 : 0);
	r1.z = half(dot(r2.yzw, 3));
	r0.x = half(dot(r2.xyz, r0.xzw));
	r0.z = half(r1.z + 1);
	r2.x = r1.z * 0.25 + 0.0078125;
	r3.x = r0.z * 0.25 + 0.0078125;
	r3.yz = float2(0.001953125, 0.064453125);
	r3 = r1.xyxy + r3.yxzx;
	r4 = half4(tex2D(ColorGradingLUT, r3.xy));
	r3 = half4(tex2D(ColorGradingLUT, r3.zw));
	r5 = half4(lerp(r4, r3, r0.y));
	r2.yz = float2(0.001953125, 0.064453125);
	r1 = r1.xyxy + r2.yxzx;
	r2 = half4(tex2D(ColorGradingLUT, r1.xy));
	r1 = half4(tex2D(ColorGradingLUT, r1.zw));
	r3 = half4(lerp(r2, r1, r0.y));
	r1 = half4(-r3 + r5);
	o = half4(r0.x * r1 + r3);
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  o.xyz = LifeIsStrangeRestoreAfterColorGradingLut(o.xyz, lis_hdr_scale);
	}

	return o;
}
