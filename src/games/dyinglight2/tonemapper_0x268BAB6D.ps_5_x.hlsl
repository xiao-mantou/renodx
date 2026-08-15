#include "./shared.h"

Texture3D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[2];
}

#define cmp -

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    out float4 o0 : SV_TARGET0) {
  // Keep the scene/input diagnostic colors independent from this later LUT.
  if ((RENODX_DEBUG_MODE > 0.5 && RENODX_DEBUG_MODE < 4.5) ||
      (RENODX_DEBUG_MODE > 9.5 && RENODX_DEBUG_MODE < 11.5) ||
      (RENODX_DEBUG_MODE > 17.5 && RENODX_DEBUG_MODE < 18.5) ||
      (RENODX_DEBUG_MODE > 18.5 && RENODX_DEBUG_MODE < 23.5)) {
    o0 = t0.SampleLevel(s0_s, v1.xy, 0);
    return;
  }

  // This writes immediately before the LUT pass returns. It determines
  // whether this late output target can preserve HDR independently from the
  // earlier scene bridge and its intermediate target.
  if (RENODX_DEBUG_MODE > 8.5 && RENODX_DEBUG_MODE < 9.5) {
    float3 output = 0.0;
    if (v1.x > 0.55 && v1.x < 0.95 && v1.y > 0.82 && v1.y < 0.92) {
      const uint index = min((uint)((v1.x - 0.55) * 10.0), 3u);
      const float levels[4] = {0.25, 1.0, 4.0, 16.0};
      output = levels[index].xxx;
    }
    o0 = float4(output, 1.0);
    return;
  }

  // A fixed HDR-white value injected after this LUT. Comparing this mode
  // against the matching Gamma and final-proxy probes pinpoints whether the
  // static-highlight flicker originates in this pass, the later Gamma pass,
  // or beyond the game shader chain.
  if (RENODX_DEBUG_MODE > 12.5 && RENODX_DEBUG_MODE < 13.5) {
    o0 = float4(6.25, 6.25, 6.25, 1.0);
    return;
  }

  // Original DL2 LUT/color-grade shader, retained verbatim below.
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
  r1.xyzw = t0.SampleLevel(s0_s, v1.xy, 0).xyzw;
  o0.w = r1.w;

  // The scene bridge (0x3E36DA5B) supplies HDR here. The original LUT has
  // an SDR domain and ends in a saturate, so grade its SDR reference only;
  // the HDR magnitude is restored after the original grading code below.
  // The corrected DL2 chain is already Linear BT.709 here. Do not decode it
  // as the legacy sRGB-shaped intermediate or HDR values will be remapped a
  // second time before the LUT bridge.
  float3 input_hdr = max(r1.xyz, 0.0);
  if (RENODX_DEBUG_MODE > 32.5 && RENODX_DEBUG_MODE < 33.5) {
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    const float strengths[4] = {0.0, 0.25, 0.5, 0.75};
    input_hdr = lerp(input_hdr, renodx::color::srgb::DecodeSafe(input_hdr), strengths[quadrant]);
  }
  // Plan B (0xA7F77A42 pattern): build a neutral SDR reference from the HDR
  // signal instead of hard-clipping it, so the vanilla LUT and its
  // smoothstep/contrast/saturation chain operate on a bounded 0..1 neutral
  // reference. The HDR magnitude survives in `input_hdr` and is restored by
  // the three-argument ToneMapPass below. This is the Silksong-style branch:
  // neutral_sdr -> LUT -> graded_color, while untonemapped -> RenoDRT.
  const float3 neutral_sdr = renodx::tonemap::renodrt::NeutralSDR(input_hdr);
  if (RENODX_TONE_MAP_TYPE != 0.0) {
    r1.xyz = neutral_sdr;
  }

  float3 lut_result;
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    // Vanilla LUT path: original sRGB-encoded 3D LUT sample.
    r2.xyz = log2(abs(r1.xyz));
    r2.xyz = float3(0.416666657, 0.416666657, 0.416666657) * r2.xyz;
    r2.xyz = exp2(r2.xyz);
    r2.xyz = r2.xyz * float3(1.05499995, 1.05499995, 1.05499995) + float3(-0.0549999997, -0.0549999997, -0.0549999997);
    r3.xyz = float3(12.9200001, 12.9200001, 12.9200001) * r1.xyz;
    r1.xyz = cmp(float3(0.00313080009, 0.00313080009, 0.00313080009) >= r1.xyz);
    r1.xyz = r1.xyz ? r3.xyz : r2.xyz;
    r1.xyz = r1.xyz * float3(0.96875, 0.96875, 0.96875) + float3(0.015625, 0.015625, 0.015625);
    r1.xyz = t1.SampleLevel(s1_s, r1.xyz, 0).xyz;
    r2.xyz = r1.xyz * float3(0.947867274, 0.947867274, 0.947867274) + float3(0.0521326996, 0.0521326996, 0.0521326996);
    r2.xyz = log2(abs(r2.xyz));
    r2.xyz = float3(2.4000001, 2.4000001, 2.4000001) * r2.xyz;
    r2.xyz = exp2(r2.xyz);
    r3.xyz = float3(0.0773993805, 0.0773993805, 0.0773993805) * r1.xyz;
    r1.xyz = cmp(float3(0.0404499993, 0.0404499993, 0.0404499993) >= r1.xyz);
    lut_result = r1.xyz ? r3.xyz : r2.xyz;
  } else {
    // HDR path: keep the LUT result in the SDR/LUT domain (0..1). NeutralSDR
    // desaturates its input and washed out the grade; use max-channel division
    // instead so chroma survives into the LUT while values stay bounded. Do
    // NOT restore >1 here: graded_sdr stays 0..1 so the vanilla
    // smoothstep/contrast/saturation chain is safe. HDR highlights are
    // recovered from untonemapped by the three-argument ToneMapPass below.
    const float max_channel = max(max(input_hdr.r, max(input_hdr.g, input_hdr.b)), 1.f);
    float3 lut_input = input_hdr / max_channel;
    float3 lut_gamma = renodx::color::srgb::EncodeSafe(lut_input);
    lut_gamma = lut_gamma * float3(0.96875, 0.96875, 0.96875) + float3(0.015625, 0.015625, 0.015625);
    float3 lut_sampled = t1.SampleLevel(s1_s, lut_gamma, 0).xyz;
    lut_sampled = lut_sampled * float3(0.947867274, 0.947867274, 0.947867274) + float3(0.0521326996, 0.0521326996, 0.0521326996);
    lut_result = renodx::color::srgb::DecodeSafe(lut_sampled);
  }
  r1.xyz = lut_result;
  r0.xyz = r1.xyz * r0.xyz;
  r0.w = dot(r0.xyz, float3(0.212599993, 0.715200007, 0.0722000003));
  r0.w = max(9.99999975e-005, r0.w);
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
  const float3 native_lut_grade = o0.rgb;
  float3 upgraded_grade = native_lut_grade;
  float3 stable_grade = native_lut_grade;

  if (RENODX_TONE_MAP_TYPE != 0.0) {
    // Plan B (0xA7F77A42 pattern): the native LUT/color-grade chain (color
    // temp, LUT sample, luminance balance, smoothstep contrast, saturation)
    // produced `native_lut_grade` from the neutral_sdr reference. Feed that
    // as graded_color through the three-argument ToneMapPass so RenoDRT
    // preserves the HDR magnitude from `input_hdr` while keeping the LUT
    // grading. This replaces the old hdr_ratio luminance restore, which only
    // stretched brightness without recovering the clipped hue/saturation.
    upgraded_grade = native_lut_grade;
    stable_grade = native_lut_grade;

    o0.rgb = renodx::draw::ToneMapPass(input_hdr, upgraded_grade, neutral_sdr);
  }

  if (RENODX_DEBUG_MODE > 34.5 && RENODX_DEBUG_MODE < 35.5) {
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    o0.rgb = quadrant == 0u ? native_lut_grade
        : quadrant == 1u ? upgraded_grade
        : quadrant == 2u ? stable_grade
        : o0.rgb;
  }

  // Game is handled by the standard ToneMapPass in 0x3E; the normal path must
  // not scale again here or Game would be applied twice. Modes 40-42 keep the
  // post-LUT Game diagnostics only, for A/B against the standard path.
  if (RENODX_DEBUG_MODE > 39.5 && RENODX_DEBUG_MODE < 45.5) {
    const float game_scale = max(RENODX_DIFFUSE_WHITE_NITS / 203.0, 0.01);
    const float luminance = renodx::color::y::from::BT709(max(o0.rgb, 0.0));
    float game_weight = 1.0 - smoothstep(1.0, 2.0, luminance);
    if (RENODX_DEBUG_MODE > 40.5 && RENODX_DEBUG_MODE < 41.5) {
      game_weight = 1.0 - smoothstep(1.0, 4.0, luminance);
    } else if (RENODX_DEBUG_MODE > 41.5 && RENODX_DEBUG_MODE < 42.5) {
      game_weight = 1.0;
    }
    o0.rgb *= lerp(1.0, game_scale, game_weight);
  }

  // Compare luminance-preserving chroma recovery after the game's LUT. This
  // runs only in the manual DLSS Off diagnostic mode.
  if (RENODX_DEBUG_MODE > 28.5 && RENODX_DEBUG_MODE < 29.5) {
    const float luminance = renodx::color::y::from::BT709(max(o0.rgb, 0.0));
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    const float strengths[4] = {1.0, 1.1, 1.2, 1.3};
    o0.rgb = lerp(luminance.xxx, o0.rgb, strengths[quadrant]);
  }

  float3 stage_probe;
  if (Dl2RenderLuminanceStageProbe(v1.xy, 2.0, stage_probe)
      || Dl2RenderLuminanceStageProbe(v1.xy, 3.0, stage_probe)) {
    o0 = float4(stage_probe, 1.0);
  }
  return;
}
