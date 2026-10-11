#include "./shared.h"
#include "./hdr_lut_bridge.hlsli"

// Experimental 4-LUT candidate translated from the extracted HlslDecompiler shader.
float4 AspectRatioAndInvAspectRatio : register(c11);
float BloomScreenBlendThreshold : register(c14);
float4 BloomTintAndScreenBlendThreshold : register(c0);
float4 BloomTintAndThreshold : register(c15);
sampler2D ColorGradingLUT : register(s5);
float4 DNEImageGrainParameter : register(c16);
sampler2D DNEImageGrainTexture : register(s3);
float4 DNEVignetColor : register(c17);
float4 DNEVignetMaskFactors : register(c18);
sampler2D DNEVignetTexture : register(s4);
float4 DepthDistances : register(c20);
float4 DepthTransition : register(c19);
float DistanceFade : register(c13);
sampler2D FilterColor1Texture : register(s1);
float4 HalfResMaskRect : register(c9);
float4 ImageAdjustments1 : register(c7);
float4 ImageAdjustments2 : register(c8);
float4 LightShaftParameters : register(c10);
sampler2D LightShaftsTexture : register(s2);
sampler2D LowResPostProcessBuffer : register(s6);
float4 MinZ_MaxZRatio : register(c2);
sampler2D SceneColorTexture : register(s0);
float2 TextureSpaceBlurOrigin : register(c12);

struct PS_IN
{
	float4 texcoord : TEXCOORD;
	float4 texcoord1 : TEXCOORD1;
	float4 texcoord2 : TEXCOORD2;
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
	float4 r6;
	r0.zw = AspectRatioAndInvAspectRatio.zw;
	r0.xy = i.texcoord.zw * -r0.zw + TextureSpaceBlurOrigin.xy;
	r0.x = dot(r0.xy, r0.xy) + 0;
	r0.x = rsqrt(r0.x);
	r0.x = 1 / r0.x;
	r0.x = saturate(r0.x * 0.5);
	r0.yz = float2(0.5, 1.5);
	r0.y = LightShaftParameters.w * -r0.y + r0.z;
	r1 = half4(tex2D(LightShaftsTexture, i.texcoord.zw));
	r0.z = half(r1.w * r1.w);
	r1 = half4(r1.zzxy * BloomTintAndThreshold.zzxy);
	r2.x = lerp(LightShaftParameters.w, r0.y, r0.z);
	r3.x = lerp(r2.x, 1, r0.x);
	r0.x = DistanceFade.x * DistanceFade.x;
	r0.x = r0.x * DistanceFade.x;
	r2.x = half(lerp(r3.x, 1, r0.x));
	r0 = tex2D(FilterColor1Texture, i.texcoord.zw);
	r0 = r0.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
	r0 = half4(r0 * 4);
	r3 = float4(1, 1, 0, 0) * i.texcoord1.xyxx;
	r3 = half4(tex2Dlod(SceneColorTexture, r3));
	r2.yz = max(i.texcoord1.zw, HalfResMaskRect.xy);
	r4.xy = min(HalfResMaskRect.zw, r2.yz);
	r4 = half4(tex2D(LowResPostProcessBuffer, r4.xy));
	r5 = r4.zzxy * -4 + r3.zzxy;
	r2.y = r3.w + -MinZ_MaxZRatio.y;
	r3 = r4.zzxy * 4;
	r3 = half4(r4.w * r5 + r3);
	r2.z = half(dot(r3.zwy, 0.11));
	r2.z = half(r2.z * -3);
	r2.z = half(exp2(r2.z));
	r2.w = half(saturate(r2.z * BloomTintAndScreenBlendThreshold.w));
	r2.z = half(saturate(r2.z * BloomScreenBlendThreshold.x));
	r1 = r1 * r2.z;
	r0 = half4(r0 * r2.w + r3);
	r0 = half4(r0 * r2.x);
	r0 = half4(r1 * 4 + r0);
	float3 lis_hdr_untonemapped = float3(r0.z, r0.w, r0.y);
	float lis_hdr_scale = 1.f;
	r1.xyz = half3(r0.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
	r3.z = 1 / r1.x;
	r3.w = 1 / r1.y;
	r3.xy = 1 / r1.zz;
	r0 = half4(saturate(r0 * r3));
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
	r0.x = -r2.y + 1E-05;
	r0.z = 1 / r2.y;
	r0.x = (r0.x >= 0) ? 100000 : r0.z;
	r0.x = half(r0.x * MinZ_MaxZRatio.x);
	r1.z = half(min(r0.x, 65504));
	r2 = r1.z + -DepthDistances;
	r2 = half4((r2 >= 0) ? 0 : 1);
	r3.xy = float2(1, 0);
	r4 = DepthDistances.xxyz * -r3.yxxx + r1.z;
	r0.xzw = half3(saturate(r1.zzz * DepthTransition.www + -DepthTransition.xyz));
	r2 = half4((r4 >= 0) ? r2 : 0);
	r1.z = half(dot(r2.yzw, 3));
	r0.x = half(dot(r2.xyz, r0.xzw));
	r0.z = half(r1.z + 1);
	r2.x = r1.z * 0.25 + 0.0078125;
	r4.x = r0.z * 0.25 + 0.0078125;
	r4.yz = float2(0.001953125, 0.064453125);
	r4 = r1.xyxy + r4.yxzx;
	r5 = half4(tex2D(ColorGradingLUT, r4.xy));
	r4 = half4(tex2D(ColorGradingLUT, r4.zw));
	r6 = half4(lerp(r5, r4, r0.y));
	r2.yz = float2(0.001953125, 0.064453125);
	r1 = r1.xyxy + r2.yxzx;
	r2 = half4(tex2D(ColorGradingLUT, r1.xy));
	r1 = half4(tex2D(ColorGradingLUT, r1.zw));
	r4 = half4(lerp(r2, r1, r0.y));
	r1 = half4(lerp(r4, r6, r0.x));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  r1.xyz = LifeIsStrangeRestoreAfterColorGradingLut(r1.xyz, lis_hdr_scale);
	}
	r0 = tex2D(DNEVignetTexture, i.texcoord2.zw);
	r0.x = saturate(dot(r0, DNEVignetMaskFactors));
	r0.yzw = -r3.xxx + DNEVignetColor.xyz;
	r0.xyz = r0.xxx * r0.yzw + 1;
	r2.x = 4;
	r2.xy = half2(i.texcoord2.zw * r2.xx + DNEImageGrainParameter.xy);
	r2 = half4(tex2D(DNEImageGrainTexture, r2.xy));
	r0.w = r2.x * 2 + -1;
	r0.w = r0.w * ImageAdjustments1.w;
	o.xyz = half3(saturate(r1.xyz * r0.xyz + r0.www));
	o.w = half(r1.w);

	return o;
}
