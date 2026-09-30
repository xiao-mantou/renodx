#include "./shared.h"

float4 BloomTintAndScreenBlendThreshold : register(c0);
float4 MinZ_MaxZRatio : register(c2);
float4 ImageAdjustments1 : register(c7);
float4 ImageAdjustments2 : register(c8);
float4 HalfResMaskRect : register(c9);
float4 DNEColorStrokesLightSrcPos0 : register(c10);
float4 DNEColorStrokesLightSrcSize0 : register(c11);
float4 DNEColorStrokesLightSrcColor0 : register(c12);
float4 DNEColorStrokesLightSrcPos1 : register(c13);
float4 DNEColorStrokesLightSrcSize1 : register(c14);
float4 DNEColorStrokesLightSrcColor1 : register(c15);
float4 DNEColorStrokesBackBlendDistances : register(c16);
float4 DNEImageGrainParameter : register(c17);
float4 DNEVignetColor : register(c18);
float4 DNEVignetMaskFactors : register(c19);
float4 DepthTransition : register(c20);
float4 DepthDistances : register(c21);

sampler2D SceneColorTexture : register(s0);
sampler2D FilterColor1Texture : register(s1);
sampler2D DNEImageGrainTexture : register(s2);
sampler2D DNEVignetTexture : register(s3);
sampler2D ColorGradingLUT : register(s4);
sampler2D LowResPostProcessBuffer : register(s5);

static const float4 c1 = float4(1.f, 0.f, 0.00001f, 100000.f);
static const float4 c3 = float4(-0.5f, 2.82800007f, 1.f, 1.5f);
static const float4 c4 = float4(0.33399999f, -0.66600001f, 0.66600001f, 2.f);
static const float4 c5 = float4(0.0070000002f, 0.0035000001f, -0.36000001f, 15.f);
static const float4 c6 = float4(0.33399999f, 0.f, -0.33300000f, 0.66600001f);
static const float4 c22 = float4(4.f, 0.33333001f, -3.f, 65504.f);
static const float4 c23 = float4(0.300000012f, 0.589999974f, 0.109999999f, 0.0625f);
static const float4 c24 = float4(1.f, 2.f, 3.f, -1.f);
static const float4 c25 = float4(0.25f, 0.0078125f, 0.001953125f, 0.064453125f);
static const float4 c26 = float4(14.9998999f, 0.05859375f, 0.234375f, 0.f);
struct PS_IN {
  float4 texcoord : TEXCOORD;
  float4 texcoord1 : TEXCOORD1;
  float4 texcoord2 : TEXCOORD2;
};

float4 main(PS_IN i) : COLOR {
  float4 r0 = 0.f;
  float4 r1 = 0.f;
  float4 r2 = 0.f;
  float4 r3 = 0.f;
  float4 r4 = 0.f;
  float4 r5 = 0.f;
  float4 r6 = 0.f;

  r0.xy = c3.x + i.texcoord2.zw;
  r0.zw = r0.xyxy * r0.xyxy;
  r0.zw = r0.zw * -c3.y + c3.z;
  r0.zw = r0.zw * r0.zw;
  r0.zw = r0.zw * -r0.zw + c1.x;
  r1.x = log2(r0.z);
  r1.y = log2(r0.w);
  r0.zw = r1.xyxy * c3.w;
  r1.xz = exp2(r0.z);
  r1.yw = exp2(r0.w);
  r1 = -r0.xyxy * r1;

  r2.x = max(abs(r0.x), abs(r0.y));
  r0.x = r2.x + c5.z;
  r0.x = saturate(r0.x * c5.w);
  r2 = saturate(r1.zwxy * -c5.xxyy + i.texcoord1.xyxy);
  r1 = saturate(r1 * c5.yyxx + i.texcoord1.xyxy);

  r3 = r2.zwxx * c1.xxyy;
  r2 = r2.xyxx * c1.xxyy;
  r2 = tex2Dlod(SceneColorTexture, r2);
  r3 = tex2Dlod(SceneColorTexture, r3);
  r4 = r0.x * c4.xxyx + c4.z;
  r5 = c6;
  r0 = r0.x * r5.xxyz + abs(r5.wwyz);
  r3 = r3.zzxy * r4.zzww;
  r2 = r2.zzxy * r0.zzyw + r3;

  r3 = r1.xyxx * c1.xxyy;
  r1 = r1.zwxx * c1.xxyy;
  r1 = tex2Dlod(SceneColorTexture, r1);
  r3 = tex2Dlod(SceneColorTexture, r3);
  r2 = r3.zzxy * r4 + r2;
  r0 = r1.zzxy * r0 + r2;

  r1 = c1.xxyy * i.texcoord1.xyxx;
  r1 = tex2Dlod(SceneColorTexture, r1);
  r0 += r1.zzxy;

  r1.x = r1.w - MinZ_MaxZRatio.y;
  r1.y = -r1.x + c1.z;
  r1.x = rcp(r1.x);
  r1.x = r1.y >= 0.f ? c1.w : r1.x;
  r2.x = MinZ_MaxZRatio.x;
  r1.y = r2.x * r1.x - DNEColorStrokesBackBlendDistances.x;
  r1.x *= MinZ_MaxZRatio.x;
  r2.x = min(r1.x, c22.w);
  r1.x = rcp(DNEColorStrokesBackBlendDistances.y);
  r1.x = saturate(r1.x * r1.y);
  r1.x = r1.x * r1.x - c1.x;

  r3.x = saturate(DNEColorStrokesLightSrcPos0.z);
  r3.y = saturate(DNEColorStrokesLightSrcPos1.z);
  r1.xy = r3.xy * r1.x + c1.x;
  r3 = c3.x + i.texcoord1.xyxy;
  r4.xy = -DNEColorStrokesLightSrcPos0.xy;
  r4.zw = -DNEColorStrokesLightSrcPos1.xy;
  r3 = r3 * c4.w + r4;
  r4.x = rcp(DNEColorStrokesLightSrcSize0.x);
  r4.y = rcp(DNEColorStrokesLightSrcSize0.y);
  r4.z = rcp(DNEColorStrokesLightSrcSize1.x);
  r4.w = rcp(DNEColorStrokesLightSrcSize1.y);
  r3 *= r4;
  r3.x = dot(r3.xy, r3.xy) + c1.y;
  r3.y = dot(r3.zw, r3.zw) + c1.y;
  r1.zw = saturate(r3.xy * c22.x);
  r1.zw = -r1.zw + c1.x;
  r1.zw *= r1.zw;
  r1.xy *= r1.zwzw;
  r3 = r1.y * DNEColorStrokesLightSrcColor1;
  r1 = DNEColorStrokesLightSrcColor0 * r1.x + r3;
  r0 = r0 * c22.y + r1.w;
  r1 = r1.zzxy + c1.x;

  r2.yz = max(i.texcoord1.zw, HalfResMaskRect.xy);
  r3.xy = min(HalfResMaskRect.zw, r2.yz);
  r3 = tex2D(LowResPostProcessBuffer, r3.xy);
  r4 = r3.zzxy * c22.x;
  r0 = r0 * r1 - r4.yyzw;
  r0 = r3.w * r0 + r4;

  r1.x = dot(r0.zwy, c23.xyz);
  r1.x *= c22.z;
  r1.x = exp2(r1.x);
  r1.x = saturate(r1.x * BloomTintAndScreenBlendThreshold.w);
  r3 = tex2D(FilterColor1Texture, i.texcoord.zw);
  r3 = r3.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
  r3 *= c22.x;
  r0 = r3 * r1.x + r0;

  // Keep the verified scene signal before ImageAdjustments2 for the HDR-only
  // extended-curve/LUT bridge. The SDR branch below remains SM3-equivalent.
  float3 untonemapped_color = float3(r0.z, r0.w, r0.x);
  bool use_hdr_lut_bridge = LIFEISSTRANGE_HDR_PIPELINE > 0.f;
  float hdr_lut_scale = 1.f;
  float3 hdr_lut_neutral_linear = 0.f;

  r1.xyz = r0.zwy * ImageAdjustments2.y + ImageAdjustments2.x;
  r3.z = rcp(r1.x);
  r3.w = rcp(r1.y);
  r3.xy = rcp(r1.z);
  r0 *= r3;

  float3 neutral_sdr = 0.f;
  if (use_hdr_lut_bridge) {
    const float pivot = LIFEISSTRANGE_HDR_CURVE_PIVOT;
    float3 curve_base_input = min(untonemapped_color, pivot.xxx);
    float3 curve_base = curve_base_input
                        / (ImageAdjustments2.x + ImageAdjustments2.y * curve_base_input);
    float pivot_denominator = ImageAdjustments2.x + ImageAdjustments2.y * pivot;
    float pivot_slope = ImageAdjustments2.x / (pivot_denominator * pivot_denominator);
    float3 extended_curve = curve_base + pivot_slope * max(untonemapped_color - pivot.xxx, 0.f);

    // Provisional domain assumption: treat the curve/LUT signal as sRGB-shaped.
    // Compress its decoded value for LUT sampling, then reconstruct it once.
    float3 extended_curve_linear = renodx::color::srgb::DecodeSafe(extended_curve);
    hdr_lut_scale = renodx::tonemap::neutwo::ComputeMaxChannelScale(extended_curve_linear);
    hdr_lut_neutral_linear = extended_curve_linear * hdr_lut_scale;
    // This guard is only on the bounded LUT proxy; HDR is retained for reconstruction.
    float3 lut_proxy = saturate(renodx::color::srgb::EncodeSafe(hdr_lut_neutral_linear));

    // The original SM3 LUT packing uses RGB in r0.z/r0.w/r0.x; r0.y duplicates B.
    r0.z = lut_proxy.r;
    r0.w = lut_proxy.g;
    r0.x = lut_proxy.b;
    r0.y = lut_proxy.b;
    neutral_sdr = hdr_lut_neutral_linear;
  } else {
    // Preserve the original SM3 mul_sat LUT-domain clamp in Vanilla/SDR.
    r0 = saturate(r0);
    neutral_sdr = renodx::color::srgb::DecodeSafe(float3(r0.z, r0.w, r0.x));
  }

  // The SM3 packed-LUT registers hold R/G/B in z/w/x (x duplicates y).
  r1.xyw = r0.xwz * c26.xzy;
  r0.x = frac(r1.x);
  r0.x = -r0.x + r1.x;
  r0.y = r0.y * c5.w - r0.x;
  r1.x = r0.x * c23.w + r1.w;

  r3 = r2.x - DepthDistances;
  r3 = 1.f - step(0.f, r3);
  r4.xy = c1.xy;
  r5 = DepthDistances.xxyz * -r4.yxxx + r2.x;
  r0.xzw = saturate(r2.x * DepthTransition.w - DepthTransition.xyz);
  r2 = r3 * step(0.f, r5);
  r1.z = dot(r2.yzw, c24.xyz);
  r0.x = dot(r2.xyz, r0.xzw);
  r0.z = c1.x + r1.z;
  r2.x = r1.z * c25.x + c25.y;
  r3.x = r0.z * c25.x + c25.y;
  r3.yz = c25.zw;
  r3 = r1.xyxy + r3.yxzx;
  r5 = tex2D(ColorGradingLUT, r3.xy);
  r3 = tex2D(ColorGradingLUT, r3.zw);
  r6 = lerp(r5, r3, r0.y);
  r2.yz = c25.zw;
  r1 = r1.xyxy + r2.yxzx;
  r2 = tex2D(ColorGradingLUT, r1.xy);
  r1 = tex2D(ColorGradingLUT, r1.zw);
  r3 = lerp(r2, r1, r0.y);
  // ps_3_0 lrp r1, r0.x, r6, r3 expands to lerp(r3, r6, r0.x).
  r1 = lerp(r3, r6, r0.x);

  float bypass_lut = step(0.5f, LIFEISSTRANGE_BYPASS_06A2_LUT);
  if (use_hdr_lut_bridge) {
    float3 graded_sdr = renodx::color::srgb::DecodeSafe(r1.xyz);
    graded_sdr = lerp(graded_sdr, neutral_sdr, bypass_lut);
    float3 reconstructed_hdr = renodx::math::DivideSafe(
        graded_sdr,
        hdr_lut_scale.xxx,
        graded_sdr);
    // Keep this transport test neutral: no RenoDX scene ToneMapPass yet.
    r1.xyz = renodx::color::srgb::EncodeSafe(reconstructed_hdr);
  } else if (bypass_lut > 0.f) {
    float3 graded_sdr = renodx::color::srgb::DecodeSafe(r1.xyz);
    r1.xyz = renodx::color::srgb::EncodeSafe(lerp(graded_sdr, neutral_sdr, bypass_lut));
  }
  float force_white_probe = bypass_lut * step(0.5f, LIFEISSTRANGE_FORCE_06A2_WHITE);
  r1.xyz = lerp(r1.xyz, 4.f, force_white_probe);
  r0 = tex2D(DNEVignetTexture, i.texcoord2.zw);
  r0.x = saturate(dot(r0, DNEVignetMaskFactors));
  r0.yzw = DNEVignetColor.xyz - r4.x;
  r0.xyz = r0.x * r0.yzw + c1.x;
  r2.x = c22.x;
  r2.xy = i.texcoord2.zw * r2.x + DNEImageGrainParameter.xy;
  r2 = tex2D(DNEImageGrainTexture, r2.xy);
  r0.w = r2.x * c24.y + c24.w;
  r0.w *= ImageAdjustments1.w;

  float3 output_color = r1.xyz * r0.xyz + r0.w;
  if (!use_hdr_lut_bridge && force_white_probe == 0.f) {
    // Preserve the original DX9 mad_sat in Vanilla/SDR output.
    output_color = saturate(output_color);
  }

  return float4(output_color, r1.w);
}
