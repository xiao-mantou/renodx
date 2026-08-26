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

// Linear segmented false color. Unlike DebugFalseColor, the bands are uniform
// in the displayed value rather than log2, so low night values do not collapse
// into one blue region and absolute levels are readable at a glance.
float3 DebugLinearSegments(float value) {
  const float v = max(0.0, value);
  if (v < 0.01) return float3(0.05, 0.05, 0.1);    // near black
  if (v < 0.25) return float3(0.1, 0.1, 1.0);      // 0.01-0.25 deep blue
  if (v < 0.5) return float3(0.0, 0.8, 1.0);       // 0.25-0.5 cyan
  if (v < 1.0) return float3(0.0, 1.0, 0.0);       // 0.5-1.0 green
  if (v < 2.0) return float3(1.0, 1.0, 0.0);       // 1-2 yellow
  if (v < 4.0) return float3(1.0, 0.55, 0.0);      // 2-4 orange
  if (v < 8.0) return float3(1.0, 0.0, 0.0);       // 4-8 red
  return float3(1.0, 1.0, 1.0);                    // >8 white
}

// Continuous dark-scene luminance palette. Values below 2.0 are mapped strictly
// linearly (absolute luminance, not log2), so the 0.05-0.5 band that dominates
// a night scene keeps a visible dark-blue -> cyan -> green -> yellow gradient.
// Values above 2.0 are compressed by log2 so scene peaks and lights do not
// dominate the whole palette, ending at red -> white.
float3 DebugDarkSceneLinear(float value) {
  const float v = max(0.0, value);
  const float3 c0 = float3(0.0, 0.08, 0.45);  // deep blue
  const float3 c1 = float3(0.0, 0.75, 0.9);   // cyan
  const float3 c2 = float3(0.1, 1.0, 0.1);    // green
  const float3 c3 = float3(0.95, 0.9, 0.1);   // yellow
  const float3 c4 = float3(1.0, 0.45, 0.0);   // orange
  const float3 c5 = float3(0.95, 0.0, 0.0);   // red
  const float3 c6 = float3(1.0, 1.0, 1.0);    // white
  // 0-2 linear, >2 log2-compressed into the 2-8 band. s spans 0..2; the
  // segment lerps below expect t to reach 2 (not saturate to 1), otherwise
  // every value >= 2 collapses to a single orange.
  const float s = v < 2.0
                      ? v * 0.5                                        // 0 -> 0, 2 -> 1
                      : 1.0 + log2(v * 0.5) / 3.0;                     // 2 -> 1, 8 -> 2
  const float t = min(s, 2.0);
  if (t < 0.25) return lerp(c0, c1, t / 0.25);
  if (t < 0.5) return lerp(c1, c2, (t - 0.25) / 0.25);
  if (t < 0.75) return lerp(c2, c3, (t - 0.5) / 0.25);
  if (t < 1.0) return lerp(c3, c4, (t - 0.75) / 0.25);
  if (t < 1.5) return lerp(c4, c5, (t - 1.0) / 0.5);
  return lerp(c5, c6, saturate((t - 1.5) / 0.5));
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

// Draws "E" (t1 global exposure baseline label) followed by the baseline as
// one integer digit and one decimal digit (e.g. E1.5). This is t1[0] ONLY and
// is a rough visual: the authoritative value is the t1 readback logged by the
// 0x3E Inputs and Curve audit. The wide gap after E keeps "E 8 . 1" from
// reading as "81", and a value >= 10 would show as 9.x because only one digit
// is rendered.
float3 DebugExposureOverlay(float2 uv, float exposure) {
  const float2 chip0 = float2(0.015, 0.012);
  const float2 chip1 = float2(0.24, 0.075);
  if (uv.x < chip0.x || uv.x > chip1.x || uv.y < chip0.y || uv.y > chip1.y) {
    return float3(0.0, 0.0, 0.0);
  }
  const int whole = clamp((int)exposure, 0, 9);
  const int tenth = clamp((int)((exposure - floor(exposure)) * 10.0 + 0.5), 0, 9);
  // Cells: 'E', digit(whole), '.', digit(tenth).
  const int glyphs[4] = {11, whole, 10, tenth};
  const float cell_x = 0.011;
  const float cell_y = 0.008;
  for (int i = 0; i < 4; ++i) {
    const float x0 = chip0.x + i * (5.0 * cell_x + 0.014);
    const float x1 = x0 + 5.0 * cell_x;
    if (uv.x < x0 || uv.x >= x1) continue;
    const int col = (int)((uv.x - x0) / cell_x);
    const int row = (int)((uv.y - chip0.y) / cell_y);
    if (row < 0 || row > 6) continue;
    const bool lit = glyphs[i] == 11
                         ? (col == 0 || col == 4 || row == 0 || row == 3 || row == 6)
                         : ((DebugGlyph(glyphs[i], row) >> col) & 1u) != 0u;
    if (lit) return float3(1.0, 1.0, 1.0);
  }
  return float3(0.0, 0.0, 0.0);
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

// Renders a small integer/decimal label at (origin_x, origin_y) using the
// 5x7 glyphs, so diagnostics can annotate each ladder bar with its input value.
// Returns white if this pixel is part of the label, black otherwise.
float3 DebugRenderLabel(float2 uv, float origin_x, float origin_y, float value, float cell = 0.012f) {
  // Format value up to one decimal: e.g. 0.18 -> "0.18", 32 -> "32".
  const int whole = clamp((int)value, 0, 99);
  const int tenth = clamp((int)((value - floor(value)) * 10.0 + 0.5), 0, 9);
  const int hundredth = value < 1.0
                            ? clamp((int)((value - floor(value)) * 100.0 + 0.5) % 10, 0, 9)
                            : -1;
  // Characters: tens, ones, ('.', tenth, hundredth if <1).
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

// DL2-specific diagnostic curve. The source scene is anchored to 203 nits,
// so Game Brightness must change the gray output without moving the input
// value that reaches Peak Brightness. The generic three-argument path uses
// Peak/Game for both decisions and therefore moves that highlight anchor.
float3 ToneMapDL2Anchored(
    float3 untonemapped,
    float3 vanilla,
    float3 neutral_sdr,
    float input_clip) {
  if (RENODX_TONE_MAP_TYPE != 3.0) {
    return ScaleToneMappedScene(
        renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr));
  }

  const float3 graded = renodx::draw::ComputeUntonemappedGraded(
      untonemapped, vanilla, neutral_sdr);
  const float y = renodx::color::y::from::BT709(max(graded, 0.0));
  const float peak = max(RENODX_PEAK_WHITE_NITS / 203.0, 1.0);
  const float game = max(RENODX_DIFFUSE_WHITE_NITS / 203.0, 0.01);
  // The input anchor must remain above the output peak ratio. This prevents
  // the curve from entering its invalid region when Peak is raised.
  const float clip = max(input_clip, peak + 0.001);
  const float mapped_y = min(renodx::tonemap::Neutwo(y, peak, clip, 1.0, game), peak);
  const float scale = y > 0.00001 ? mapped_y / y : 1.0;
  return max(graded, 0.0) * scale;
}

// Strict DL2 separation candidate: Game affects only the paper-white range,
// while the Neutwo shoulder remains in fixed 203-nit scene units.
float3 ToneMapDL2Separated(
    float3 untonemapped,
    float3 vanilla,
    float3 neutral_sdr,
    float transition_end) {
  if (RENODX_TONE_MAP_TYPE != 3.0) {
    return ScaleToneMappedScene(
        renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr));
  }

  const float3 graded = renodx::draw::ComputeUntonemappedGraded(
      untonemapped, vanilla, neutral_sdr);
  const float y = renodx::color::y::from::BT709(max(graded, 0.0));
  const float peak = max(RENODX_PEAK_WHITE_NITS / 203.0, 1.0);
  const float game = max(RENODX_DIFFUSE_WHITE_NITS / 203.0, 0.01);
  const float clip = max(RENODX_RENO_DRT_WHITE_CLIP, peak + 0.001);
  const float fixed_y = min(renodx::tonemap::Neutwo(y, peak, clip, 1.0, 1.0), peak);
  const float low_y = fixed_y * game;
  const float blend = smoothstep(1.0, max(transition_end, 1.001), y);
  const float mapped_y = lerp(low_y, fixed_y, blend);
  const float scale = y > 0.00001 ? mapped_y / y : 1.0;
  return max(graded, 0.0) * scale;
}

// Fixed 203-nit scene mapping for post-LUT paper-white diagnostics. Game is
// deliberately absent so the later 0x268 pass can own that control.
float3 ToneMapDL2FixedScene(
    float3 untonemapped,
    float3 vanilla,
    float3 neutral_sdr) {
  const float3 graded = renodx::draw::ComputeUntonemappedGraded(
      untonemapped, vanilla, neutral_sdr);
  const float y = renodx::color::y::from::BT709(max(graded, 0.0));
  const float peak = max(RENODX_PEAK_WHITE_NITS / 203.0, 1.0);
  const float clip = max(RENODX_RENO_DRT_WHITE_CLIP, peak + 0.001);
  const float mapped_y = min(renodx::tonemap::Neutwo(y, peak, clip, 1.0, 1.0), peak);
  const float scale = y > 0.00001 ? mapped_y / y : 1.0;
  return max(graded, 0.0) * scale;
}

// Bounded display mapping in DL2's fixed 203-nit scene units. Source White
// is the scene value mapped to Peak and is independent from Game Brightness.
float3 ToneMapDL2FixedHermite(
    float3 untonemapped,
    float3 vanilla,
    float3 neutral_sdr,
    float source_white) {
  const float3 graded = max(renodx::draw::ComputeUntonemappedGraded(
                                untonemapped, vanilla, neutral_sdr),
                            0.0);
  const float max_channel = max(graded.r, max(graded.g, graded.b));
  const float peak = max(RENODX_PEAK_WHITE_NITS / 203.0, 1.0);
  const float mapped_max = max_channel > 0.0
      ? min(renodx::tonemap::HermiteSplineLuminanceRolloff(
                max_channel, peak, max(source_white, peak + 0.001)),
            peak)
      : 0.0;
  const float scale = max_channel > 0.00001 ? mapped_max / max_channel : 1.0;
  return graded * scale;
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
  // A1 baseline: return to standard RenoDRT input. The 0.6 scene calibration
  // and Night Scene Gain were DL2-specific experiments with no derivation;
  // scene_linear applies DL2's native 0.6 calibration: the original 0x3E
  // disassembly does `source * exposure * 0.6` before its SDR curve (dump
  // 0x3E36DA5B.ps_5_0.hlsl). Removing it scaled the scene 1.67x and caused
  // the washed-out/overexposed look. untonemapped uses the game exposure
  // directly, without the DL2 highlight-protection chain (that chain is still
  // computed below only because the diagnostic modes reference it).
  const float3 scene_linear = source.rgb * 0.6;
  const float3 game_exposed = scene_linear * exposure;
  const float3 vanilla = ApplyDL2SDRCurve(game_exposed, cb0[0], cb0[1]);

  // Retained only for the diagnostic modes that reference it. The normal path
  // uses `scene_linear * exposure` directly below.
  const float exposure_min = min(max(RENODX_AUTO_EXPOSURE_MIN, 0.01),
                                 max(RENODX_AUTO_EXPOSURE_MAX, 0.01));
  const float exposure_max = max(max(RENODX_AUTO_EXPOSURE_MIN, 0.01),
                                 max(RENODX_AUTO_EXPOSURE_MAX, 0.01));
  const float retained_exposure = clamp(max(exposure, 0.001), exposure_min, exposure_max);
  const float protected_exposure = lerp(1.0, retained_exposure, saturate(CUSTOM_AUTO_EXPOSURE));
  const float protection_start = min(max(RENODX_HDR_EXPOSURE_PROTECTION_START, 0.0),
                                     max(RENODX_HDR_EXPOSURE_PROTECTION_END, 0.001));
  const float protection_end = max(max(RENODX_HDR_EXPOSURE_PROTECTION_START, 0.0),
                                   max(RENODX_HDR_EXPOSURE_PROTECTION_END, protection_start + 0.001));
  const float scene_luminance = renodx::color::y::from::BT709(max(scene_linear, 0.0));
  const float protection = smoothstep(protection_start, protection_end, scene_luminance);
  const float adaptive_exposure = lerp(exposure, protected_exposure, protection);
  const float3 untonemapped = scene_linear * exposure;
  const float3 neutral_sdr = renodx::tonemap::renodrt::NeutralSDR(untonemapped);

  float3 stage_probe;
  if (Dl2RenderLuminanceStageProbe(v1.xy, 1.0, stage_probe)) {
    o0 = float4(stage_probe, 1.0);
    return;
  }

  // ToneMapPass input->output response ladder. Injects known untonemapped
  // scene values (0.18 through 32) into the standard single-argument
  // ToneMapPass and renders each result as an opaque bar, so the full
  // input->output nit curve can be read from one frame without scene
  // interference. The bar color is the raw ToneMapPass result; the label above
  // each bar shows the injected input value. A healthy curve is monotonic with
  // the high bars reaching toward Peak.
  if (RENODX_DEBUG_MODE > 47.5 && RENODX_DEBUG_MODE < 48.5) {
    const float levels[8] = {0.18, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0};
    // Opaque black background so the scene cannot leak through and skew the
    // luminance readback.
    o0 = float4(0.0, 0.0, 0.0, 1.0);
    if (v1.x >= 0.06 && v1.x <= 0.94 && v1.y >= 0.35 && v1.y <= 0.75) {
      const uint index = min((uint)((v1.x - 0.06) / 0.88 * 8.0), 7u);
      const float3 test_input = levels[index].xxx;
      // Single-argument ToneMapPass: pure input->output curve, no graded/
      // neutral reconstruction that would depend on the real scene.
      const float3 test_output = renodx::draw::ToneMapPass(test_input);
      o0.rgb = test_output;
    } else {
      // Label row above the bars shows each input value.
      const float label_y = 0.26;
      for (int i = 0; i < 8; ++i) {
        const float bar_left = 0.06 + (float)i / 8.0 * 0.88;
        o0.rgb += DebugRenderLabel(v1.xy, bar_left + 0.01, label_y, levels[i]);
      }
    }
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
  // Same raw t0 source as mode 10, but this time as DL2's full auto-exposed
  // scene (scene_linear * exposure) and with a linear segmented palette. This
  // shows what the game actually sees before the SDR curve: low night walls
  // read blue/green and any light source at or above scene 1.0 reads
  // yellow/orange/red/white, so the nighttime highlight gap becomes readable.
  if (RENODX_DEBUG_MODE > 45.5 && RENODX_DEBUG_MODE < 46.5) {
    const float3 exposed_scene = scene_linear * exposure;
    const float exposed_range = max(exposed_scene.r, max(exposed_scene.g, exposed_scene.b));
    o0 = float4(renodx::draw::RenderIntermediatePass(DebugLinearSegments(exposed_range)), 1.0);
    return;
  }
  // Dark-scene absolute luminance of the RAW t0 scene, continuous linear
  // palette. Unlike Mode 46 this does NOT multiply by t1[0] - scaling the whole
  // frame by one scalar reads like a uniform filter. It matches Source t0 Range
  // (Mode 10) but with the linear 0-2 palette and log-compressed highlights, so
  // the 0.05-0.5 night bulk keeps visible gradient and lights reach orange/red.
  // The top-left chip shows the t1[0] global exposure baseline ONLY; the real
  // per-pixel exposure is t1[0] * cb0 curve * pixel luminance.
  if (RENODX_DEBUG_MODE > 46.5 && RENODX_DEBUG_MODE < 47.5) {
    const float raw_range = max(source.r, max(source.g, source.b));
    const float3 probe = renodx::draw::RenderIntermediatePass(DebugDarkSceneLinear(raw_range));
    const float3 readout = DebugExposureOverlay(v1.xy, exposure);
    o0 = float4(probe + readout, 1.0);
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

  // Repeat the same source image in four tiles and test explicit input-view
  // semantics before any DL2 exposure or RenoDRT work. The differences are
  // intentionally large: current linear interpretation, hardware-equivalent
  // sRGB decode, gamma-2.2 decode, and a half-strength sRGB decode.
  if (RENODX_DEBUG_MODE > 31.5 && RENODX_DEBUG_MODE < 32.5) {
    const float2 tile_uv = frac(v1.xy * 2.0);
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    const float4 tile_source = t0.SampleLevel(s0_s, tile_uv, 0);
    const float3 srgb_decoded = renodx::color::srgb::DecodeSafe(tile_source.rgb);
    const float3 gamma_decoded = pow(max(tile_source.rgb, 0.0), 2.2);
    const float3 interpreted_source = quadrant == 0u ? tile_source.rgb
        : quadrant == 1u ? srgb_decoded
        : quadrant == 2u ? gamma_decoded
        : lerp(tile_source.rgb, srgb_decoded, 0.5);
    const float3 interpreted_scene = interpreted_source * 0.6;
    const float3 interpreted_exposed = interpreted_scene * exposure;
    const float3 interpreted_vanilla = ApplyDL2SDRCurve(interpreted_exposed, cb0[0], cb0[1]);
    const float interpreted_luminance = renodx::color::y::from::BT709(max(interpreted_scene, 0.0));
    const float interpreted_protection = smoothstep(protection_start, protection_end, interpreted_luminance);
    const float interpreted_adaptive_exposure = lerp(exposure, protected_exposure, interpreted_protection);
    const float3 interpreted_hdr = interpreted_scene * interpreted_adaptive_exposure;
    const float3 interpreted_neutral = renodx::tonemap::renodrt::NeutralSDR(interpreted_hdr);
    o0.rgb = RENODX_TONE_MAP_TYPE == 0.0
        ? interpreted_vanilla
        : ScaleToneMappedScene(renodx::draw::ToneMapPass(
              interpreted_hdr, interpreted_vanilla, interpreted_neutral));
    o0.a = tile_source.a;
    return;
  }

  // Repeat the normal 0x3E output so the 0x268 replacement can compare four
  // partial read-side sRGB decode strengths without changing this pass.
  if (RENODX_DEBUG_MODE > 32.5 && RENODX_DEBUG_MODE < 33.5) {
    const float2 tile_uv = frac(v1.xy * 2.0);
    const float4 tile_source = t0.SampleLevel(s0_s, tile_uv, 0);
    const float3 tile_scene = tile_source.rgb * 0.6;
    const float3 tile_exposed = tile_scene * exposure;
    const float3 tile_vanilla = ApplyDL2SDRCurve(tile_exposed, cb0[0], cb0[1]);
    const float tile_luminance = renodx::color::y::from::BT709(max(tile_scene, 0.0));
    const float tile_protection = smoothstep(protection_start, protection_end, tile_luminance);
    const float tile_adaptive_exposure = lerp(exposure, protected_exposure, tile_protection);
    const float3 tile_hdr = tile_scene * tile_adaptive_exposure;
    const float3 tile_neutral = renodx::tonemap::renodrt::NeutralSDR(tile_hdr);
    const float3 tile_output = RENODX_TONE_MAP_TYPE == 0.0
        ? tile_vanilla
        : ScaleToneMappedScene(renodx::draw::ToneMapPass(tile_hdr, tile_vanilla, tile_neutral));
    o0 = float4(tile_output, tile_source.a);
    return;
  }

  // This reaches the game's subsequent composite passes, unlike the output
  // probe in the swapchain proxy. With Peak=500 and Game=100, the scene area
  // should measure 500 nits if no later pass normalizes it back to SDR.
  if (RENODX_DEBUG_MODE > 33.5 && RENODX_DEBUG_MODE < 34.5) {
    const float3 scene = source.rgb * 0.6;
    const float3 exposed = scene * adaptive_exposure;
    const float3 neutral = renodx::tonemap::renodrt::NeutralSDR(exposed);
    const float3 scaled_hdr = ScaleToneMappedScene(
        renodx::draw::ToneMapPass(exposed, vanilla, neutral));
    const float3 unscaled_hdr = renodx::draw::ToneMapPass(exposed, vanilla, neutral);
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    o0.rgb = quadrant == 0u ? scene
        : quadrant == 1u ? vanilla
        : quadrant == 2u ? scaled_hdr
        : unscaled_hdr;
    o0.a = source.a;
    return;
  }

  // Repeat the same source image in four tiles to isolate scene calibration
  // from highlight-only expansion. TL is the current path, TR removes DL2's
  // 0.6 HDR calibration, BL applies a fixed 1.5x highlight gain, and BR uses
  // Peak Brightness to expand a 2.0 scene value toward the selected peak.
  // Both highlight expansions preserve values below diffuse white and keep
  // the original DL2 SDR curve as the graded reference.
  if (RENODX_DEBUG_MODE > 36.5 && RENODX_DEBUG_MODE < 37.5) {
    const float2 tile_uv = frac(v1.xy * 2.0);
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    const float4 tile_source = t0.SampleLevel(s0_s, tile_uv, 0);
    const float3 baseline_scene = tile_source.rgb * 0.6;
    const float baseline_luminance = renodx::color::y::from::BT709(max(baseline_scene, 0.0));
    const float highlight_weight = smoothstep(1.0, 2.4, baseline_luminance);
    const float peak_ratio = max(1.0, RENODX_PEAK_WHITE_NITS / 203.0);
    const float fixed_gain = lerp(1.0, 1.5, highlight_weight);
    const float peak_gain = lerp(1.0, max(1.0, peak_ratio / 2.0), highlight_weight);
    const float3 candidate_scene = quadrant == 0u ? baseline_scene
        : quadrant == 1u ? tile_source.rgb
        : quadrant == 2u ? baseline_scene * fixed_gain
        : baseline_scene * peak_gain;
    const float3 tile_vanilla = ApplyDL2SDRCurve(baseline_scene * exposure, cb0[0], cb0[1]);
    const float candidate_luminance = renodx::color::y::from::BT709(max(candidate_scene, 0.0));
    const float candidate_protection = smoothstep(protection_start, protection_end, candidate_luminance);
    const float candidate_exposure = lerp(exposure, protected_exposure, candidate_protection);
    const float3 candidate_hdr = candidate_scene * candidate_exposure;
    const float3 candidate_neutral = renodx::tonemap::renodrt::NeutralSDR(candidate_hdr);
    o0.rgb = RENODX_TONE_MAP_TYPE == 0.0
        ? tile_vanilla
        : ScaleToneMappedScene(renodx::draw::ToneMapPass(
              candidate_hdr, tile_vanilla, candidate_neutral));
    o0.a = tile_source.a;
    return;
  }

  // Compare the current Game/Peak coupling with fixed input highlight
  // anchors. This is diagnostic only; normal rendering remains unchanged.
  if (RENODX_DEBUG_MODE > 37.5 && RENODX_DEBUG_MODE < 38.5) {
    const float2 tile_uv = frac(v1.xy * 2.0);
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    const float4 tile_source = t0.SampleLevel(s0_s, tile_uv, 0);
    const float3 tile_scene_linear = tile_source.rgb * 0.6;
    const float tile_luminance = renodx::color::y::from::BT709(max(tile_scene_linear, 0.0));
    const float tile_protection = smoothstep(protection_start, protection_end, tile_luminance);
    const float tile_adaptive_exposure = lerp(exposure, protected_exposure, tile_protection);
    const float3 tile_scene = tile_scene_linear * tile_adaptive_exposure;
    const float3 tile_vanilla = ApplyDL2SDRCurve(tile_scene, cb0[0], cb0[1]);
    const float3 tile_neutral = renodx::tonemap::renodrt::NeutralSDR(tile_scene);
    const float3 current = ScaleToneMappedScene(
        renodx::draw::ToneMapPass(tile_scene, tile_vanilla, tile_neutral));
    const float3 anchored_10 = ToneMapDL2Anchored(
        tile_scene, tile_vanilla, tile_neutral, 10.0);
    const float3 anchored_5 = ToneMapDL2Anchored(
        tile_scene, tile_vanilla, tile_neutral, 5.0);
    const float3 anchored_20 = ToneMapDL2Anchored(
        tile_scene, tile_vanilla, tile_neutral, 20.0);
    o0.rgb = quadrant == 0u ? current
        : quadrant == 1u ? anchored_10
        : quadrant == 2u ? anchored_5
        : anchored_20;
    // Keep this diagnostic opaque so a later composite cannot reveal the
    // untouched full-screen source underneath the four quadrants.
    o0.a = 1.0;
    return;
  }

  // Compare strict paper-white separation with two transition widths.
  if (RENODX_DEBUG_MODE > 38.5 && RENODX_DEBUG_MODE < 39.5) {
    const float2 tile_uv = frac(v1.xy * 2.0);
    const uint quadrant = (v1.x >= 0.5 ? 1u : 0u) + (v1.y >= 0.5 ? 2u : 0u);
    const float4 tile_source = t0.SampleLevel(s0_s, tile_uv, 0);
    const float3 tile_scene_linear = tile_source.rgb * 0.6;
    const float tile_luminance = renodx::color::y::from::BT709(max(tile_scene_linear, 0.0));
    const float tile_protection = smoothstep(protection_start, protection_end, tile_luminance);
    const float tile_exposure = lerp(exposure, protected_exposure, tile_protection);
    const float3 tile_scene = tile_scene_linear * tile_exposure;
    const float3 tile_vanilla = ApplyDL2SDRCurve(tile_scene, cb0[0], cb0[1]);
    const float3 tile_neutral = renodx::tonemap::renodrt::NeutralSDR(tile_scene);
    const float3 current = ScaleToneMappedScene(
        renodx::draw::ToneMapPass(tile_scene, tile_vanilla, tile_neutral));
    const float3 anchored = ToneMapDL2Anchored(
        tile_scene, tile_vanilla, tile_neutral, RENODX_RENO_DRT_WHITE_CLIP);
    const float3 separated_2 = ToneMapDL2Separated(
        tile_scene, tile_vanilla, tile_neutral, 2.0);
    const float3 separated_3 = ToneMapDL2Separated(
        tile_scene, tile_vanilla, tile_neutral, 3.0);
    o0.rgb = quadrant == 0u ? current
        : quadrant == 1u ? anchored
        : quadrant == 2u ? separated_2
        : separated_3;
    o0.a = 1.0;
    return;
  }

  const bool downstream_color_grid = (RENODX_DEBUG_MODE > 28.5 && RENODX_DEBUG_MODE < 31.5)
      || (RENODX_DEBUG_MODE > 34.5 && RENODX_DEBUG_MODE < 35.5);
  const bool post_lut_game_probe = RENODX_DEBUG_MODE > 39.5 && RENODX_DEBUG_MODE < 45.5;
  // Modes 59/60 supply the real scene to the 0x268 LUT-input probes below;
  // they must not be blanked by the generic white patch.
  const bool lut_input_probe = RENODX_DEBUG_MODE > 58.5 && RENODX_DEBUG_MODE < 60.5;
  if (RENODX_DEBUG_MODE > 5.5 && !downstream_color_grid && !post_lut_game_probe && !lut_input_probe) {
    const float target_white = RENODX_PEAK_WHITE_NITS / 203.0;
    o0.rgb = renodx::draw::RenderIntermediatePass(float3(target_white, target_white, target_white));
    o0.a = 1.0;
    return;
  }

  o0.rgb = (RENODX_TONE_MAP_TYPE == 0.0 && !lut_input_probe)
      ? vanilla
      : RENODX_DEBUG_MODE > 42.5 && RENODX_DEBUG_MODE < 45.5
          ? ToneMapDL2FixedHermite(
                untonemapped, vanilla, neutral_sdr,
                RENODX_DEBUG_MODE < 43.5 ? 4.0
                    : RENODX_DEBUG_MODE < 44.5 ? 8.0
                                               : 16.0)
      : post_lut_game_probe
          ? ToneMapDL2FixedScene(untonemapped, vanilla, neutral_sdr)
          // Plan B (Silksong pattern): 0x3E outputs the untonemapped linear HDR
          // so the 0x268 LUT pass can run the full bridge (max-channel compress
          // into the game LUT, reconstruct, then three-argument ToneMapPass).
          // This lets 0x268's ToneMapPass receive the raw HDR and own the
          // Game/Peak rolloff, matching the SKILL reference. The vanilla SDR
          // path above is preserved for Off. Mode 59 forces untonemapped in
          // both modes so the 0x268 LUT-input probe gets a mode-invariant input.
          : untonemapped;
  o0.a = source.a;
}
