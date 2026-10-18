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
	r0.xy = half2(-0.5 + i.texcoord2.zw);
	r0.zw = half2(r0.xy * r0.xy);
	r0.zw = half2(r0.zw * -2.828 + 1);
	r0.zw = half2(r0.zw * r0.zw);
	r0.zw = half2(r0.zw * -r0.zw + 1);
	r1.x = log2(r0.z);
	r1.y = log2(r0.w);
	r0.zw = r1.xy * 1.5;
	r1.xz = half2(exp2(r0.zz));
	r1.yw = half2(exp2(r0.ww));
	r1 = half4(-r0.xyxy * r1);
	r2.x = half(max(abs(r0.x), abs(r0.y)));
	r0.x = r2.x + -0.36;
	r0.x = saturate(r0.x * 15);
	r2 = saturate(r1.zwxy * float4(-0.007, -0.007, -0.0035, -0.0035) + i.texcoord1.xyxy);
	r1 = saturate(r1 * float4(0.0035, 0.0035, 0.007, 0.007) + i.texcoord1.xyxy);
	r3 = r2.zwxx * float4(1, 1, 0, 0);
	r2 = r2.xyxx * float4(1, 1, 0, 0);
	r2 = half4(tex2Dlod(SceneColorTexture, r2));
	r3 = half4(tex2Dlod(SceneColorTexture, r3));
	r4 = r0.x * float4(0.334, 0.334, -0.666, 0.334) + 0.666;
	r5 = float4(0.334, 0, -0.333, 0.666);
	r0 = r0.x * r5.xxyz + abs(r5.wwyz);
	r3 = r3.zzxy * r4.zzww;
	r2 = r2.zzxy * r0.zzyw + r3;
	r3 = r1.xyxx * float4(1, 1, 0, 0);
	r1 = r1.zwxx * float4(1, 1, 0, 0);
	r1 = half4(tex2Dlod(SceneColorTexture, r1));
	r3 = half4(tex2Dlod(SceneColorTexture, r3));
	r2 = r3.zzxy * r4 + r2;
	r0 = r1.zzxy * r0 + r2;
	r1 = float4(1, 1, 0, 0) * i.texcoord1.xyxx;
	r1 = half4(tex2Dlod(SceneColorTexture, r1));
	r0 = half4(r0 + r1.zzxy);
	r1.x = r1.w + -MinZ_MaxZRatio.y;
	r1.yz = max(i.texcoord1.zw, HalfResMaskRect.xy);
	r2.xy = min(HalfResMaskRect.zw, r1.yz);
	r2 = tex2D(DNEHexDofTexture2, r2.xy);
	r1.y = 1 / r2.w;
	r2 = saturate(r1.y * r2.zzxy);
	r2 = r0.yyzw * -0.33333 + r2;
	r0 = half4(r0 * 0.33333);
	r1.y = -r1.x + 1E-05;
	r1.x = 1 / r1.x;
	r1.x = (r1.y >= 0) ? 100000 : r1.x;
	r3.x = half(PackedParameters.x);
	r1.y = half(MinZ_MaxZRatio.x * r1.x + -r3.x);
	r1.x = half(r1.x * MinZ_MaxZRatio.x);
	r3.x = half(min(r1.x, 65504));
	r1.x = half(saturate(abs(r1.y) * PackedParameters.y));
	r3.y = half(pow(r1.x, PackedParameters.z));
	r4.xy = float2(1, 0);
	r1.x = half((r1.y >= 0) ? MinMaxBlurClamp.y : r4.x);
	r4.z = half(min(r3.y, r1.x));
	r1.x = half(max(PackedParameters.w, r4.z));
	r1.z = half((-r1.y >= 0) ? 0 : 1);
	r1.y = half((r1.y >= 0) ? -0 : -1);
	r1.y = half(r1.y + r1.z);
	r1.x = half(r1.y * r1.x);
	r1.x = saturate(r1.x * 10);
	r0 = half4(r1.x * r2 + r0);
	r1.x = half(dot(r0.zwy, 0.3));
	r1.x = half(r1.x * -3);
	r1.x = half(exp2(r1.x));
	r1.y = half(saturate(r1.x * BloomTintAndScreenBlendThreshold.w));
	r1.x = half(saturate(r1.x * BloomScreenBlendThreshold.x));
	r2 = tex2D(FilterColor1Texture, i.texcoord.zw);
	r2 = r2.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
	r2 = half4(r2 * 4);
	r0 = half4(r2 * r1.y + r0);
	r1.zw = AspectRatioAndInvAspectRatio.zw;
	r1.yz = i.texcoord.zw * -r1.zw + TextureSpaceBlurOrigin.xy;
	r1.y = dot(r1.yz, r1.yz) + 0;
	r1.y = rsqrt(r1.y);
	r1.y = 1 / r1.y;
	r1.y = saturate(r1.y * 0.5);
	r2.xw = float2(-0.5, 1.5);
	r1.z = LightShaftParameters.w * -abs(r2.x) + abs(r2.w);
	r2 = half4(tex2D(LightShaftsTexture, i.texcoord.zw));
	r1.w = half(r2.w * r2.w);
	r2 = half4(r2.zzxy * BloomTintAndThreshold.zzxy);
	r2 = r1.x * r2;
	r3.y = lerp(LightShaftParameters.w, r1.z, r1.w);
	r4.z = lerp(r3.y, 1, r1.y);
	r1.x = DistanceFade.x * DistanceFade.x;
	r1.x = r1.x * DistanceFade.x;
	r3.y = half(lerp(r4.z, 1, r1.x));
	r0 = half4(r0 * r3.y);
	r0 = half4(r2 * 4 + r0);
	float3 lis_hdr_untonemapped = float3(r0.z, r0.w, r0.y);
	float lis_hdr_scale = 1.f;
	r1.xyz = half3(r0.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
	r2.z = 1 / r1.x;
	r2.w = 1 / r1.y;
	r2.xy = 1 / r1.zz;
	r0 = half4(saturate(r0 * r2));
	if (LIFEISSTRANGE_HDR_PIPELINE > 0.f) {
	  float3 lis_hdr_reference;
	  float3 lis_hdr_extended = LifeIsStrangeExtendImageAdjustments2(
	      lis_hdr_untonemapped, ImageAdjustments2.xy, LIFEISSTRANGE_HDR_CURVE_PIVOT, LIFEISSTRANGE_HDR_CURVE_METHOD,
	      lis_hdr_reference);
	  float3 lis_hdr_lut_proxy = LifeIsStrangeCompressForColorGradingLut(
	      lis_hdr_extended,
	      lis_hdr_reference,
	      lis_hdr_scale);
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
	r2 = r3.x + -DepthDistances;
	r2 = half4((r2 >= 0) ? 0 : 1);
	r4 = DepthDistances.xxyz * -r4.yxxx + r3.x;
	r0.xzw = half3(saturate(r3.xxx * DepthTransition.www + -DepthTransition.xyz));
	r2 = half4((r4 >= 0) ? r2 : 0);
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
	  o.xyz = LifeIsStrangeApplyToneMapOrRestoreColorGradingLut(o.xyz, lis_hdr_scale, lis_hdr_untonemapped);
	}

	o.xyz = LifeIsStrangeApplyShaderMarker(o.xyz, i.texcoord2.zw, 18.f);
	return o;
}
