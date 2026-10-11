#include "./shared.h"
#include "./hdr_lut_bridge.hlsli"

// Experimental 4-LUT candidate translated from the extracted HlslDecompiler shader.
float4 BloomTintAndScreenBlendThreshold : register(c7);
sampler2D ColorGradingLUT : register(s3);
sampler2D DNEHexDofTexture1 : register(s2);
float4 DepthDistances : register(c11);
float4 DepthTransition : register(c10);
sampler2D FilterColor1Texture : register(s1);
float4 HalfResMaskRect : register(c9);
float4 ImageAdjustments2 : register(c8);
float4 MinMaxBlurClamp : register(c0);
float4 MinZ_MaxZRatio : register(c2);
sampler2D SceneColorTexture : register(s0);

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
	r0 = tex2D(DNEHexDofTexture1, r1.xy);
	r0.w = r0.w + -0.05;
	r0.w = r0.w * 3;
	r1.z = 5;
	r1.x = r1.z * MinMaxBlurClamp.x;
	r2.x = min(r1.x, 1);
	r0.w = saturate(r0.w * r2.x);
	r1 = float4(1, 1, 0, 0) * i.texcoord1.xyxx;
	r1 = half4(tex2Dlod(SceneColorTexture, r1));
	r2 = half4(lerp(r1.zzxy, r0.zzxy, r0.w));
	r0.x = r1.w + -MinZ_MaxZRatio.y;
	r0.y = half(dot(r2.zwy, 0.59));
	r0.y = half(r0.y * -3);
	r0.y = half(exp2(r0.y));
	r0.y = half(saturate(r0.y * BloomTintAndScreenBlendThreshold.w));
	r1 = tex2D(FilterColor1Texture, i.texcoord.zw);
	r1 = r1.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
	r1 = half4(r1 * 4);
	r1 = half4(r1 * r0.y + r2);
	float3 lis_hdr_untonemapped = float3(r1.z, r1.w, r1.y);
	float lis_hdr_scale = 1.f;
	r0.yzw = half3(r1.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
	r2.z = 1 / r0.y;
	r2.w = 1 / r0.z;
	r2.xy = 1 / r0.ww;
	r1 = half4(saturate(r1 * r2));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  float3 lis_hdr_extended = LifeIsStrangeExtendImageAdjustments2(
	      lis_hdr_untonemapped, ImageAdjustments2.xy, LIFEISSTRANGE_HDR_CURVE_PIVOT, LIFEISSTRANGE_HDR_CURVE_METHOD);
	  float3 lis_hdr_lut_proxy = LifeIsStrangeCompressForColorGradingLut(lis_hdr_extended, lis_hdr_scale);
	  r1.z = lis_hdr_lut_proxy.r;
	  r1.w = lis_hdr_lut_proxy.g;
	  r1.x = lis_hdr_lut_proxy.b;
	  r1.y = lis_hdr_lut_proxy.b;
	}
	r2.xyw = r1.xwz * float3(14.9999, 0.234375, 0.05859375);
	r0.y = frac(r2.x);
	r0.y = -r0.y + r2.x;
	r2.x = r0.y * 0.0625 + r2.w;
	r0.y = r1.y * 15 + -r0.y;
	r0.z = -r0.x + 1E-05;
	r0.x = 1 / r0.x;
	r0.x = (r0.z >= 0) ? 100000 : r0.x;
	r0.x = half(r0.x * MinZ_MaxZRatio.x);
	r1.x = half(min(r0.x, 65504));
	r3 = r1.x + -DepthDistances;
	r3 = half4((r3 >= 0) ? 0 : 1);
	r4.xy = float2(1, 0);
	r4 = DepthDistances.xxyz * -r4.yxxx + r1.x;
	r0.xzw = half3(saturate(r1.xxx * DepthTransition.www + -DepthTransition.xyz));
	r1 = half4((r4 >= 0) ? r3 : 0);
	r1.w = half(dot(r1.yzw, 15));
	r0.x = half(dot(r1.xyz, r0.xzw));
	r0.z = half(r1.w + 1);
	r1.x = r1.w * 0.25 + 0.0078125;
	r3.x = r0.z * 0.25 + 0.0078125;
	r3.yz = float2(0.001953125, 0.064453125);
	r3 = r2.xyxy + r3.yxzx;
	r4 = half4(tex2D(ColorGradingLUT, r3.xy));
	r3 = half4(tex2D(ColorGradingLUT, r3.zw));
	r5 = half4(lerp(r4, r3, r0.y));
	r1.yz = float2(0.001953125, 0.064453125);
	r1 = r2.xyxy + r1.yxzx;
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
