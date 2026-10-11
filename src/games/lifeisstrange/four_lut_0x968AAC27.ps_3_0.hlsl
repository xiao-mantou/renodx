#include "./shared.h"
#include "./hdr_lut_bridge.hlsli"

// Experimental 4-LUT candidate translated from the extracted HlslDecompiler shader.
float4 BloomTintAndScreenBlendThreshold : register(c0);
sampler2D ColorGradingLUT : register(s4);
float2 DNEColorStrokesBackBlendDistances : register(c16);
float4 DNEColorStrokesLightSrcColor0 : register(c12);
float4 DNEColorStrokesLightSrcColor1 : register(c15);
float3 DNEColorStrokesLightSrcPos0 : register(c10);
float3 DNEColorStrokesLightSrcPos1 : register(c13);
float2 DNEColorStrokesLightSrcSize0 : register(c11);
float2 DNEColorStrokesLightSrcSize1 : register(c14);
float4 DNEImageGrainParameter : register(c17);
sampler2D DNEImageGrainTexture : register(s2);
float4 DNEVignetColor : register(c18);
float4 DNEVignetMaskFactors : register(c19);
sampler2D DNEVignetTexture : register(s3);
float4 DepthDistances : register(c21);
float4 DepthTransition : register(c20);
sampler2D FilterColor1Texture : register(s1);
float4 HalfResMaskRect : register(c9);
float4 ImageAdjustments1 : register(c7);
float4 ImageAdjustments2 : register(c8);
sampler2D LowResPostProcessBuffer : register(s5);
float4 MinZ_MaxZRatio : register(c2);
sampler2D SceneColorTexture : register(s0);

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
	r0 = -0.5 + i.texcoord1.xyxy;
	r1.xy = half2(-DNEColorStrokesLightSrcPos0.xy);
	r1.zw = half2(-DNEColorStrokesLightSrcPos1.xy);
	r0 = half4(r0 * 2 + r1);
	r1.x = 1 / DNEColorStrokesLightSrcSize0.x;
	r1.y = 1 / DNEColorStrokesLightSrcSize0.y;
	r1.z = 1 / DNEColorStrokesLightSrcSize1.x;
	r1.w = 1 / DNEColorStrokesLightSrcSize1.y;
	r0 = half4(r0 * r1);
	r0.x = half(dot(r0.xy, r0.xy) + 0);
	r0.y = half(dot(r0.zw, r0.zw) + 0);
	r0.xy = half2(saturate(r0.xy * 4));
	r0.xy = half2(-r0.xy + 1);
	r0.xy = half2(r0.xy * r0.xy);
	r1 = float4(1, 1, 0, 0) * i.texcoord1.xyxx;
	r1 = half4(tex2Dlod(SceneColorTexture, r1));
	r0.z = r1.w + -MinZ_MaxZRatio.y;
	r0.w = -r0.z + 1E-05;
	r0.z = 1 / r0.z;
	r0.z = (r0.w >= 0) ? 100000 : r0.z;
	r2.x = MinZ_MaxZRatio.x;
	r0.w = half(r2.x * r0.z + -DNEColorStrokesBackBlendDistances.x);
	r0.z = half(r0.z * MinZ_MaxZRatio.x);
	r1.w = half(min(r0.z, 65504));
	r0.z = 1 / DNEColorStrokesBackBlendDistances.y;
	r0.z = half(saturate(r0.z * r0.w));
	r0.z = r0.z * r0.z + -1;
	r2.x = half(saturate(DNEColorStrokesLightSrcPos0.z));
	r2.y = half(saturate(DNEColorStrokesLightSrcPos1.z));
	r0.zw = r2.xy * r0.zz + 1;
	r0.xy = half2(r0.zw * r0.xy);
	r2 = half4(r0.y * DNEColorStrokesLightSrcColor1);
	r0 = half4(DNEColorStrokesLightSrcColor0 * r0.x + r2);
	r2 = half4(r0.w + r1.zzxy);
	r0 = r0.zzxy + 1;
	r1.xy = max(i.texcoord1.zw, HalfResMaskRect.xy);
	r3.xy = min(HalfResMaskRect.zw, r1.xy);
	r3 = half4(tex2D(LowResPostProcessBuffer, r3.xy));
	r4 = r3.zzxy * 4;
	r0 = r2 * r0 + -r4.yyzw;
	r0 = half4(r3.w * r0 + r4);
	r1.x = half(dot(r0.zwy, 0.3));
	r1.x = half(r1.x * -3);
	r1.x = half(exp2(r1.x));
	r1.x = half(saturate(r1.x * BloomTintAndScreenBlendThreshold.w));
	r2 = tex2D(FilterColor1Texture, i.texcoord.zw);
	r2 = r2.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
	r2 = half4(r2 * 4);
	r0 = half4(r2 * r1.x + r0);
	float3 lis_hdr_untonemapped = float3(r0.z, r0.w, r0.y);
	float lis_hdr_scale = 1.f;
	r1.xyz = half3(r0.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
	r2.z = 1 / r1.x;
	r2.w = 1 / r1.y;
	r2.xy = 1 / r1.zz;
	r0 = half4(saturate(r0 * r2));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  float3 lis_hdr_extended = LifeIsStrangeExtendImageAdjustments2(
	      lis_hdr_untonemapped, ImageAdjustments2.xy, LIFEISSTRANGE_HDR_CURVE_PIVOT, LIFEISSTRANGE_HDR_CURVE_METHOD);
	  float3 lis_hdr_lut_proxy = LifeIsStrangeCompressForColorGradingLut(lis_hdr_extended, lis_hdr_scale);
	  r0.z = lis_hdr_lut_proxy.r;
	  r0.w = lis_hdr_lut_proxy.g;
	  r0.x = lis_hdr_lut_proxy.b;
	  r0.y = lis_hdr_lut_proxy.b;
	}
	r2.xyw = r0.xwz * float3(14.9999, 0.234375, 0.05859375);
	r0.x = frac(r2.x);
	r0.x = -r0.x + r2.x;
	r0.y = r0.y * 15 + -r0.x;
	r2.x = r0.x * 0.0625 + r2.w;
	r3 = r1.w + -DepthDistances;
	r3 = half4((r3 >= 0) ? 0 : 1);
	r1.xy = float2(1, 0);
	r4 = DepthDistances.xxyz * -r1.yxxx + r1.w;
	r0.xzw = half3(saturate(r1.www * DepthTransition.www + -DepthTransition.xyz));
	r3 = half4((r4 >= 0) ? r3 : 0);
	r1.y = half(dot(r3.yzw, 2));
	r0.x = half(dot(r3.xyz, r0.xzw));
	r0.z = half(r1.y + 1);
	r3.x = r1.y * 0.25 + 0.0078125;
	r4.x = r0.z * 0.25 + 0.0078125;
	r4.yz = float2(0.001953125, 0.064453125);
	r4 = r2.xyxy + r4.yxzx;
	r5 = half4(tex2D(ColorGradingLUT, r4.xy));
	r4 = half4(tex2D(ColorGradingLUT, r4.zw));
	r6 = half4(lerp(r5, r4, r0.y));
	r3.yz = float2(0.001953125, 0.064453125);
	r2 = r2.xyxy + r3.yxzx;
	r3 = half4(tex2D(ColorGradingLUT, r2.xy));
	r2 = half4(tex2D(ColorGradingLUT, r2.zw));
	r4 = half4(lerp(r3, r2, r0.y));
	r2 = half4(lerp(r4, r6, r0.x));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  r2.xyz = LifeIsStrangeRestoreAfterColorGradingLut(r2.xyz, lis_hdr_scale);
	}
	r0 = tex2D(DNEVignetTexture, i.texcoord2.zw);
	r0.x = saturate(dot(r0, DNEVignetMaskFactors));
	r0.yzw = -r1.xxx + DNEVignetColor.xyz;
	r0.xyz = r0.xxx * r0.yzw + 1;
	r1.z = 4;
	r1.xy = half2(i.texcoord2.zw * r1.zz + DNEImageGrainParameter.xy);
	r1 = half4(tex2D(DNEImageGrainTexture, r1.xy));
	r0.w = r1.x * 2 + -1;
	r0.w = r0.w * ImageAdjustments1.w;
	o.xyz = half3(saturate(r2.xyz * r0.xyz + r0.www));
	o.w = half(r2.w);

	return o;
}
