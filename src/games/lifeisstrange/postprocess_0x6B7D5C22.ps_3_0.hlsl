#include "./shared.h"

float4 BloomTintAndScreenBlendThreshold : register(c0);
float4 MinZ_MaxZRatio : register(c2);
float4 ImageAdjustments1 : register(c7);
float4 ImageAdjustments2 : register(c8);
float4 HalfResMaskRect : register(c9);
float4 LightShaftParameters : register(c10);
float4 AspectRatioAndInvAspectRatio : register(c11);
float2 TextureSpaceBlurOrigin : register(c12);
float DistanceFade : register(c13);
float BloomScreenBlendThreshold : register(c14);
float4 BloomTintAndThreshold : register(c15);
float3 DNEColorStrokesLightSrcPos0 : register(c16);
float2 DNEColorStrokesLightSrcSize0 : register(c17);
float4 DNEColorStrokesLightSrcColor0 : register(c18);
float3 DNEColorStrokesLightSrcPos1 : register(c19);
float2 DNEColorStrokesLightSrcSize1 : register(c20);
float4 DNEColorStrokesLightSrcColor1 : register(c21);
float2 DNEColorStrokesBackBlendDistances : register(c22);
float4 DNEImageGrainParameter : register(c23);
float4 DNEVignetColor : register(c24);
float4 DNEVignetMaskFactors : register(c25);
float4 DepthTransition : register(c26);
float4 DepthDistances : register(c27);

sampler2D SceneColorTexture : register(s0);
sampler2D FilterColor1Texture : register(s1);
sampler2D LightShaftsTexture : register(s2);
sampler2D DNEImageGrainTexture : register(s3);
sampler2D DNEVignetTexture : register(s4);
sampler2D ColorGradingLUT : register(s5);
sampler2D LowResPostProcessBuffer : register(s6);

static const float4 c1 = float4(1.f, 0.f, 0.00000999999975f, 100000.f);
static const float4 c3 = float4(-0.5f, 2.82800007f, 1.f, 1.5f);
static const float4 c4 = float4(0.00700000022f, 0.00350000011f, -0.360000014f, 15.f);
static const float4 c5 = float4(0.333999991f, -0.666000009f, 0.666000009f, 2.f);
static const float4 c6 = float4(0.333999991f, 0.f, -0.333000004f, 0.666000009f);
static const float4 c28 = float4(4.f, 0.333330005f, -3.f, 65504.f);
static const float4 c29 = float4(0.300000012f, 0.589999974f, 0.109999999f, 0.0625f);
static const float4 c30 = float4(1.f, 2.f, 3.f, -1.f);
static const float4 c31 = float4(0.25f, 0.0078125f, 0.001953125f, 0.064453125f);
static const float4 c32 = float4(14.9998999f, 0.05859375f, 0.234375f, 0.f);

struct PS_IN {
  float4 texcoord : TEXCOORD;
  float4 texcoord1 : TEXCOORD1;
  float4 texcoord2 : TEXCOORD2;
};

float4 main(PS_IN i) : COLOR {
  float4 r0;
  float4 r1;
  float4 r2;
  float4 r3;
  float4 r4;
  float4 r5;
  float4 r6;

  r0.xy = half2(c3.x + i.texcoord2.zw);
  r0.zw = half2(r0.xy * r0.xy);
  r0.zw = half2(r0.zw * -c3.y + c3.z);
  r0.zw = half2(r0.zw * r0.zw);
  r0.zw = half2(r0.zw * -r0.zw + c1.x);
  r1.x = log2(r0.z);
  r1.y = log2(r0.w);
  r0.zw = r1.xy * c3.w;
  r1.xz = half2(exp2(r0.zz));
  r1.yw = half2(exp2(r0.ww));
  r1 = half4(-r0.xyxy * r1);
  r2.x = half(max(abs(r0.x), abs(r0.y)));
  r0.x = r2.x + c4.z;
  r0.x = saturate(r0.x * c4.w);
  r2 = saturate(r1.zwxy * -c4.xxyy + i.texcoord1.xyxy);
  r1 = saturate(r1 * c4.yyxx + i.texcoord1.xyxy);

  r3 = r2.zwxx * c1.xxyy;
  r2 = r2.xyxx * c1.xxyy;
  r2 = half4(tex2Dlod(SceneColorTexture, r2));
  r3 = half4(tex2Dlod(SceneColorTexture, r3));
  r4 = r0.x * c5.xxyx + c5.z;
  r5 = c6;
  r0 = r0.x * r5.xxyz + abs(r5.wwyz);
  r3 = r3.zzxy * r4.zzww;
  r2 = r2.zzxy * r0.zzyw + r3;
  r3 = r1.xyxx * c1.xxyy;
  r1 = r1.zwxx * c1.xxyy;
  r1 = half4(tex2Dlod(SceneColorTexture, r1));
  r3 = half4(tex2Dlod(SceneColorTexture, r3));
  r2 = r3.zzxy * r4 + r2;
  r0 = r1.zzxy * r0 + r2;
  r1 = c1.xxyy * i.texcoord1.xyxx;
  r1 = tex2Dlod(SceneColorTexture, r1);
  r0 = half4(r0 + r1.zzxy);

  r1.x = r1.w - MinZ_MaxZRatio.y;
  r1.y = -r1.x + c1.z;
  r1.x = rcp(r1.x);
  r1.x = r1.y >= 0.f ? c1.w : r1.x;
  r2.x = MinZ_MaxZRatio.x;
  r1.y = half(r2.x * r1.x - DNEColorStrokesBackBlendDistances.x);
  r1.x = half(r1.x * MinZ_MaxZRatio.x);
  r2.x = half(min(r1.x, c28.w));
  r1.x = rcp(DNEColorStrokesBackBlendDistances.y);
  r1.x = half(saturate(r1.x * r1.y));
  r1.x = r1.x * r1.x - c1.x;
  r3.x = half(saturate(DNEColorStrokesLightSrcPos0.z));
  r3.y = half(saturate(DNEColorStrokesLightSrcPos1.z));
  r1.xy = r3.xy * r1.xx + c1.xx;
  r3 = c3.x + i.texcoord1.xyxy;
  r4.xy = half2(-DNEColorStrokesLightSrcPos0.xy);
  r4.zw = half2(-DNEColorStrokesLightSrcPos1.xy);
  r3 = half4(r3 * c5.w + r4);
  r4.x = rcp(DNEColorStrokesLightSrcSize0.x);
  r4.y = rcp(DNEColorStrokesLightSrcSize0.y);
  r4.z = rcp(DNEColorStrokesLightSrcSize1.x);
  r4.w = rcp(DNEColorStrokesLightSrcSize1.y);
  r3 = half4(r3 * r4);
  r3.x = half(dot(r3.xy, r3.xy) + c1.y);
  r3.y = half(dot(r3.zw, r3.zw) + c1.y);
  r1.zw = half2(saturate(r3.xy * c28.x));
  r1.zw = half2(-r1.zw + c1.x);
  r1.zw = half2(r1.zw * r1.zw);
  r1.xy = half2(r1.xy * r1.zwzw);
  r3 = half4(r1.y * DNEColorStrokesLightSrcColor1);
  r1 = half4(DNEColorStrokesLightSrcColor0 * r1.x + r3);
  r0 = half4(r0 * c28.y + r1.w);
  r1 = r1.zzxy + c1.x;

  r2.yz = max(i.texcoord1.zw, HalfResMaskRect.xy);
  r3.xy = min(HalfResMaskRect.zw, r2.yz);
  r3 = tex2D(LowResPostProcessBuffer, r3.xy);
  r4 = r3.zzxy * c28.x;
  r0 = r0 * r1 - r4.yyzw;
  r0 = half4(r3.w * r0 + r4);
  r1.x = half(dot(r0.zwy, c29.xyz));
  r1.x = half(r1.x * c28.z);
  r1.x = half(exp2(r1.x));
  r1.y = half(saturate(r1.x * BloomTintAndScreenBlendThreshold.w));
  r1.x = half(saturate(r1.x * BloomScreenBlendThreshold));
  r3 = tex2D(FilterColor1Texture, i.texcoord.zw);
  r3 = r3.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
  r3 = half4(r3 * c28.x);
  r0 = half4(r3 * r1.y + r0);

  r1.zw = AspectRatioAndInvAspectRatio.zw;
  r1.yz = i.texcoord.zw * -r1.zw + TextureSpaceBlurOrigin.xy;
  r1.y = dot(r1.yz, r1.yz) + c1.y;
  r1.y = rsqrt(r1.y);
  r1.y = rcp(r1.y);
  r1.y = saturate(r1.y * -c3.x);
  r3.xw = c3.xw;
  r1.z = LightShaftParameters.w * -abs(r3.x) + abs(r3.w);
  r3 = tex2D(LightShaftsTexture, i.texcoord.zw);
  r1.w = half(r3.w * r3.w);
  r3 = half4(r3.zzxy * BloomTintAndThreshold.zzxy);
  r3 = r1.x * r3;
  r2.y = lerp(LightShaftParameters.w, r1.z, r1.w);
  r4.x = lerp(r2.y, c1.x, r1.y);
  r1.x = DistanceFade * DistanceFade;
  r1.x = r1.x * DistanceFade;
  r2.y = half(lerp(r4.x, c1.x, r1.x));
  r0 = half4(r0 * r2.y);
  r0 = half4(r3 * c28.x + r0);

  float3 untonemapped_color = r0.zwy;
  bool use_hdr_lut_bridge = LIFEISSTRANGE_HDR_PIPELINE > 0.f;
  float hdr_lut_scale = 1.f;

  r1.xyz = half3(r0.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
  r3.z = rcp(r1.x);
  r3.w = rcp(r1.y);
  r3.xy = rcp(r1.zz);
  r0 *= r3;

  if (use_hdr_lut_bridge) {
    const float pivot = LIFEISSTRANGE_HDR_CURVE_PIVOT;
    float3 curve_base_input = min(untonemapped_color, pivot.xxx);
    float3 curve_base = curve_base_input
                        / (ImageAdjustments2.x + ImageAdjustments2.y * curve_base_input);
    float pivot_denominator = ImageAdjustments2.x + ImageAdjustments2.y * pivot;
    float pivot_slope = ImageAdjustments2.x / (pivot_denominator * pivot_denominator);
    float3 extended_curve = curve_base + pivot_slope * max(untonemapped_color - pivot.xxx, 0.f);

    if (LIFEISSTRANGE_HDR_CURVE_METHOD > 0.5f) {
      float max_channel_input = max(
          untonemapped_color.r,
          max(untonemapped_color.g, untonemapped_color.b));
      if (max_channel_input > pivot) {
        float max_channel_extended = pivot / pivot_denominator
                                     + pivot_slope * (max_channel_input - pivot);
        float max_channel_vanilla = max_channel_input
                                    / (ImageAdjustments2.x + ImageAdjustments2.y * max_channel_input);
        float shared_scale = max_channel_extended / max(max_channel_vanilla, 1e-6f);
        float3 vanilla_curve = untonemapped_color
                               / (ImageAdjustments2.x + ImageAdjustments2.y * untonemapped_color);
        extended_curve = vanilla_curve * shared_scale;
      }
    }

    // The native shader feeds this shaped signal directly to its packed LUT.
    // The sRGB interpretation follows the existing 06A2 transport experiment and remains provisional.
    float3 extended_curve_linear = renodx::color::srgb::DecodeSafe(extended_curve);
    hdr_lut_scale = renodx::tonemap::neutwo::ComputeMaxChannelScale(extended_curve_linear);
    float3 lut_proxy_linear = extended_curve_linear * hdr_lut_scale;
    float3 lut_proxy = saturate(renodx::color::srgb::EncodeSafe(lut_proxy_linear));

    // Native packed-LUT register order is B, B, R, G in r0.xyzw.
    r0 = half4(lut_proxy.b, lut_proxy.b, lut_proxy.r, lut_proxy.g);
  } else {
    // Preserve the native mul_sat LUT-domain clamp in SDR mode.
    r0 = half4(saturate(r0));
  }

  r1.xyw = r0.xwz * c32.xzy;
  r0.x = frac(r1.x);
  r0.x = -r0.x + r1.x;
  r0.y = r0.y * c4.w - r0.x;
  r1.x = r0.x * c29.w + r1.w;

  r3 = r2.x - DepthDistances;
  r3 = half4(r3 >= 0.f ? c1.y : c1.x);
  r4.xy = c1.xy;
  r5 = DepthDistances.xxyz * -r4.yxxx + r2.x;
  r0.xzw = half3(saturate(r2.xxx * DepthTransition.www - DepthTransition.xyz));
  r2 = half4(r5 >= 0.f ? r3 : c1.y);
  r1.z = half(dot(r2.yzw, c30.xyz));
  r0.x = half(dot(r2.xyz, r0.xzw));
  r0.z = half(r1.z + c1.x);
  r2.x = r1.z * c31.x + c31.y;
  r3.x = r0.z * c31.x + c31.y;
  r3.yz = c31.zw;
  r3 = r1.xyxy + r3.yxzx;
  r5 = half4(tex2D(ColorGradingLUT, r3.xy));
  r3 = half4(tex2D(ColorGradingLUT, r3.zw));
  r6 = half4(lerp(r5, r3, r0.y));
  r2.yz = c31.zw;
  r1 = r1.xyxy + r2.yxzx;
  r2 = half4(tex2D(ColorGradingLUT, r1.xy));
  r1 = half4(tex2D(ColorGradingLUT, r1.zw));
  r3 = half4(lerp(r2, r1, r0.y));
  r1 = half4(lerp(r3, r6, r0.x));

  if (use_hdr_lut_bridge) {
    float3 graded_sdr = renodx::color::srgb::DecodeSafe(r1.xyz);
    float3 reconstructed_hdr = renodx::math::DivideSafe(
        graded_sdr,
        hdr_lut_scale.xxx,
        graded_sdr);
    r1.xyz = renodx::color::srgb::EncodeSafe(reconstructed_hdr);
  }

  r0 = tex2D(DNEVignetTexture, i.texcoord2.zw);
  r0.x = saturate(dot(r0, DNEVignetMaskFactors));
  r0.yzw = -r4.x + DNEVignetColor.xyz;
  r0.xyz = r0.xxx * r0.yzw + c1.xxx;
  r2.x = c28.x;
  r2.xy = i.texcoord2.zw * r2.xx + DNEImageGrainParameter.xy;
  r2 = half4(tex2D(DNEImageGrainTexture, r2.xy));
  r0.w = r2.x * c30.y + c30.w;
  r0.w = r0.w * ImageAdjustments1.w;

  float3 output_color = r1.xyz * r0.xyz + r0.www;
  if (!use_hdr_lut_bridge) {
    // Preserve the native final mad_sat in SDR mode; the HDR path writes to FP16.
    output_color = half3(saturate(output_color));
  }
  return float4(output_color, half(r1.w));
}
