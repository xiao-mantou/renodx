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

void main(
    float4 v0 : SV_POSITION0,
    linear noperspective float2 v1 : TEXCOORD0,
    out float4 o0 : SV_TARGET0) {
  const float4 source = t0.SampleLevel(s0_s, v1.xy, 0);
  const float exposure = t1.SampleLevel(s0_s, float2(0.0, 0.0), 0).x;
  const float3 scene_linear = source.rgb * 0.6;
  // Keep the game's global exposure direction, but compress its magnitude in
  // log space. Full t1 multiplication normalizes outdoor HDR back to SDR;
  // ignoring it leaves indoor midtones too dark. A global scalar preserves
  // scene consistency and leaves highlight rolloff to RenoDRT.
  const float exposure_log = log2(max(exposure, 0.001));
  const float preserved_exposure = exp2(clamp(exposure_log * 0.35, -1.0, 1.0));
  const float3 game_exposed = scene_linear * exposure;
  const float3 preserved_exposed = scene_linear * preserved_exposure;
  // The game's final auto-exposure normalizes bright outdoor content to SDR
  // before its curve. Preserve the raw scene signal for RenoDX so Peak
  // Brightness can map that headroom; Vanilla retains the original exposure.
  const float3 untonemapped = RENODX_TONE_MAP_TYPE == 0.0
      ? game_exposed
      : preserved_exposed;
  const float3 neutral_sdr = renodx::tonemap::renodrt::NeutralSDR(untonemapped);

  // Preserve the game's original curve for Vanilla mode.
  const float3 a = cb0[0].xxx * untonemapped + cb0[0].yyy;
  const float3 b = cb0[0].zzz * untonemapped + cb0[0].www;
  const float3 vanilla = saturate((a * untonemapped) / (b * untonemapped + cb0[1].xxx));

  if (RENODX_DEBUG_MODE > 0.5) {
    // Mode 4 runs the actual RenoDRT curve first. It verifies Peak and White
    // Clip before the game's later composite passes.
    const float3 renodrt_output = renodx::draw::ToneMapPass(untonemapped, neutral_sdr, neutral_sdr);
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

  const float3 graded = RENODX_TONE_MAP_TYPE == 0.0
      ? vanilla
      : neutral_sdr;
  o0.rgb = RENODX_TONE_MAP_TYPE == 0.0
      ? vanilla
      : renodx::draw::ToneMapPass(untonemapped, graded, neutral_sdr);
  o0.rgb = renodx::draw::RenderIntermediatePass(o0.rgb);
  o0.a = source.a;
}
