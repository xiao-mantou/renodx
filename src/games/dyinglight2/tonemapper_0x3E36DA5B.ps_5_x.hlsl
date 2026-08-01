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

float3 DebugChroma(float3 color) {
  const float peak = max(max(color.r, color.g), max(color.b, 0.0001));
  return saturate(color / peak);
}

// DL2's Linear BT.709 intermediate uses a fixed physical unit of
// 1.0 = 203 nits. RenoDX ToneMapPass returns values relative to the selected
// Game Brightness, so convert that relative output back into DL2's fixed unit.
// Without this conversion, changing Game Brightness only changes Peak/Game
// inside the curve: lowering Game incorrectly brightens the image and raising
// it compresses/desaturates the midrange.
float3 ScaleToneMappedScene(float3 color) {
  return color * (RENODX_DIFFUSE_WHITE_NITS / 203.0);
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

  float3 stage_probe;
  if (Dl2RenderLuminanceStageProbe(v1.xy, 1.0, stage_probe)) {
    o0 = float4(stage_probe, 1.0);
    return;
  }

  // Mode 5 belongs to the swapchain proxy. Let the scene pass through so it
  // can prove whether the final HDR output path is actually being executed.
  if (RENODX_DEBUG_MODE > 0.5 && RENODX_DEBUG_MODE < 4.5) {
    // Mode 4 runs the actual RenoDRT curve first. It verifies Peak and White
    // Clip before the game's later composite passes.
    const float3 renodrt_output = ScaleToneMappedScene(
        renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr));
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

  // These modes separate the game's source scene buffer from its scalar
  // auto exposure. They deliberately precede the 0.6 scene calibration and
  // every RenoDX exposure/highlight operation.
  if (RENODX_DEBUG_MODE > 9.5 && RENODX_DEBUG_MODE < 10.5) {
    const float source_range = max(source.r, max(source.g, source.b));
    o0 = float4(renodx::draw::RenderIntermediatePass(DebugFalseColor(source_range)), 1.0);
    return;
  }
  if (RENODX_DEBUG_MODE > 10.5 && RENODX_DEBUG_MODE < 11.5) {
    o0 = float4(renodx::draw::RenderIntermediatePass(DebugFalseColor(exposure)), 1.0);
    return;
  }
  // Normalize out intensity so DLSS modes can be compared for t0 chroma
  // alone. This is a visual fallback when a driver keeps its root-CBV upload
  // buffer persistently mapped outside the generic constant-buffer cache.
  if (RENODX_DEBUG_MODE > 18.5 && RENODX_DEBUG_MODE < 19.5) {
    o0 = float4(renodx::draw::RenderIntermediatePass(DebugChroma(source.rgb)), 1.0);
    return;
  }
  // These two probes distinguish the original DL2 SDR reference from the
  // HDR bridge. If Vanilla remains invariant across DLSS modes but RenoDRT
  // does not, the correction belongs in the bridge's color anchor.
  if (RENODX_DEBUG_MODE > 19.5 && RENODX_DEBUG_MODE < 21.5) {
    const float3 debug_color = RENODX_DEBUG_MODE < 20.5
        ? vanilla
        : ScaleToneMappedScene(renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr));
    o0 = float4(renodx::draw::RenderIntermediatePass(DebugChroma(debug_color)), 1.0);
    return;
  }
  // Direct counterparts to the chroma probes above. These retain luminance
  // and saturation so a tiny input chroma delta is not artificially expanded.
  if (RENODX_DEBUG_MODE > 21.5 && RENODX_DEBUG_MODE < 23.5) {
    const float3 debug_color = RENODX_DEBUG_MODE < 22.5
        ? vanilla
        : ScaleToneMappedScene(renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr));
    o0 = float4(renodx::draw::RenderIntermediatePass(debug_color), 1.0);
    return;
  }

  // Stability probe: aim the suspected flickering highlight at screen center.
  // Each column samples one scalar stage from the same center pixel; the top
  // row bypasses the late Gamma pass and the lower row runs through it. The
  // last column is a fixed HDR reference that exposes any later instability.
  if (RENODX_DEBUG_MODE > 17.5 && RENODX_DEBUG_MODE < 18.5) {
    const float2 probe_uv = float2(0.5, 0.5);
    const float4 probe_source = t0.SampleLevel(s0_s, probe_uv, 0);
    const float probe_exposure = t1.SampleLevel(s0_s, float2(0.0, 0.0), 0).x;
    const float3 probe_scene = probe_source.rgb * 0.6;
    const float probe_raw_range = max(probe_scene.r, max(probe_scene.g, probe_scene.b));
    const float probe_exposed_range = max(probe_scene.r * probe_exposure,
                                           max(probe_scene.g * probe_exposure, probe_scene.b * probe_exposure));
    const uint column = min((uint)(v1.x * 4.0), 3u);
    const float3 probe_output = column == 0u ? DebugFalseColor(probe_raw_range)
        : column == 1u ? DebugFalseColor(probe_exposure)
        : column == 2u ? DebugFalseColor(probe_exposed_range)
        : float3(6.25, 6.25, 6.25);
    o0 = float4(renodx::draw::RenderIntermediatePass(probe_output), 1.0);
    return;
  }

  // Four known linear values travel through every later DL2 composite pass.
  // With Game Brightness at 203 nits, their expected unclipped output is
  // approximately 51, 203, 812, and 3248 nits respectively.
  if (RENODX_DEBUG_MODE > 6.5 && RENODX_DEBUG_MODE < 7.5) {
    float3 output = vanilla;
    if (v1.x > 0.55 && v1.x < 0.95 && v1.y > 0.82 && v1.y < 0.92) {
      const uint index = min((uint)((v1.x - 0.55) * 10.0), 3u);
      const float levels[4] = {0.25, 1.0, 4.0, 16.0};
      output = levels[index].xxx;
    }
    o0.rgb = renodx::draw::RenderIntermediatePass(output);
    o0.a = 1.0;
    return;
  }

  // Same ladder as mode 7, but bypasses RenoDX's intermediate encoding.
  // Comparing modes 7 and 8 isolates an encoding/target-format clamp from
  // a later DL2 composite clamp in one game session.
  if (RENODX_DEBUG_MODE > 7.5 && RENODX_DEBUG_MODE < 8.5) {
    float3 output = vanilla;
    if (v1.x > 0.55 && v1.x < 0.95 && v1.y > 0.82 && v1.y < 0.92) {
      const uint index = min((uint)((v1.x - 0.55) * 10.0), 3u);
      const float levels[4] = {0.25, 1.0, 4.0, 16.0};
      output = levels[index].xxx;
    }
    o0.rgb = output;
    o0.a = 1.0;
    return;
  }

  // Legacy A/B: the old bridge encoded an sRGB-shaped intermediate here. The
  // native menu/UI composite and final proxy are linear BT.709, so this path is
  // retained only to compare against the corrected default below.
  if (RENODX_DEBUG_MODE > 25.5 && RENODX_DEBUG_MODE < 26.5) {
    o0.rgb = RENODX_TONE_MAP_TYPE == 0.0
        ? vanilla
        : ScaleToneMappedScene(renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr));
    o0.rgb = renodx::draw::RenderIntermediatePass(o0.rgb);
    o0.a = source.a;
    return;
  }

  // DLSS Off color grid. All quadrants start from the same tone-mapped HDR
  // value so this compares only the missing color treatment, not exposure or
  // peak brightness. Top-left is the current path; top-right blends half of
  // the legacy intermediate encoding; bottom-left/right apply progressively
  // stronger linear-light chroma recovery while preserving BT.709 luminance.
  if (RENODX_DEBUG_MODE > 27.5 && RENODX_DEBUG_MODE < 28.5) {
    const float3 current = RENODX_TONE_MAP_TYPE == 0.0
        ? vanilla
        : ScaleToneMappedScene(renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr));
    const float luminance = renodx::color::y::from::BT709(max(current, 0.0));
    const float3 chroma_115 = lerp(luminance.xxx, current, 1.15);
    const float3 chroma_130 = lerp(luminance.xxx, current, 1.30);
    const bool right = v1.x >= 0.5;
    const bool bottom = v1.y >= 0.5;
    o0.rgb = !bottom && !right ? current
        : !bottom && right ? lerp(current, renodx::draw::RenderIntermediatePass(current), 0.5)
        : bottom && !right ? chroma_115
        : chroma_130;
    o0.a = source.a;
    return;
  }

  // This reaches the game's subsequent composite passes, unlike the output
  // probe in the swapchain proxy. With Peak=500 and Game=100, the scene area
  // should measure 500 nits if no later pass normalizes it back to SDR.
  const bool downstream_color_grid = RENODX_DEBUG_MODE > 28.5 && RENODX_DEBUG_MODE < 31.5;
  if (RENODX_DEBUG_MODE > 5.5 && !downstream_color_grid) {
    const float target_white = RENODX_PEAK_WHITE_NITS / 203.0;
    o0.rgb = renodx::draw::RenderIntermediatePass(float3(target_white, target_white, target_white));
    o0.a = 1.0;
    return;
  }

  o0.rgb = RENODX_TONE_MAP_TYPE == 0.0
      ? vanilla
      : ScaleToneMappedScene(renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr));
  o0.a = source.a;
}
