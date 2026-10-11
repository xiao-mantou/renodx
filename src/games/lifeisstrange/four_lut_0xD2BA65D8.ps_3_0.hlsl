#include "./shared.h"
#include "./hdr_lut_bridge.hlsli"

// Experimental 4-LUT candidate translated from the extracted HlslDecompiler shader.
float4 AspectRatioAndInvAspectRatio : register(c13);
float BloomScreenBlendThreshold : register(c16);
float4 BloomTintAndScreenBlendThreshold : register(c8);
float4 BloomTintAndThreshold : register(c17);
sampler2D ColorGradingLUT : register(s7);
sampler2D DNEHexDofTexture1 : register(s5);
sampler2D DNEHexDofTexture2 : register(s6);
float4 DNEImageGrainParameter : register(c18);
sampler2D DNEImageGrainTexture : register(s3);
float4 DNEVignetColor : register(c19);
float4 DNEVignetMaskFactors : register(c20);
sampler2D DNEVignetTexture : register(s4);
float4 DepthDistances : register(c22);
float4 DepthTransition : register(c21);
float DistanceFade : register(c15);
sampler2D FilterColor1Texture : register(s1);
float4 HalfResMaskRect : register(c11);
float4 ImageAdjustments1 : register(c9);
float4 ImageAdjustments2 : register(c10);
float4 LightShaftParameters : register(c12);
sampler2D LightShaftsTexture : register(s2);
float4 MinMaxBlurClamp : register(c7);
float4 MinZ_MaxZRatio : register(c2);
float4 PackedParameters : register(c0);
sampler2D SceneColorTexture : register(s0);
float2 TextureSpaceBlurOrigin : register(c14);

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
	r0 = float4(1, 1, 0, 0) * i.texcoord1.xyxx;
	r0 = half4(tex2Dlod(SceneColorTexture, r0));
	r0.w = r0.w + -MinZ_MaxZRatio.y;
	r1.x = -r0.w + 1E-05;
	r0.w = 1 / r0.w;
	r0.w = (r1.x >= 0) ? 100000 : r0.w;
	r1.x = half(PackedParameters.x);
	r1.x = half(MinZ_MaxZRatio.x * r0.w + -r1.x);
	r0.w = half(r0.w * MinZ_MaxZRatio.x);
	r1.y = half(min(r0.w, 65504));
	r0.w = half(saturate(abs(r1.x) * PackedParameters.y));
	r1.z = half(pow(r0.w, PackedParameters.z));
	r2.xy = float2(1, 0);
	r0.w = half((r1.x >= 0) ? MinMaxBlurClamp.y : r2.x);
	r2.z = half(min(r1.z, r0.w));
	r0.w = half(max(PackedParameters.w, r2.z));
	r1.z = half((-r1.x >= 0) ? 0 : 1);
	r1.x = half((r1.x >= 0) ? -0 : -1);
	r1.x = half(r1.x + r1.z);
	r0.w = half(r0.w * r1.x);
	r0.w = saturate(r0.w * 10);
	r1.xz = max(i.texcoord1.zw, HalfResMaskRect.xy);
	r2.zw = min(HalfResMaskRect.zw, r1.xz);
	r3 = tex2D(DNEHexDofTexture2, r2.zw);
	r4 = tex2D(DNEHexDofTexture1, r2.zw);
	r1.x = 1 / r3.w;
	r3 = saturate(r1.x * r3.zzxy);
	r5 = half4(lerp(r0.zzxy, r3, r0.w));
	r0 = r4.zzxy + -r5.yyzw;
	r1.x = r4.w + -0.05;
	r1.x = r1.x * 3;
	r1.w = 5;
	r1.z = r1.w * MinMaxBlurClamp.x;
	r2.z = min(r1.z, 1);
	r1.x = saturate(r1.x * r2.z);
	r0 = half4(r1.x * r0 + r5);
	r1.x = half(dot(r0.zwy, 0.3));
	r1.x = half(r1.x * -3);
	r1.x = half(exp2(r1.x));
	r1.z = half(saturate(r1.x * BloomTintAndScreenBlendThreshold.w));
	r1.x = half(saturate(r1.x * BloomScreenBlendThreshold.x));
	r3 = tex2D(FilterColor1Texture, i.texcoord.zw);
	r3 = r3.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
	r3 = half4(r3 * 4);
	r0 = half4(r3 * r1.z + r0);
	r1.zw = AspectRatioAndInvAspectRatio.zw;
	r1.zw = i.texcoord.zw * -r1.zw + TextureSpaceBlurOrigin.xy;
	r1.z = dot(r1.zw, r1.zw) + 0;
	r1.z = rsqrt(r1.z);
	r1.z = 1 / r1.z;
	r1.z = saturate(r1.z * 0.5);
	r3.xy = float2(0.5, 1.5);
	r1.w = LightShaftParameters.w * -r3.x + r3.y;
	r3 = half4(tex2D(LightShaftsTexture, i.texcoord.zw));
	r2.z = half(r3.w * r3.w);
	r3 = half4(r3.zzxy * BloomTintAndThreshold.zzxy);
	r3 = r1.x * r3;
	r4.x = lerp(LightShaftParameters.w, r1.w, r2.z);
	r2.z = lerp(r4.x, 1, r1.z);
	r1.x = DistanceFade.x * DistanceFade.x;
	r1.x = r1.x * DistanceFade.x;
	r4.x = half(lerp(r2.z, 1, r1.x));
	r0 = half4(r0 * r4.x);
	r0 = half4(r3 * 4 + r0);
	float3 lis_hdr_untonemapped = float3(r0.z, r0.w, r0.y);
	float lis_hdr_scale = 1.f;
	r1.xzw = half3(r0.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
	r3.z = 1 / r1.x;
	r3.w = 1 / r1.z;
	r3.xy = 1 / r1.ww;
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
	r3.xyw = r0.xwz * float3(14.9999, 0.234375, 0.05859375);
	r0.x = frac(r3.x);
	r0.x = -r0.x + r3.x;
	r0.y = r0.y * 15 + -r0.x;
	r3.x = r0.x * 0.0625 + r3.w;
	r4 = r1.y + -DepthDistances;
	r4 = half4((r4 >= 0) ? 0 : 1);
	r5 = DepthDistances.xxyz * -r2.yxxx + r1.y;
	r0.xzw = half3(saturate(r1.yyy * DepthTransition.www + -DepthTransition.xyz));
	r1 = half4((r5 >= 0) ? r4 : 0);
	r1.w = half(dot(r1.yzw, 0.0625));
	r0.x = half(dot(r1.xyz, r0.xzw));
	r0.z = half(r1.w + 1);
	r1.x = r1.w * 0.25 + 0.0078125;
	r4.x = r0.z * 0.25 + 0.0078125;
	r4.yz = float2(0.001953125, 0.064453125);
	r4 = r3.xyxy + r4.yxzx;
	r5 = half4(tex2D(ColorGradingLUT, r4.xy));
	r4 = half4(tex2D(ColorGradingLUT, r4.zw));
	r6 = half4(lerp(r5, r4, r0.y));
	r1.yz = float2(0.001953125, 0.064453125);
	r1 = r3.xyxy + r1.yxzx;
	r3 = half4(tex2D(ColorGradingLUT, r1.xy));
	r1 = half4(tex2D(ColorGradingLUT, r1.zw));
	r4 = half4(lerp(r3, r1, r0.y));
	r1 = half4(lerp(r4, r6, r0.x));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  r1.xyz = LifeIsStrangeRestoreAfterColorGradingLut(r1.xyz, lis_hdr_scale);
	}
	r0 = tex2D(DNEVignetTexture, i.texcoord2.zw);
	r0.x = saturate(dot(r0, DNEVignetMaskFactors));
	r0.yzw = -r2.xxx + DNEVignetColor.xyz;
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
