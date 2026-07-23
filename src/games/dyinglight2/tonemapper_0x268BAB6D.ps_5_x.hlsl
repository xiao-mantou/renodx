#include "./shared.h"

Texture2D<float4> t0 : register(t0);
Texture3D<float4> lutTexture : register(t1);

SamplerState s1_s : register(s1);

SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[2];
}

// 3Dmigoto declarations
#define cmp -

// Debug false-color visualization for HDR range analysis.
// Uses log2-scale mapping to clearly distinguish SDR (<=1.0) from HDR (>1.0).
//   near-black -> blue -> cyan (SDR) -> green (1.0 boundary) -> yellow -> red -> white
float3 DebugFalseColor(float value) {
  float v = max(0, value);
  float log_v = log2(max(0.001, v));
  float3 col;
  [flatten] if (log_v < -3.0) {
    col = float3(0, 0, 0.1);
  } else if (log_v < 0.0) {
    float t = (log_v + 3.0) / 3.0;
    col = lerp(float3(0, 0, 0.2), float3(0, 0.5, 1.0), t);
  } else if (log_v < 1.0) {
    float t = log_v;
    col = lerp(float3(0, 0.5, 1.0), float3(0, 1.0, 0.3), t);
  } else if (log_v < 2.0) {
    float t = log_v - 1.0;
    col = lerp(float3(0, 1.0, 0.3), float3(1.0, 1.0, 0), t);
  } else if (log_v < 3.0) {
    float t = log_v - 2.0;
    col = lerp(float3(1.0, 1.0, 0), float3(1.0, 0, 0), t);
  } else {
    float t = saturate(log_v - 3.0);
    col = lerp(float3(1.0, 0, 0), float3(1.0, 1.0, 1.0), t);
  }
  return col;
}

void main(float4 v0: SV_POSITION0, float4 v1: TEXCOORD0, out float4 o0: SV_TARGET0) {
  float4 r0, r1, r2, r3;

  r0.x = -1000 + cb0[1].y;
  r0.x = saturate(-0.00100000005 * r0.x);
  r0.y = r0.x * -2 + 3;
  r0.x = r0.x * r0.x;
  r0.x = r0.y * r0.x;
  r0.y = cmp(6500 >= cb0[1].y);
  r1.xyz = r0.yyy ? float3(0, 1669.58032, 2575.28271) : float3(-2666.34741, -2173.10132, 2575.28271);
  r1.xyz = cb0[1].yyy + r1.xyz;
  r2.xyz = r0.yyy ? float3(0, -2902.19556, -8257.7998) : float3(1745.04248, 1216.61682, -8257.7998);
  r0.yzw = r0.yyy ? float3(1, 1.33026743, 1.89937544) : float3(0.509953916, 0.703812003, 1.89937544);
  r1.xyz = r2.xyz / r1.xyz;
  r0.yzw = saturate(r1.xyz + r0.yzw);
  r1.xyz = float3(1, 1, 1) + -r0.yzw;
  r0.xyz = r0.xxx * r1.xyz + r0.yzw;
  r1 = t0.SampleLevel(s0_s, v1.xy, 0);

  const float3 untonemapped = r1.xyz;
  float3 neutral_sdr = renodx::tonemap::renodrt::NeutralSDR(r1.xyz);

  if (CUSTOM_AUTO_EXPOSURE != 1.f) {
    r1.xyz = neutral_sdr;
  }
  float3 signs = 1.f;

  // Lut sample
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    r2.xyz = log2(abs(r1.xyz));
    r2.xyz = float3(0.416666657, 0.416666657, 0.416666657) * r2.xyz;
    r2.xyz = exp2(r2.xyz);
    r2.xyz = r2.xyz * float3(1.05499995, 1.05499995, 1.05499995) + float3(-0.0549999997, -0.0549999997, -0.0549999997);
    r3.xyz = float3(12.9200001, 12.9200001, 12.9200001) * r1.xyz;
    r1.xyz = cmp(float3(0.00313080009, 0.00313080009, 0.00313080009) >= r1.xyz);
    r1.xyz = r1.xyz ? r3.xyz : r2.xyz;
    r1.xyz = r1.xyz * float3(0.96875, 0.96875, 0.96875) + float3(0.015625, 0.015625, 0.015625);
    r1.xyz = lutTexture.SampleLevel(s1_s, r1.xyz, 0).xyz;
    r2.xyz = r1.xyz * float3(0.947867274, 0.947867274, 0.947867274) + float3(0.0521326996, 0.0521326996, 0.0521326996);
    r2.xyz = log2(abs(r2.xyz));
    r2.xyz = float3(2.4000001, 2.4000001, 2.4000001) * r2.xyz;
    r2.xyz = exp2(r2.xyz);
    r3.xyz = float3(0.0773993805, 0.0773993805, 0.0773993805) * r1.xyz;
    r1.xyz = cmp(float3(0.0404499993, 0.0404499993, 0.0404499993) >= r1.xyz);
    r1.xyz = r1.xyz ? r3.xyz : r2.xyz;
  } else {
    renodx::lut::Config lut_config = renodx::lut::config::Create();
    lut_config.tetrahedral = true;
    lut_config.type_input = renodx::lut::config::type::SRGB;
    lut_config.type_output = renodx::lut::config::type::SRGB;
    lut_config.scaling = CUSTOM_LUT_SCALING;
    lut_config.lut_sampler = s1_s;
    r1.xyz = renodx::lut::Sample(lutTexture, lut_config, r1.xyz);
    signs = renodx::math::Sign(r1.xyz);
    r1.xyz = abs(r1.xyz);
  }
  r0.xyz = r1.xyz * r0.xyz;
  r0.w = dot(r0.xyz, float3(0.212599993, 0.715200007, 0.0722000003));
  r0.w = max(9.99999975e-05, r0.w);
  r1.w = dot(r1.xyz, float3(0.212599993, 0.715200007, 0.0722000003));
  r0.w = r1.w / r0.w;
  r0.xyz = r0.xyz * r0.www;
  r0.w = cmp(cb0[1].y != 6500.000000);
  r0.xyz = r0.www ? r0.xyz : r1.xyz;
  r1.xyz = saturate(r0.xyz);
  r2.xyz = r1.xyz * float3(-2, -2, -2) + float3(3, 3, 3);
  r1.xyz = r1.xyz * r1.xyz;
  r1.xyz = r2.xyz * r1.xyz + -r0.xyz;
  r0.xyz = cb0[0].www * r1.xyz + r0.xyz;
  r0.w = saturate(dot(float3(0.212500006, 0.715399981, 0.0720999986), r0.xyz));
  r0.xyz = r0.xyz + -r0.www;
  r0.xyz = cb0[1].xxx * r0.xyz + r0.www;
  r1.xy = cmp(cb0[0].yx < abs(v1.wz));
  r0.w = (int)r1.y | (int)r1.x;
  r1.xyz = r0.www ? float3(0, 0, 0) : r0.xyz;
  r0.w = cmp(0 < cb0[0].z);

  o0.xyz = r0.www ? r1.xyz : r0.xyz;

  o0.rgb *= signs;

  // Debug visualization: bypass tonemapping, show raw values as false color
  // Mode 1: t0 (untonemapped) — shows actual HDR scene values (critical for Peak diagnosis)
  // Mode 2: neutral_sdr — shows NeutralSDR extraction
  // Mode 3: graded_sdr — shows o0.rgb after LUT grading, before ToneMapPass
  if (RENODX_DEBUG_MODE > 0.5f) {
    float3 debug_input;
    [flatten] if (RENODX_DEBUG_MODE < 1.5f) {
      debug_input = untonemapped;
    } else if (RENODX_DEBUG_MODE < 2.5f) {
      debug_input = neutral_sdr;
    } else {
      debug_input = o0.rgb;
    }
    // The t0 probe must detect HDR in any channel. BT.709 luminance can remain
    // below one for saturated highlights even when an input component exceeds one.
    float debug_range = RENODX_DEBUG_MODE < 1.5f
        ? max(debug_input.r, max(debug_input.g, debug_input.b))
        : renodx::color::y::from::BT709(max(0, debug_input));
    o0.rgb = DebugFalseColor(debug_range);
    o0.rgb = renodx::draw::RenderIntermediatePass(o0.rgb);
    o0.w = 1;
    return;
  }

  if (RENODX_TONE_MAP_TYPE == 0.f) {
    o0.rgb = saturate(o0.rgb);
  } else {
    o0.rgb = renodx::draw::ToneMapPass(untonemapped, o0.rgb, neutral_sdr);
  }

  o0.rgb = renodx::draw::RenderIntermediatePass(o0.rgb);
  o0.w = 1;
  return;
}
