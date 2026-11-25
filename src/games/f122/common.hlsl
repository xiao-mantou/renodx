#ifndef SRC_GAMES_F12022_COMMON_HLSL_
#define SRC_GAMES_F12022_COMMON_HLSL_

#include "./shared.h"

// F1 22 (EGO) filmic curve: Uncharted2/Hable variant with a ^1.1 shoulder.
// Exactly maps input radiance to filmic contrast, reaching 1.0 at x = 0.856.
float3 F1FilmicCurve(float3 color) {
  float3 color_06 = color * 0.6f;
  float3 numerator = (color * 9.480000495910645f + 2.119999885559082f) * color_06;
  float3 denominator = (color * 0.7200000286102295f + 5.920000076293945f) * color_06 + 1.899999976158142f;
  return exp2(log2(max(numerator / denominator, 9.999999747378752e-06f)) * 1.100000023841858f);
}

// Vanilla color transform: sRGB encode -> colorXFormLUT (3x4) -> sRGB decode.
float3 F1ColorXFormLUT(float3 color, float4 color_xform_lut[4]) {
  float3 encoded = renodx::color::srgb::EncodeSafe(color);
  float3 transformed = float3(
      mad(color_xform_lut[2].x, encoded.b, mad(color_xform_lut[1].x, encoded.g, color_xform_lut[0].x * encoded.r)) + color_xform_lut[3].x,
      mad(color_xform_lut[2].y, encoded.b, mad(color_xform_lut[1].y, encoded.g, color_xform_lut[0].y * encoded.r)) + color_xform_lut[3].y,
      mad(color_xform_lut[2].z, encoded.b, mad(color_xform_lut[1].z, encoded.g, color_xform_lut[0].z * encoded.r)) + color_xform_lut[3].z);
  return renodx::color::srgb::DecodeSafe(transformed);
}

// Canonical N2 Max-Channel SDR LUT Bridge (per RenoDX skill handle-sdr-tonemap-lut):
// - For diffuse colors (max channel <= 1.0), passes through 100% bit-for-bit to vanilla LUT.
// - For HDR colors (max channel > 1.0), proportionally scales along the chromaticity ray into [0, 1],
//   samples the color transform at the SDR boundary, and reconstructs the HDR signal preserving ratios.
float3 F1ApplySdrLutBridge(float3 color_hdr, float4 color_xform_lut[4]) {
  float max_channel = max(1.0f, max(color_hdr.r, max(color_hdr.g, color_hdr.b)));
  float3 color_proxy = color_hdr / max_channel;
  float3 graded_proxy = F1ColorXFormLUT(color_proxy, color_xform_lut);
  return graded_proxy * max_channel;
}

// Continuous C1 rational soft shoulder above paper white (1.0) to peak_ratio.
// Exactly continuous in value (1.0) and first derivative (1.0) at the paper white boundary,
// smoothly rolling off highlights to the display peak without hard clipping or kinks.
float F1SoftShoulder(float y, float peak_ratio) {
  if (y <= 1.0f) return y;
  float z = y - 1.0f;
  float m = peak_ratio - 1.0f;
  return 1.0f + (m * z) / (z + m);
}

// Applies the soft shoulder based on BT.709 luminance to preserve exact hue and saturation ratios.
float3 F1ApplyHdrSoftShoulder(float3 color, float peak_ratio) {
  float y = renodx::color::y::from::BT709(color);
  if (y <= 0.0f) return color;
  float y_mapped = F1SoftShoulder(y, peak_ratio);
  return color * (y_mapped / y);
}

#endif  // SRC_GAMES_F12022_COMMON_HLSL_
