#include "./shared.h"
#include "./hdr_lut_bridge.hlsli"

float4 AspectRatioAndInvAspectRatio : register(c12);
float BloomScreenBlendThreshold : register(c15);
float4 BloomTintAndScreenBlendThreshold : register(c7);
float4 BloomTintAndThreshold : register(c16);
sampler2D ColorGradingLUT : register(s6);
float2 DNEColorStrokesBackBlendDistances : register(c23);
float4 DNEColorStrokesLightSrcColor0 : register(c19);
float4 DNEColorStrokesLightSrcColor1 : register(c22);
float3 DNEColorStrokesLightSrcPos0 : register(c17);
float3 DNEColorStrokesLightSrcPos1 : register(c20);
float2 DNEColorStrokesLightSrcSize0 : register(c18);
float2 DNEColorStrokesLightSrcSize1 : register(c21);
sampler2D DNEHexDofTexture1 : register(s5);
float4 DNEImageGrainParameter : register(c24);
sampler2D DNEImageGrainTexture : register(s3);
float4 DNEVignetColor : register(c25);
float4 DNEVignetMaskFactors : register(c26);
sampler2D DNEVignetTexture : register(s4);
float4 DepthDistances : register(c28);
float4 DepthTransition : register(c27);
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
	float4 r6;
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
	r1.y = -r1.x + 1E-05;
	r1.x = 1 / r1.x;
	r1.x = (r1.y >= 0) ? 100000 : r1.x;
	r2.x = MinZ_MaxZRatio.x;
	r1.y = half(r2.x * r1.x + -DNEColorStrokesBackBlendDistances.x);
	r1.x = half(r1.x * MinZ_MaxZRatio.x);
	r2.x = half(min(r1.x, 65504));
	r1.x = 1 / DNEColorStrokesBackBlendDistances.y;
	r1.x = half(saturate(r1.x * r1.y));
	r1.x = r1.x * r1.x + -1;
	r3.x = half(saturate(DNEColorStrokesLightSrcPos0.z));
	r3.y = half(saturate(DNEColorStrokesLightSrcPos1.z));
	r1.xy = r3.xy * r1.xx + 1;
	r3 = -0.5 + i.texcoord1.xyxy;
	r4.xy = half2(-DNEColorStrokesLightSrcPos0.xy);
	r4.zw = half2(-DNEColorStrokesLightSrcPos1.xy);
	r3 = half4(r3 * 2 + r4);
	r4.x = 1 / DNEColorStrokesLightSrcSize0.x;
	r4.y = 1 / DNEColorStrokesLightSrcSize0.y;
	r4.z = 1 / DNEColorStrokesLightSrcSize1.x;
	r4.w = 1 / DNEColorStrokesLightSrcSize1.y;
	r3 = half4(r3 * r4);
	r3.x = half(dot(r3.xy, r3.xy) + 0);
	r3.y = half(dot(r3.zw, r3.zw) + 0);
	r1.zw = half2(saturate(r3.xy * 4));
	r1.zw = half2(-r1.zw + 1);
	r1.zw = half2(r1.zw * r1.zw);
	r1.xy = half2(r1.xy * r1.zw);
	r3 = half4(r1.y * DNEColorStrokesLightSrcColor1);
	r1 = half4(DNEColorStrokesLightSrcColor0 * r1.x + r3);
	r0 = half4(r0 * 0.33333 + r1.w);
	r1 = r1.zzxy + 1;
	r3 = half4(r0 * r1);
	r2.yz = max(i.texcoord1.zw, HalfResMaskRect.xy);
	r4.xy = min(HalfResMaskRect.zw, r2.yz);
	r4 = tex2D(DNEHexDofTexture1, r4.xy);
	r0 = r0.yyzw * -r1.yyzw + r4.zzxy;
	r1.x = r4.w + -0.05;
	r1.x = r1.x * 3;
	r4.x = 5;
	r1.y = r4.x * MinMaxBlurClamp.x;
	r2.y = min(r1.y, 1);
	r1.x = saturate(r1.x * r2.y);
	r0 = half4(r1.x * r0 + r3);
	r1.x = half(dot(r0.zwy, float3(0.300000012f, 0.589999974f, 0.109999999f)));
	r1.x = half(r1.x * -3);
	r1.x = half(exp2(r1.x));
	r1.y = half(saturate(r1.x * BloomTintAndScreenBlendThreshold.w));
	r1.x = half(saturate(r1.x * BloomScreenBlendThreshold.x));
	r3 = tex2D(FilterColor1Texture, i.texcoord.zw);
	r3 = r3.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
	r3 = half4(r3 * 4);
	r0 = half4(r3 * r1.y + r0);
	r1.zw = AspectRatioAndInvAspectRatio.zw;
	r1.yz = i.texcoord.zw * -r1.zw + TextureSpaceBlurOrigin.xy;
	r1.y = dot(r1.yz, r1.yz) + 0;
	r1.y = rsqrt(r1.y);
	r1.y = 1 / r1.y;
	r1.y = saturate(r1.y * 0.5);
	r3.xw = float2(-0.5, 1.5);
	r1.z = LightShaftParameters.w * -abs(r3.x) + abs(r3.w);
	r3 = half4(tex2D(LightShaftsTexture, i.texcoord.zw));
	r1.w = half(r3.w * r3.w);
	r3 = half4(r3.zzxy * BloomTintAndThreshold.zzxy);
	r3 = r1.x * r3;
	r2.y = lerp(LightShaftParameters.w, r1.z, r1.w);
	r4.x = lerp(r2.y, 1, r1.y);
	r1.x = DistanceFade.x * DistanceFade.x;
	r1.x = r1.x * DistanceFade.x;
	r2.y = half(lerp(r4.x, 1, r1.x));
	r0 = half4(r0 * r2.y);
	r0 = half4(r3 * 4 + r0);
	  float3 untonemapped_color = float3(r0.z, r0.w, r0.y);
  bool use_hdr_lut_bridge = LIFEISSTRANGE_HDR_PIPELINE > 0.f;
  float hdr_lut_scale = 1.f;

  if (use_hdr_lut_bridge) {
    float3 extended_curve = LifeIsStrangeExtendImageAdjustments2(
        untonemapped_color,
        ImageAdjustments2.xy,
        LIFEISSTRANGE_HDR_CURVE_PIVOT,
        LIFEISSTRANGE_HDR_CURVE_METHOD);

    float3 lut_proxy = LifeIsStrangeCompressForColorGradingLut(extended_curve, hdr_lut_scale);

    // All five variants use the SM3 packed order B, B, R, G in r0.xyzw.
    r0 = half4(lut_proxy.b, lut_proxy.b, lut_proxy.r, lut_proxy.g);
  } else {
	r1.xyz = half3(r0.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
	r3.z = 1 / r1.x;
	r3.w = 1 / r1.y;
	r3.xy = 1 / r1.zz;
	r0 = half4(saturate(r0 * r3));
  }
	r1.xyw = r0.xwz * float3(14.9999, 0.234375, 0.05859375);
	r0.x = frac(r1.x);
	r0.x = -r0.x + r1.x;
	r0.y = r0.y * 15 + -r0.x;
	r1.x = r0.x * 0.0625 + r1.w;
	r3 = r2.x + -DepthDistances;
	r3 = half4((r3 >= 0) ? 0 : 1);
	r4.xy = float2(1, 0);
	r5 = DepthDistances.xxyz * -r4.yxxx + r2.x;
	r0.xzw = half3(saturate(r2.xxx * DepthTransition.www + -DepthTransition.xyz));
	r2 = half4((r5 >= 0) ? r3 : 0);
	r1.z = half(dot(r2.yzw, float3(1.f, 2.f, 3.f)));
	r0.x = half(dot(r2.xyz, r0.xzw));
	r0.z = half(r1.z + 1);
	r2.x = r1.z * 0.25 + 0.0078125;
	r3.x = r0.z * 0.25 + 0.0078125;
	r3.yz = float2(0.001953125, 0.064453125);
	r3 = r1.xyxy + r3.yxzx;
	r5 = half4(tex2D(ColorGradingLUT, r3.xy));
	r3 = half4(tex2D(ColorGradingLUT, r3.zw));
	r6 = half4(lerp(r5, r3, r0.y));
	r2.yz = float2(0.001953125, 0.064453125);
	r1 = r1.xyxy + r2.yxzx;
	r2 = half4(tex2D(ColorGradingLUT, r1.xy));
	r1 = half4(tex2D(ColorGradingLUT, r1.zw));
	r3 = half4(lerp(r2, r1, r0.y));
	r1 = half4(lerp(r3, r6, r0.x));
  if (use_hdr_lut_bridge) {
    r1.xyz = LifeIsStrangeRestoreAfterColorGradingLut(r1.xyz, hdr_lut_scale);
  }
	r0 = tex2D(DNEVignetTexture, i.texcoord2.zw);
	r0.x = saturate(dot(r0, DNEVignetMaskFactors));
	r0.yzw = -r4.xxx + DNEVignetColor.xyz;
	r0.xyz = r0.xxx * r0.yzw + 1;
	r2.x = 4;
	r2.xy = half2(i.texcoord2.zw * r2.xx + DNEImageGrainParameter.xy);
	r2 = half4(tex2D(DNEImageGrainTexture, r2.xy));
	r0.w = r2.x * 2 + -1;
	r0.w = r0.w * ImageAdjustments1.w;
	float3 output_color = r1.xyz * r0.xyz + r0.www;
  if (!use_hdr_lut_bridge) {
    // Preserve the native final mad_sat clamp in SDR mode only.
    output_color = saturate(output_color);
  }
  o.xyz = half3(output_color);
	o.w = half(r1.w);

	return o;
}
