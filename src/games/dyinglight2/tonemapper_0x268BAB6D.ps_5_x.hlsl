#include "./shared.h"

Texture3D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[2];
}

#define cmp -

// DL2's SDR curve, same as 0x3E. In the HDR path input_hdr equals the
// untonemapped scene (scene_linear * exposure), so applying this gives exactly
// the vanilla SDR curve output. Using it as the LUT input makes HDR's LUT
// grade match vanilla's, which fixes the washed-out low/mid region.
float3 ApplyDL2SDRCurve(float3 color, float4 curve0, float4 curve1) {
  const float3 a = curve0.xxx * color + curve0.yyy;
  const float3 b = curve0.zzz * color + curve0.www;
  return saturate((a * color) / (b * color + curve1.xxx));
}

// H1 HDR LUT reference: preserve the exact DL2 curve below its SDR ceiling,
// but leave the upper result available for one shared pixel-local compression
// scale. The lower clamp matches the non-negative scene domain; there is no
// upper saturate here because HDR highlights must remain recoverable.
float3 ApplyDL2SDRCurveExtended(float3 color, float4 curve0, float4 curve1) {
  const float3 a = curve0.xxx * color + curve0.yyy;
  const float3 b = curve0.zzz * color + curve0.www;
  return max((a * color) / (b * color + curve1.xxx), 0.0);
}

// 5x7 pixel glyphs (bit k set = column k from the left, 1 = lit).
// Index 0-9 = digits, 10 = decimal point.
uint DebugGlyph(int digit, int row) {
  const int k = clamp(digit, 0, 10);
  const uint patterns[11][7] = {
      {31, 17, 17, 17, 17, 17, 31},  // 0
      {4, 12, 4, 4, 4, 4, 14},       // 1
      {31, 16, 16, 31, 1, 1, 31},     // 2
      {31, 16, 16, 31, 16, 16, 31},   // 3
      {17, 17, 17, 31, 16, 16, 16},   // 4
      {31, 1, 1, 31, 16, 16, 31},     // 5
      {31, 1, 1, 31, 17, 17, 31},     // 6
      {31, 16, 16, 16, 16, 16, 16},   // 7
      {31, 17, 17, 31, 17, 17, 31},   // 8
      {31, 17, 17, 31, 16, 16, 31},   // 9
      {0, 0, 0, 0, 0, 4, 4},         // .
  };
  return patterns[k][clamp(row, 0, 6)];
}

// Renders a small integer/decimal label at (origin_x, origin_y) using the
// 5x7 glyphs, so diagnostics can annotate measured values. Returns white if
// this pixel is part of the label, black otherwise.
float3 DebugRenderLabel(float2 uv, float origin_x, float origin_y, float value, float cell = 0.012f) {
  const int whole = clamp((int)value, 0, 99);
  const int tenth = clamp((int)((value - floor(value)) * 10.0 + 0.5), 0, 9);
  const int hundredth = value < 1.0
                            ? clamp((int)((value - floor(value)) * 100.0 + 0.5) % 10, 0, 9)
                            : -1;
  int chars[6] = {-1, -1, -1, -1, -1, -1};
  int count = 0;
  if (whole >= 10) {
    chars[count++] = whole / 10;
  }
  chars[count++] = whole % 10;
  if (value < 1.0) {
    chars[count++] = 10;  // decimal point
    chars[count++] = tenth;
    if (hundredth >= 0) chars[count++] = hundredth;
  }
  for (int i = 0; i < count; ++i) {
    const float x0 = origin_x + i * (5.0 * cell + 0.008);
    const float x1 = x0 + 5.0 * cell;
    if (uv.x < x0 || uv.x >= x1) continue;
    const int col = (int)((uv.x - x0) / cell);
    const int row = (int)((uv.y - origin_y) / (cell * 1.4f));
    if (row < 0 || row > 6) continue;
    if (((DebugGlyph(chars[i], row) >> col) & 1u) != 0u) return float3(1.0, 1.0, 1.0);
  }
  return float3(0.0, 0.0, 0.0);
}

// Linear segmented false color, same palette as 0x3E's DebugLinearSegments.
// Uniform bands in the displayed value (not log2) so dark, mid and bright
// ranges are all readable in one frame: <0.01 near black, 0.01-0.25 deep blue,
// 0.25-0.5 cyan, 0.5-1 green, 1-2 yellow, 2-4 orange, 4-8 red, >8 white.
float3 DebugLinearSegments(float value) {
  const float v = max(0.0, value);
  if (v < 0.01) return float3(0.05, 0.05, 0.1);
  if (v < 0.25) return float3(0.1, 0.1, 1.0);
  if (v < 0.5) return float3(0.0, 0.8, 1.0);
  if (v < 1.0) return float3(0.0, 1.0, 0.0);
  if (v < 2.0) return float3(1.0, 1.0, 0.0);
  if (v < 4.0) return float3(1.0, 0.55, 0.0);
  if (v < 8.0) return float3(1.0, 0.0, 0.0);
  return float3(1.0, 1.0, 1.0);
}

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
  const bool probe59 = RENODX_DEBUG_MODE > 58.5 && RENODX_DEBUG_MODE < 59.5;
  if (RENODX_DEBUG_MODE > 32.5 && RENODX_DEBUG_MODE < 33.5) {
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    const float strengths[4] = {0.0, 0.25, 0.5, 0.75};
    input_hdr = lerp(input_hdr, renodx::color::srgb::DecodeSafe(input_hdr), strengths[quadrant]);
  }
  // Vanilla keeps DL2's original SDR curve. HDR uses a hybrid proxy: below
  // the recovered SDR-white input it is exactly the same curve; above that
  // boundary it smoothly expands toward the exposed scene value so highlights
  // are not limited by the curve's finite ~1.34 asymptote. The proxy is then
  // compressed with one shared pixel-local scale, carried through the entire
  // native grade, and restored after grading.
  const float4 sdr_curve0 = float4(2.27, 0.17, 1.69, 0.8);
  const float4 sdr_curve1 = float4(0.14, 0.0, 0.0, 0.0);
  const float3 hdr_curve = ApplyDL2SDRCurveExtended(input_hdr, sdr_curve0, sdr_curve1);
  // Solving ApplyDL2SDRCurve(x)=1 with the audited constants gives ~1.275.
  // The existing exit@2 diagnostics provide a conservative upper end for the
  // transition; below the first value the LUT reference remains unchanged.
  const float dl2_sdr_white_input = 1.275f;
  const float dl2_hdr_expansion_end = 2.0f;
  const float input_max = max(input_hdr.r, max(input_hdr.g, input_hdr.b));
  const float expansion = smoothstep(
      dl2_sdr_white_input,
      dl2_hdr_expansion_end,
      input_max);
  const float3 hdr_proxy = lerp(hdr_curve, input_hdr, expansion);
  const float hdr_proxy_max = max(hdr_proxy.r, max(hdr_proxy.g, hdr_proxy.b));
  const float hdr_proxy_scale = hdr_proxy_max > 1.0
                                    ? rcp(max(hdr_proxy_max, 1e-6))
                                    : 1.0;
  const float3 neutral_sdr = RENODX_TONE_MAP_TYPE == 0.0
                                 ? ApplyDL2SDRCurve(input_hdr, sdr_curve0, sdr_curve1)
                                 : hdr_proxy * hdr_proxy_scale;
  if (RENODX_TONE_MAP_TYPE != 0.0 || probe59) {
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
    // HDR path: keep the LUT result in the SDR/LUT domain (0..1). The LUT is
    // sampled with the same hand-written sRGB encode as the Vanilla path, so
    // HDR's LUT sample coordinates match vanilla exactly. Using Renodx
    // srgb::EncodeSafe here produced slightly different coordinates, which
    // shifted the LUT grade and washed out low/mid colors. Highlights are
    // recovered by the ToneMapPass below.
    float3 lut_gamma;
    float3 lut_gamma_lin = neutral_sdr * float3(12.9200001, 12.9200001, 12.9200001);
    float3 lut_gamma_srgb = exp2(float3(0.416666657, 0.416666657, 0.416666657) * log2(abs(neutral_sdr)));
    lut_gamma_srgb = lut_gamma_srgb * float3(1.05499995, 1.05499995, 1.05499995) + float3(-0.0549999997, -0.0549999997, -0.0549999997);
    lut_gamma = cmp(float3(0.00313080009, 0.00313080009, 0.00313080009) >= neutral_sdr) ? lut_gamma_lin : lut_gamma_srgb;
    lut_gamma = lut_gamma * float3(0.96875, 0.96875, 0.96875) + float3(0.015625, 0.015625, 0.015625);
    float3 lut_sampled = t1.SampleLevel(s1_s, lut_gamma, 0).xyz;
    // Match Vanilla's single LUT-output decode exactly. The affine/pow
    // sequence below is already the inverse sRGB operation; calling
    // srgb::DecodeSafe on its result would decode the LUT output twice and
    // crush/destaturate low-light colors such as fog.
    float3 lut_linear = lut_sampled * float3(0.947867274, 0.947867274, 0.947867274)
                        + float3(0.0521326996, 0.0521326996, 0.0521326996);
    float3 lut_linear_pow = exp2(float3(2.4000001, 2.4000001, 2.4000001) * log2(abs(lut_linear)));
    float3 lut_linear_low = lut_sampled * float3(0.0773993805, 0.0773993805, 0.0773993805);
    lut_result = lut_sampled <= float3(0.0404499993, 0.0404499993, 0.0404499993)
                     ? lut_linear_low
                     : lut_linear_pow;
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
    // Reconstruct the LUT-graded HDR signal, then apply one standalone HDR
    // tone map. Do not feed the result back through the three-argument
    // UpgradeToneMap path.
    upgraded_grade = renodx::math::DivideSafe(
        native_lut_grade,
        hdr_proxy_scale.xxx,
        native_lut_grade);
    stable_grade = upgraded_grade;
    o0.rgb = renodx::draw::ToneMapPass(upgraded_grade);
  }

  if (RENODX_DEBUG_MODE > 34.5 && RENODX_DEBUG_MODE < 35.5) {
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    o0.rgb = quadrant == 0u ? native_lut_grade
        : quadrant == 1u ? upgraded_grade
        : quadrant == 2u ? stable_grade
        : o0.rgb;
  }

  // LUT-input probe (mode 59). Mode 59 forces 0x3E to output untonemapped in
  // BOTH tone map types, so 0x268 receives the same linear scene and every
  // quadrant except BR is mode-invariant:
  //   TL = input_hdr (raw untonemapped this pass actually received)
  //   TR = neutral_sdr (curve(untonemapped) == vanilla, the HDR LUT input)
  //   BL = native_lut_grade (LUT grading of the vanilla reference)
  //   BR = current normal output (ToneMapPass in HDR, LUT grade in Off)
  // TL/TR/BL are false-colored with the linear segmented palette (per-max-
  // channel) so values are readable even in a bright scene: TL raw stays
  // yellow/orange/red in the sun while TR/BL vanilla-domain saturate green.
  // Toggling ToneMapType must not change TL/TR/BL at all; if it does, either
  // the auto exposure drifted or 0x268 received something other than
  // untonemapped. The old design showed BL = neutral_sdr, but in the Off mode
  // input_hdr was already the SDR output so neutral_sdr double-curved and was
  // NOT mode-invariant; forcing untonemapped through 0x3E fixes that.
  if (probe59) {
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    if (quadrant < 3u) {
      const float3 value = quadrant == 0u ? input_hdr
          : quadrant == 1u ? neutral_sdr
          : native_lut_grade;
      o0.rgb = DebugLinearSegments(max(value.r, max(value.g, value.b)));
    }
    // BR quadrant keeps the real output as-is.
    o0.a = 1.0;
    return;
  }

  // Numeric probe (mode 60): samples input_hdr / native_lut_grade /
  // reconstructed at the same ten-pixel-left source point used by the AD
  // probe and renders their luminance as glyph digits under a center
  // crosshair. Mode 59's false-color bands are too coarse to compare exact
  // values across ToneMapType toggles; this gives exact numbers for one point.
  // 0x3E forces untonemapped for this mode too (same lut_input_probe range).
  // The vignette is skipped (diagnostic source point is near center, off).
  if (RENODX_DEBUG_MODE > 59.5 && RENODX_DEBUG_MODE < 60.5) {
    uint probe_width = 1u;
    uint probe_height = 1u;
    t0.GetDimensions(probe_width, probe_height);
    const float probe_offset_x = 10.0 / max((float)probe_width, 1.0);
    const float2 probe_uv = float2(0.5 - probe_offset_x, 0.5);
    const float4 probe_src = t0.SampleLevel(s0_s, probe_uv, 0);
    const float3 probe_hdr = max(probe_src.rgb, 0.0);
    const float3 probe_hdr_curve = ApplyDL2SDRCurveExtended(probe_hdr, sdr_curve0, sdr_curve1);
    const float probe_input_max = max(probe_hdr.r, max(probe_hdr.g, probe_hdr.b));
    const float probe_expansion = smoothstep(
        dl2_sdr_white_input, dl2_hdr_expansion_end, probe_input_max);
    const float3 probe_proxy = lerp(probe_hdr_curve, probe_hdr, probe_expansion);
    const float probe_proxy_max = max(probe_proxy.r, max(probe_proxy.g, probe_proxy.b));
    const float probe_proxy_scale = probe_proxy_max > 1.0
                                        ? rcp(max(probe_proxy_max, 1e-6))
                                        : 1.0;
    const float3 probe_neutral = probe_proxy * probe_proxy_scale;
    float3 lut_gamma;
    float3 lut_gamma_lin = probe_neutral * float3(12.9200001, 12.9200001, 12.9200001);
    float3 lut_gamma_srgb = exp2(float3(0.416666657, 0.416666657, 0.416666657) * log2(abs(probe_neutral)));
    lut_gamma_srgb = lut_gamma_srgb * float3(1.05499995, 1.05499995, 1.05499995) + float3(-0.0549999997, -0.0549999997, -0.0549999997);
    lut_gamma = cmp(float3(0.00313080009, 0.00313080009, 0.00313080009) >= probe_neutral) ? lut_gamma_lin : lut_gamma_srgb;
    lut_gamma = lut_gamma * float3(0.96875, 0.96875, 0.96875) + float3(0.015625, 0.015625, 0.015625);
    float3 probe_lut = t1.SampleLevel(s1_s, lut_gamma, 0).xyz;
    const float3 probe_lut_linear = probe_lut * float3(0.947867274, 0.947867274, 0.947867274)
                                    + float3(0.0521326996, 0.0521326996, 0.0521326996);
    const float3 probe_lut_linear_pow = exp2(
        float3(2.4000001, 2.4000001, 2.4000001) * log2(abs(probe_lut_linear)));
    const float3 probe_lut_linear_low = probe_lut * float3(0.0773993805, 0.0773993805, 0.0773993805);
    probe_lut = probe_lut <= float3(0.0404499993, 0.0404499993, 0.0404499993)
                    ? probe_lut_linear_low
                    : probe_lut_linear_pow;
    float3 temp_adjusted = probe_lut * r0.xyz;
    float temp_luma = dot(temp_adjusted, float3(0.212599993, 0.715200007, 0.0722000003));
    temp_luma = max(9.99999975e-005, temp_luma);
    float lut_luma = dot(probe_lut, float3(0.212599993, 0.715200007, 0.0722000003));
    temp_adjusted = temp_adjusted * (lut_luma / temp_luma);
    float3 probe_grade = (cb0[1].y != 6500.000000) ? temp_adjusted : probe_lut;
    float3 smooth = saturate(probe_grade);
    float3 smooth2 = smooth * float3(-2, -2, -2) + float3(3, 3, 3);
    smooth = smooth * smooth;
    smooth = smooth2 * smooth + -probe_grade;
    probe_grade = cb0[0].www * smooth + probe_grade;
    float grade_luma = saturate(dot(float3(0.212500006, 0.715399981, 0.0720999986), probe_grade));
    probe_grade = probe_grade + -grade_luma;
    probe_grade = cb0[1].xxx * probe_grade + grade_luma;
    const float3 probe_reconstructed = RENODX_TONE_MAP_TYPE == 0.0
                                           ? probe_grade
                                           : renodx::math::DivideSafe(
                                                 probe_grade,
                                                 probe_proxy_scale.xxx,
                                                 probe_grade);
    const float3 probe_tonemapped = RENODX_TONE_MAP_TYPE == 0.0
                                        ? probe_reconstructed
                                        : renodx::draw::ToneMapPass(probe_reconstructed);
    const float v_in = dot(probe_hdr, float3(0.2126, 0.7152, 0.0722));
    const float v_l = dot(probe_grade, float3(0.2126, 0.7152, 0.0722));
    const float v_r = dot(probe_reconstructed, float3(0.2126, 0.7152, 0.0722));
    const float v_t = dot(probe_tonemapped, float3(0.2126, 0.7152, 0.0722));
    o0.rgb = 0.0;
    if (abs(v1.x - 0.5) < 0.002 || abs(v1.y - 0.5) < 0.002) o0.rgb += float3(1.0, 1.0, 1.0);
    // Raw full-RGB values for the deferred center probe. All four values were
    // derived from probe_src sampled once at the same ten-pixel-left source
    // point used by the AD probe, not from these output pixel coordinates.
    if (all(abs(v1.xy - float2(0.30, 0.58)) < float2(0.0015, 0.0015))) o0.rgb = probe_hdr;
    if (all(abs(v1.xy - float2(0.30, 0.68)) < float2(0.0015, 0.0015))) o0.rgb = probe_grade;
    if (all(abs(v1.xy - float2(0.30, 0.78)) < float2(0.0015, 0.0015))) o0.rgb = probe_reconstructed;
    if (all(abs(v1.xy - float2(0.30, 0.88)) < float2(0.0015, 0.0015))) o0.rgb = probe_tonemapped;
    // Four values stacked vertically, top to bottom: I, L, R, T.
    const float label_x = 0.44;
    o0.rgb += DebugRenderLabel(v1.xy, label_x, 0.58, v_in, 0.008);
    o0.rgb += DebugRenderLabel(v1.xy, label_x, 0.68, v_l, 0.008);
    o0.rgb += DebugRenderLabel(v1.xy, label_x, 0.78, v_r, 0.008);
    o0.rgb += DebugRenderLabel(v1.xy, label_x, 0.88, v_t, 0.008);
    o0.a = 1.0;
    return;
  }

  // Game/Peak are handled by ToneMapPass above on the HDR normal path. Do not
  // scale again here or Game would be applied twice. Modes 40-42 keep the
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
