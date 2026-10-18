#ifndef SRC_GAMES_LIFEISSTRANGE_HDR_LUT_BRIDGE_HLSLI
#define SRC_GAMES_LIFEISSTRANGE_HDR_LUT_BRIDGE_HLSLI

bool LifeIsStrangeUsesToneMapPass(float curve_method) {
  return curve_method > 2.5f;
}

float3 LifeIsStrangeApplyToneMapPass(float3 untonemapped) {
  // Fixed A/B diagnostic: isolate RenoDRT from the game's LUT grade.
  return renodx::draw::ToneMapPass(untonemapped);
}

static const float LIFEISSTRANGE_HDR_CURVE_CHROMA_LIMIT = 1.25f;

float3 LifeIsStrangeHyperbolicCurve(
    float3 color,
    float2 image_adjustments,
    float pivot,
    float upper_bound) {
  float3 curve_base_input = min(color, pivot.xxx);
  float3 curve_base = curve_base_input
                      / (image_adjustments.x + image_adjustments.y * curve_base_input);
  float pivot_denominator = image_adjustments.x + image_adjustments.y * pivot;
  float pivot_curve = pivot / pivot_denominator;
  float pivot_slope = image_adjustments.x / (pivot_denominator * pivot_denominator);
  float3 tangent_delta = pivot_slope * max(color - pivot.xxx, 0.f);
  float safe_headroom = max(upper_bound - pivot_curve, 1e-5f);
  return curve_base + tangent_delta / (1.f + tangent_delta / safe_headroom);
}

float LifeIsStrangeGetHdrPeakCurveLimit(float2 image_adjustments, float pivot) {
  // FC2A sRGB-decodes this value, applies GammaCorrection, then scales by Game/Graphics white.
  // Keep the calculation independent of proxy-only encoding and enable settings.
  float relative_peak = RENODX_PEAK_WHITE_NITS / max(RENODX_DIFFUSE_WHITE_NITS, 1e-5f);
  float encoded_peak = renodx::color::srgb::EncodeSafe(relative_peak);
  if (RENODX_GAMMA_CORRECTION == renodx::draw::GAMMA_CORRECTION_GAMMA_2_2) {
    encoded_peak = renodx::color::gamma::EncodeSafe(relative_peak, 2.2f);
  } else if (RENODX_GAMMA_CORRECTION == renodx::draw::GAMMA_CORRECTION_GAMMA_2_4) {
    encoded_peak = renodx::color::gamma::EncodeSafe(relative_peak, 2.4f);
  }
  float pivot_denominator = image_adjustments.x + image_adjustments.y * pivot;
  float pivot_curve = pivot / pivot_denominator;
  return max(encoded_peak, pivot_curve + 1e-4f);
}

// Shared ImageAdjustments2 tangent extension and bounded SDR-LUT transport.
// The sRGB-shaped LUT domain is a working assumption retained from the
// established 06A2 bridge; the LUT sampler and native packed addressing stay
// in each original pixel shader.
float3 LifeIsStrangeExtendImageAdjustments2(
    float3 color,
    float2 image_adjustments,
    float pivot,
    float curve_method,
    out float3 hdr_reference) {
  float3 curve_base_input = min(color, pivot.xxx);
  float3 curve_base = curve_base_input
                      / (image_adjustments.x + image_adjustments.y * curve_base_input);
  float pivot_denominator = image_adjustments.x + image_adjustments.y * pivot;
  float pivot_slope = image_adjustments.x / (pivot_denominator * pivot_denominator);
  float3 extended_curve = curve_base + pivot_slope * max(color - pivot.xxx, 0.f);

  hdr_reference = extended_curve;

  if (curve_method >= 1.5f && curve_method < 2.5f) {
    // Method 2 separates LUT-domain desaturation from HDR headroom. The LUT
    // ceiling stays fixed; the HDR ceiling follows the configured output peak.
    extended_curve = LifeIsStrangeHyperbolicCurve(
        color,
        image_adjustments,
        pivot,
        LIFEISSTRANGE_HDR_CURVE_CHROMA_LIMIT);
    hdr_reference = LifeIsStrangeHyperbolicCurve(
        color,
        image_adjustments,
        pivot,
        LifeIsStrangeGetHdrPeakCurveLimit(image_adjustments, pivot));
  } else if (curve_method > 0.5f && curve_method < 1.5f) {
    float max_channel_input = max(color.r, max(color.g, color.b));
    if (max_channel_input > pivot) {
      float pivot_curve = pivot / pivot_denominator;
      float max_channel_extended = pivot_curve
                                   + pivot_slope * (max_channel_input - pivot);
      float max_channel_vanilla = max_channel_input
                                  / (image_adjustments.x + image_adjustments.y * max_channel_input);
      float shared_scale = max_channel_extended / max(max_channel_vanilla, 1e-6f);
      float3 vanilla_curve = color / (image_adjustments.x + image_adjustments.y * color);
      extended_curve = vanilla_curve * shared_scale;
    }
  }

  return extended_curve;
}

float3 LifeIsStrangeCompressForColorGradingLut(
    float3 lut_curve,
    float3 hdr_curve,
    out float reconstruction_scale) {
  float3 lut_linear = renodx::color::srgb::DecodeSafe(lut_curve);
  float3 hdr_linear = renodx::color::srgb::DecodeSafe(hdr_curve);
  float n2_scale = renodx::tonemap::neutwo::ComputeMaxChannelScale(lut_linear);
  float lut_max = max(abs(lut_linear.r), max(abs(lut_linear.g), abs(lut_linear.b)));
  float hdr_max = max(abs(hdr_linear.r), max(abs(hdr_linear.g), abs(hdr_linear.b)));
  float curve_ratio = hdr_max > 0.f ? lut_max / hdr_max : 1.f;

  // The LUT is sampled from lut_linear, but restoration must reach the
  // separate HDR reference curve when method 2 is active. Equal curves keep
  // methods 0 and 1 bit-for-bit equivalent to the previous bridge.
  reconstruction_scale = n2_scale * curve_ratio;
  return saturate(renodx::color::srgb::EncodeSafe(lut_linear * n2_scale));
}

float3 LifeIsStrangeRestoreAfterColorGradingLut(float3 lut_result, float reconstruction_scale) {
  float3 graded_linear = renodx::color::srgb::DecodeSafe(lut_result);
  float3 reconstructed_linear = renodx::math::DivideSafe(
      graded_linear,
      reconstruction_scale.xxx,
      graded_linear);
  return renodx::color::srgb::EncodeSafe(reconstructed_linear);
}

float3 LifeIsStrangeApplyToneMapOrRestoreColorGradingLut(
    float3 lut_result,
    float reconstruction_scale,
    float3 untonemapped) {
  if (LifeIsStrangeUsesToneMapPass(LIFEISSTRANGE_HDR_CURVE_METHOD)) {
    return renodx::color::srgb::EncodeSafe(LifeIsStrangeApplyToneMapPass(untonemapped));
  }
  return LifeIsStrangeRestoreAfterColorGradingLut(lut_result, reconstruction_scale);
}

#endif  // SRC_GAMES_LIFEISSTRANGE_HDR_LUT_BRIDGE_HLSLI
