#ifndef SRC_GAMES_LIFEISSTRANGE_HDR_LUT_BRIDGE_HLSLI
#define SRC_GAMES_LIFEISSTRANGE_HDR_LUT_BRIDGE_HLSLI

// Shared ImageAdjustments2 tangent extension and bounded SDR-LUT transport.
// The sRGB-shaped LUT domain is a working assumption retained from the
// established 06A2 bridge; the LUT sampler and native packed addressing stay
// in each original pixel shader.
float3 LifeIsStrangeExtendImageAdjustments2(
    float3 color,
    float2 image_adjustments,
    float pivot,
    float curve_method) {
  float3 curve_base_input = min(color, pivot.xxx);
  float3 curve_base = curve_base_input
                      / (image_adjustments.x + image_adjustments.y * curve_base_input);
  float pivot_denominator = image_adjustments.x + image_adjustments.y * pivot;
  float pivot_slope = image_adjustments.x / (pivot_denominator * pivot_denominator);
  float3 extended_curve = curve_base + pivot_slope * max(color - pivot.xxx, 0.f);

  if (curve_method > 0.5f) {
    float max_channel_input = max(color.r, max(color.g, color.b));
    if (max_channel_input > pivot) {
      float max_channel_extended = pivot / pivot_denominator
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

float3 LifeIsStrangeCompressForColorGradingLut(float3 extended_curve, out float reconstruction_scale) {
  float3 curve_linear = renodx::color::srgb::DecodeSafe(extended_curve);
  reconstruction_scale = renodx::tonemap::neutwo::ComputeMaxChannelScale(curve_linear);
  return saturate(renodx::color::srgb::EncodeSafe(curve_linear * reconstruction_scale));
}

float3 LifeIsStrangeRestoreAfterColorGradingLut(float3 lut_result, float reconstruction_scale) {
  float3 graded_linear = renodx::color::srgb::DecodeSafe(lut_result);
  float3 reconstructed_linear = renodx::math::DivideSafe(
      graded_linear,
      reconstruction_scale.xxx,
      graded_linear);
  return renodx::color::srgb::EncodeSafe(reconstructed_linear);
}

#endif  // SRC_GAMES_LIFEISSTRANGE_HDR_LUT_BRIDGE_HLSLI
