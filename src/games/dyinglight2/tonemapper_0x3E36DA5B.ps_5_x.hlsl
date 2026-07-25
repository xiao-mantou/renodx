#include "./shared.h"

Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[2];
}

float3 DebugFalseColor(float value) {
  const float v = max(0.0, value);
  const float log_v = log2(max(0.001, v));
  if (log_v < 0.0) {
    return lerp(float3(0.0, 0.0, 0.2), float3(0.0, 1.0, 0.3), saturate((log_v + 3.0) / 3.0));
  }
  if (log_v < 1.0) {
    return lerp(float3(0.0, 1.0, 0.3), float3(1.0, 1.0, 0.0), log_v);
  }
  if (log_v < 2.0) {
    return lerp(float3(1.0, 1.0, 0.0), float3(1.0, 0.0, 0.0), log_v - 1.0);
  }
  return lerp(float3(1.0, 0.0, 0.0), float3(1.0, 1.0, 1.0), saturate(log_v - 2.0));
}

// Exact DL2 SDR curve recovered from the original shader. This is evaluated
// with the original t1 exposure and passed to RenoDRT as the SDR reference.
float3 ApplyDL2SDRCurve(float3 color, float4 curve0, float4 curve1) {
  const float3 a = curve0.xxx * color + curve0.yyy;
  const float3 b = curve0.zzz * color + curve0.www;
  return saturate((a * color) / (b * color + curve1.xxx));
}

void main(
    float4 v0 : SV_POSITION0,
    linear noperspective float2 v1 : TEXCOORD0,
    out float4 o0 : SV_TARGET0) {
  const float4 source = t0.SampleLevel(s0_s, v1.xy, 0);
  const float exposure = t1.SampleLevel(s0_s, float2(0.0, 0.0), 0).x;
  const float3 scene_linear = source.rgb * 0.6;
  const float3 game_exposed = scene_linear * exposure;
  const float3 vanilla = ApplyDL2SDRCurve(game_exposed, cb0[0], cb0[1]);

  // Keep DL2's full automatic exposure for shadow and midtone intent. Only
  // bright raw scene values gradually use a protected exposure, so the
  // original SDR reference remains intact while HDR headroom survives.
  const float exposure_min = min(max(RENODX_AUTO_EXPOSURE_MIN, 0.01),
                                 max(RENODX_AUTO_EXPOSURE_MAX, 0.01));
  const float exposure_max = max(max(RENODX_AUTO_EXPOSURE_MIN, 0.01),
                                 max(RENODX_AUTO_EXPOSURE_MAX, 0.01));
  // Use a linear blend between unexposed HDR and clamped game exposure.
  // The previous logarithmic exponent was mathematically continuous, but
  // visually collapsed most of the slider into its two endpoints in DL2.
  const float retained_exposure = clamp(max(exposure, 0.001), exposure_min, exposure_max);
  const float protected_exposure = lerp(1.0, retained_exposure, saturate(CUSTOM_AUTO_EXPOSURE));
  const float protection_start = min(max(RENODX_HDR_EXPOSURE_PROTECTION_START, 0.0),
                                     max(RENODX_HDR_EXPOSURE_PROTECTION_END, 0.001));
  const float protection_end = max(max(RENODX_HDR_EXPOSURE_PROTECTION_START, 0.0),
                                   max(RENODX_HDR_EXPOSURE_PROTECTION_END, protection_start + 0.001));
  const float scene_luminance = renodx::color::y::from::BT709(max(scene_linear, 0.0));
  const float protection = smoothstep(protection_start, protection_end, scene_luminance);
  const float adaptive_exposure = lerp(exposure, protected_exposure, protection);
  const float3 untonemapped = scene_linear * adaptive_exposure;
  const float3 neutral_sdr = renodx::tonemap::renodrt::NeutralSDR(untonemapped);

  if (RENODX_DEBUG_MODE > 0.5) {
    // Mode 4 runs the actual RenoDRT curve first. It verifies Peak and White
    // Clip before the game's later composite passes.
    const float3 renodrt_output = renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr);
    const float3 debug_input = RENODX_DEBUG_MODE < 1.5 ? untonemapped
        : RENODX_DEBUG_MODE < 2.5 ? neutral_sdr
        : RENODX_DEBUG_MODE < 3.5 ? vanilla
        : renodrt_output;
    const float debug_range = RENODX_DEBUG_MODE < 1.5
        ? max(debug_input.r, max(debug_input.g, debug_input.b))
        : renodx::color::y::from::BT709(max(0.0, debug_input));
    o0 = float4(renodx::draw::RenderIntermediatePass(DebugFalseColor(debug_range)), 1.0);
    return;
  }

  o0.rgb = RENODX_TONE_MAP_TYPE == 0.0
      ? vanilla
      : renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr);
  o0.rgb = renodx::draw::RenderIntermediatePass(o0.rgb);
  o0.a = source.a;
}
