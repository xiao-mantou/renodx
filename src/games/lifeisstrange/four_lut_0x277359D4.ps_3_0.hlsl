#include "./shared.h"
#include "./hdr_lut_bridge.hlsli"

// Experimental 4-LUT candidate translated from the extracted HlslDecompiler shader.
float4 BloomTintAndScreenBlendThreshold : register(c7);
sampler2D ColorGradingLUT : register(s5);
sampler2D DNEHexDofTexture1 : register(s4);
float4 DNEImageGrainParameter : register(c11);
sampler2D DNEImageGrainTexture : register(s2);
float4 DNEVignetColor : register(c12);
float4 DNEVignetMaskFactors : register(c13);
sampler2D DNEVignetTexture : register(s3);
float4 DepthDistances : register(c15);
float4 DepthTransition : register(c14);
sampler2D FilterColor1Texture : register(s1);
float4 HalfResMaskRect : register(c10);
float4 ImageAdjustments1 : register(c8);
float4 ImageAdjustments2 : register(c9);
float4 MinMaxBlurClamp : register(c0);
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
	float2 r4;
	float4 r5;
	float4 r6;
	r0.xy = max(i.texcoord1.zw, HalfResMaskRect.xy);
	r1.xy = min(HalfResMaskRect.zw, r0.xy);
	r0 = tex2D(DNEHexDofTexture1, r1.xy);
	r0.w = r0.w + -0.05;
	r0.w = r0.w * 3;
	r1.zw = float2(5, 4);
	r1.x = r1.z * MinMaxBlurClamp.x;
	r2.x = min(r1.x, 1);
	r0.w = saturate(r0.w * r2.x);
	r2 = float4(1, 1, 0, 0) * i.texcoord1.xyxx;
	r2 = half4(tex2Dlod(SceneColorTexture, r2));
	r3 = half4(lerp(r2.zzxy, r0.zzxy, r0.w));
	r0.x = r2.w + -MinZ_MaxZRatio.y;
	r0.y = half(dot(r3.zwy, 0.59));
	r0.y = half(r0.y * -3);
	r0.y = half(exp2(r0.y));
	r0.y = half(saturate(r0.y * BloomTintAndScreenBlendThreshold.w));
	r2 = tex2D(FilterColor1Texture, i.texcoord.zw);
	r2 = r2.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
	r2 = half4(r2 * 4);
	r2 = half4(r2 * r0.y + r3);
	float3 lis_hdr_untonemapped = float3(r2.z, r2.w, r2.y);
	float lis_hdr_scale = 1.f;
	r0.yzw = half3(r2.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
	r3.z = 1 / r0.y;
	r3.w = 1 / r0.z;
	r3.xy = 1 / r0.ww;
	r2 = half4(saturate(r2 * r3));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  float3 lis_hdr_extended = LifeIsStrangeExtendImageAdjustments2(
	      lis_hdr_untonemapped, ImageAdjustments2.xy, LIFEISSTRANGE_HDR_CURVE_PIVOT, LIFEISSTRANGE_HDR_CURVE_METHOD);
	  float3 lis_hdr_lut_proxy = LifeIsStrangeCompressForColorGradingLut(lis_hdr_extended, lis_hdr_scale);
	  r2.z = lis_hdr_lut_proxy.r;
	  r2.w = lis_hdr_lut_proxy.g;
	  r2.x = lis_hdr_lut_proxy.b;
	  r2.y = lis_hdr_lut_proxy.b;
	}
	r3.xyw = r2.xwz * float3(14.9999, 0.234375, 0.05859375);
	r0.y = frac(r3.x);
	r0.y = -r0.y + r3.x;
	r3.x = r0.y * 0.0625 + r3.w;
	r0.y = r2.y * 15 + -r0.y;
	r0.z = -r0.x + 1E-05;
	r0.x = 1 / r0.x;
	r0.x = (r0.z >= 0) ? 100000 : r0.x;
	r0.x = half(r0.x * MinZ_MaxZRatio.x);
	r1.x = half(min(r0.x, 65504));
	r2 = r1.x + -DepthDistances;
	r2 = half4((r2 >= 0) ? 0 : 1);
	r4 = float2(1, 0);
	r5 = DepthDistances.xxyz * -r4.yxxx + r1.x;
	r0.xzw = half3(saturate(r1.xxx * DepthTransition.www + -DepthTransition.xyz));
	r2 = half4((r5 >= 0) ? r2 : 0);
	r1.x = half(dot(r2.yzw, 1));
	r0.x = half(dot(r2.xyz, r0.xzw));
	r0.z = half(r1.x + 1);
	r1.x = r1.x * 0.25 + 0.0078125;
	r2.x = r0.z * 0.25 + 0.0078125;
	r2.yz = float2(0.001953125, 0.064453125);
	r2 = r3.xyxy + r2.yxzx;
	r5 = half4(tex2D(ColorGradingLUT, r2.xy));
	r2 = half4(tex2D(ColorGradingLUT, r2.zw));
	r6 = half4(lerp(r5, r2, r0.y));
	r1.yz = float2(0.001953125, 0.064453125);
	r2 = r3.xyxy + r1.yxzx;
	r3 = half4(tex2D(ColorGradingLUT, r2.xy));
	r2 = half4(tex2D(ColorGradingLUT, r2.zw));
	r5 = half4(lerp(r3, r2, r0.y));
	r2 = half4(lerp(r5, r6, r0.x));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  r2.xyz = LifeIsStrangeRestoreAfterColorGradingLut(r2.xyz, lis_hdr_scale);
	}
	r0 = tex2D(DNEVignetTexture, i.texcoord2.zw);
	r0.x = saturate(dot(r0, DNEVignetMaskFactors));
	r0.yzw = -r4.xxx + DNEVignetColor.xyz;
	r0.xyz = r0.xxx * r0.yzw + 1;
	r1.xy = half2(i.texcoord2.zw * r1.ww + DNEImageGrainParameter.xy);
	r1 = half4(tex2D(DNEImageGrainTexture, r1.xy));
	r0.w = r1.x * 2 + -1;
	r0.w = r0.w * ImageAdjustments1.w;
	o.xyz = half3(saturate(r2.xyz * r0.xyz + r0.www));
	o.w = half(r2.w);

	return o;
}
