#ifndef SRC_GAMES_F12022_COMMON_HLSL_
#define SRC_GAMES_F12022_COMMON_HLSL_

#include "./shared.h"

// Raw scene value where the EGO filmic curve reaches paper white (F1FilmicCurve(0.858) == 1.0).
static const float F1_PAPER_WHITE = 0.858f;

// F1 22 (EGO) filmic curve: Uncharted2/Hable variant with a ^1.1 shoulder.
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

#endif  // SRC_GAMES_F12022_COMMON_HLSL_
