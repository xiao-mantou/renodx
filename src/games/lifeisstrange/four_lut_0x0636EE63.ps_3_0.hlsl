#include "./shared.h"
#include "./hdr_lut_bridge.hlsli"

// Experimental 4-LUT candidate translated from the extracted HlslDecompiler shader.
float4 AspectRatioAndInvAspectRatio : register(c12);
float BloomScreenBlendThreshold : register(c15);
float4 BloomTintAndScreenBlendThreshold : register(c7);
float4 BloomTintAndThreshold : register(c16);
sampler2D ColorGradingLUT : register(s6);
sampler2D DNEHexDofTexture1 : register(s5);
float4 DNEImageGrainParameter : register(c17);
sampler2D DNEImageGrainTexture : register(s3);
float4 DNEVignetColor : register(c18);
float4 DNEVignetMaskFactors : register(c19);
sampler2D DNEVignetTexture : register(s4);
float4 DepthDistances : register(c21);
float4 DepthTransition : register(c20);
float DistanceFade : register(c14);
sampler2D FilterColor1Texture : register(s1);
float4 HalfResMaskRect : register(c10);
float4 ImageAdjustments1 : register(c8);
float4 ImageAdjustments2 : register(c9);
float4 LightShaftParameters : register(c11);
sampler2D LightShaftsTexture : register(s2);
float4 MinMaxBlurClamp : register(c0);
float4 MinZ_MaxZRatio : register(c2);
sampler2D SceneColorTexture : register(s0);
float2 TextureSpaceBlurOrigin : register(c13);

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
	r0.zw = AspectRatioAndInvAspectRatio.zw;
	r0.xy = i.texcoord.zw * -r0.zw + TextureSpaceBlurOrigin.xy;
	r0.x = dot(r0.xy, r0.xy) + 0;
	r0.x = rsqrt(r0.x);
	r0.x = 1 / r0.x;
	r0.x = saturate(r0.x * 0.5);
	r1.xy = float2(0.5, 1.5);
	r0.y = LightShaftParameters.w * -r1.x + r1.y;
	r1 = half4(tex2D(LightShaftsTexture, i.texcoord.zw));
	r0.z = half(r1.w * r1.w);
	r1 = half4(r1.zzxy * BloomTintAndThreshold.zzxy);
	r2.x = lerp(LightShaftParameters.w, r0.y, r0.z);
	r3.x = lerp(r2.x, 1, r0.x);
	r0.x = DistanceFade.x * DistanceFade.x;
	r0.x = r0.x * DistanceFade.x;
	r2.x = half(lerp(r3.x, 1, r0.x));
	r0.xy = max(i.texcoord1.zw, HalfResMaskRect.xy);
	r2.yz = min(HalfResMaskRect.zw, r0.xy);
	r0 = tex2D(DNEHexDofTexture1, r2.yz);
	r0.w = r0.w + -0.05;
	r0.w = r0.w * 3;
	r2.zw = float2(5, 4);
	r2.y = r2.z * MinMaxBlurClamp.x;
	r3.x = min(r2.y, 1);
	r0.w = saturate(r0.w * r3.x);
	r3 = float4(1, 1, 0, 0) * i.texcoord1.xyxx;
	r3 = half4(tex2Dlod(SceneColorTexture, r3));
	r4 = half4(lerp(r3.zzxy, r0.zzxy, r0.w));
	r0.x = r3.w + -MinZ_MaxZRatio.y;
	r3 = tex2D(FilterColor1Texture, i.texcoord.zw);
	r3 = r3.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
	r3 = half4(r3 * 4);
	r0.y = half(dot(r4.zwy, 0.59));
	r0.y = half(r0.y * -3);
	r0.y = half(exp2(r0.y));
	r0.z = half(saturate(r0.y * BloomTintAndScreenBlendThreshold.w));
	r0.y = half(saturate(r0.y * BloomScreenBlendThreshold.x));
	r1 = r0.y * r1;
	r3 = half4(r3 * r0.z + r4);
	r3 = half4(r2.x * r3);
	r1 = half4(r1 * 4 + r3);
	float3 lis_hdr_untonemapped = float3(r1.z, r1.w, r1.y);
	float lis_hdr_scale = 1.f;
	r0.yzw = half3(r1.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
	r3.z = 1 / r0.y;
	r3.w = 1 / r0.z;
	r3.xy = 1 / r0.ww;
	r1 = half4(saturate(r1 * r3));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  float3 lis_hdr_extended = LifeIsStrangeExtendImageAdjustments2(
	      lis_hdr_untonemapped, ImageAdjustments2.xy, LIFEISSTRANGE_HDR_CURVE_PIVOT, LIFEISSTRANGE_HDR_CURVE_METHOD);
	  float3 lis_hdr_lut_proxy = LifeIsStrangeCompressForColorGradingLut(lis_hdr_extended, lis_hdr_scale);
	  r1.z = lis_hdr_lut_proxy.r;
	  r1.w = lis_hdr_lut_proxy.g;
	  r1.x = lis_hdr_lut_proxy.b;
	  r1.y = lis_hdr_lut_proxy.b;
	}
	r3.xyw = r1.xwz * float3(14.9999, 0.234375, 0.05859375);
	r0.y = frac(r3.x);
	r0.y = -r0.y + r3.x;
	r0.z = r1.y * 15 + -r0.y;
	r3.x = r0.y * 0.0625 + r3.w;
	r0.y = -r0.x + 1E-05;
	r0.x = 1 / r0.x;
	r0.x = (r0.y >= 0) ? 100000 : r0.x;
	r0.x = half(r0.x * MinZ_MaxZRatio.x);
	r1.x = half(min(r0.x, 65504));
	r4 = r1.x + -DepthDistances;
	r4 = half4((r4 >= 0) ? 0 : 1);
	r0.xy = float2(1, 0);
	r5 = DepthDistances.xxyz * -r0.yxxx + r1.x;
	r1.xyz = half3(saturate(r1.xxx * DepthTransition.www + -DepthTransition.xyz));
	r4 = half4((r5 >= 0) ? r4 : 0);
	r0.y = half(dot(r4.yzw, 2));
	r0.w = half(dot(r4.xyz, r1.xyz));
	r1.x = half(r0.y + 1);
	r2.x = r0.y * 0.25 + 0.0078125;
	r1.x = r1.x * 0.25 + 0.0078125;
	r1.yz = float2(0.001953125, 0.064453125);
	r1 = r3.xyxy + r1.yxzx;
	r4 = half4(tex2D(ColorGradingLUT, r1.xy));
	r1 = half4(tex2D(ColorGradingLUT, r1.zw));
	r5 = half4(lerp(r4, r1, r0.z));
	r2.yz = float2(0.001953125, 0.064453125);
	r1 = r3.xyxy + r2.yxzx;
	r3 = half4(tex2D(ColorGradingLUT, r1.xy));
	r1 = half4(tex2D(ColorGradingLUT, r1.zw));
	r4 = half4(lerp(r3, r1, r0.z));
	r1 = half4(lerp(r4, r5, r0.w));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  r1.xyz = LifeIsStrangeRestoreAfterColorGradingLut(r1.xyz, lis_hdr_scale);
	}
	r3 = tex2D(DNEVignetTexture, i.texcoord2.zw);
	r0.y = saturate(dot(r3, DNEVignetMaskFactors));
	r0.xzw = -r0.xxx + DNEVignetColor.xyz;
	r0.xyz = r0.yyy * r0.xzw + 1;
	r2.xy = half2(i.texcoord2.zw * r2.ww + DNEImageGrainParameter.xy);
	r2 = half4(tex2D(DNEImageGrainTexture, r2.xy));
	r0.w = r2.x * 2 + -1;
	r0.w = r0.w * ImageAdjustments1.w;
	o.xyz = half3(saturate(r1.xyz * r0.xyz + r0.www));
	o.w = half(r1.w);

	return o;
}
