#include "./shared.h"

float4 BloomTintAndScreenBlendThreshold : register(c0);
float4 MinZ_MaxZRatio : register(c2);
float4 ImageAdjustments1 : register(c7);
float4 ImageAdjustments2 : register(c8);
float4 HalfResMaskRect : register(c9);
float4 DNEImageGrainParameter : register(c17);
float4 DNEVignetColor : register(c18);
float4 DNEVignetMaskFactors : register(c19);
sampler2D SceneColorTexture : register(s0);
sampler2D FilterColor1Texture : register(s1);
sampler2D DNEImageGrainTexture : register(s2);
sampler2D DNEVignetTexture : register(s3);
sampler2D ColorGradingLUT : register(s4);
sampler2D LowResPostProcessBuffer : register(s5);

struct PS_IN {
  float4 texcoord : TEXCOORD;
  float4 texcoord1 : TEXCOORD1;
  float4 texcoord2 : TEXCOORD2;
};

float4 SampleSceneLod(float2 uv, float lod) {
  return tex2Dlod(SceneColorTexture, float4(uv, 0.f, lod));
}

float3 ApplyDisplayMap(float3 hdr_color, float3 vanilla_sdr) {
  float3 sdr_linear = renodx::color::srgb::DecodeSafe(vanilla_sdr);
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    return saturate(sdr_linear);
  }

  return renodx::draw::ToneMapPass(hdr_color, sdr_linear);
}

float4 main(PS_IN i) : COLOR {
  float2 radial = i.texcoord2.zw - 0.5f;
  float2 radial_sq = radial * radial;
  radial_sq = radial_sq * -2.828f + 1.f;
  radial_sq = radial_sq * radial_sq;
  radial_sq = radial_sq * -radial_sq + 1.f;

  float2 radial_exp = exp2(log2(radial_sq) * 1.5f);
  float4 radial_weight = float4(radial_exp.x, radial_exp.y, radial_exp.x, radial_exp.y);
  radial_weight *= -float4(radial.x, radial.y, radial.x, radial.y);

  float radial_mask = saturate((max(abs(radial.x), abs(radial.y)) - 0.36f) * 15.f);
  float4 tap_a = saturate(radial_weight.zwxy * float4(-0.007f, -0.007f, -0.0035f, -0.0035f) + i.texcoord1.xyxy);
  float4 tap_b = saturate(radial_weight * float4(0.0035f, 0.0035f, 0.007f, 0.007f) + i.texcoord1.xyxy);

  float4 sample_a = SampleSceneLod(tap_a.xy, tap_a.w);
  float4 sample_b = SampleSceneLod(tap_b.xy, tap_b.w);

  float4 sample_weight_a = radial_mask * float4(0.334f, 0.f, -0.333f, 0.f) + float4(0.666f, 0.666f, 0.f, 0.666f);
  float4 sample_weight_b = radial_mask * float4(0.334f, 0.334f, -0.666f, 0.334f) + 0.666f;
  float4 reconstructed = sample_a.zzxy * sample_weight_a.zzyw;
  reconstructed += sample_b.zzxy * sample_weight_b.zzww;

  float4 tap_c = float4(radial_weight.xy, 0.f, 0.f);
  float4 tap_d = float4(radial_weight.zw, 0.f, 0.f);
  float4 sample_c = SampleSceneLod(tap_c.xy, 0.f);
  float4 sample_d = SampleSceneLod(tap_d.xy, 0.f);
  reconstructed += sample_d.zzxy * sample_weight_b;
  reconstructed += sample_c.zzxy * sample_weight_a;
  reconstructed += SampleSceneLod(i.texcoord1.xy, 0.f).zzxy;

  float3 hdr_color = reconstructed.zwy;

  float2 low_res_uv = min(HalfResMaskRect.zw, max(i.texcoord1.zw, HalfResMaskRect.xy));
  float4 low_res = tex2D(LowResPostProcessBuffer, low_res_uv);
  float4 low_res_scaled = low_res.zzxy * 4.f;
  hdr_color = lerp(low_res.xyz * 4.f, hdr_color, low_res.w);
  hdr_color = hdr_color * (1.f + 0.33333f) - low_res_scaled.yzw;
  hdr_color = low_res.w * hdr_color + low_res_scaled.xyz;

  float bloom_weight = saturate(exp2(-3.f * dot(hdr_color, float3(0.3f, 0.59f, 0.11f))) * BloomTintAndScreenBlendThreshold.w);
  float4 filter = tex2D(FilterColor1Texture, i.texcoord.zw);
  float3 bloom = filter.zzx * BloomTintAndScreenBlendThreshold.zzx * 4.f;
  hdr_color += bloom * bloom_weight;

  float3 hdr_scale = renodx::tonemap::neutwo::ComputeMaxChannelScale(hdr_color);
  float3 pre_lut = hdr_color * lerp(1.f, hdr_scale, step(0.5f, RENODX_TONE_MAP_TYPE));
  float3 lut_input = saturate(pre_lut * rcp(max(ImageAdjustments2.xyz, 0.00001f)) + ImageAdjustments2.xxx);

  float3 lut_coord = float3(lut_input.b, lut_input.r, lut_input.g);
  lut_coord = lut_coord * float3(14.9999f, 14.9999f, 14.9999f);
  float lut_slice = floor(lut_coord.x);
  float lut_fraction = lut_coord.x - lut_slice;
  float2 lut_uv0 = float2(lut_slice * 0.0625f + lut_input.a, lut_coord.y * 0.0625f + lut_input.a);
  float2 lut_uv1 = float2((lut_slice + 1.f) * 0.0625f + lut_input.a, lut_coord.y * 0.0625f + lut_input.a);
  float3 vanilla_sdr = lerp(tex2D(ColorGradingLUT, lut_uv0).rgb, tex2D(ColorGradingLUT, lut_uv1).rgb, lut_fraction);

  float3 output_color = ApplyDisplayMap(hdr_color, vanilla_sdr);
  float vignette = saturate(dot(tex2D(DNEVignetTexture, i.texcoord2.zw), DNEVignetMaskFactors));
  float3 vignette_color = 1.f + vignette * (DNEVignetColor.rgb - 1.f);
  output_color *= vignette_color;

  float grain = tex2D(DNEImageGrainTexture, i.texcoord2.zw * 4.f + DNEImageGrainParameter.xy).r * 2.f - 1.f;
  output_color += grain * ImageAdjustments1.w;
  output_color = renodx::draw::RenderIntermediatePass(output_color);

  return float4(output_color, 1.f);
}
