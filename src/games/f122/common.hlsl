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

// Exponential shoulder with finite highlight clip (Frostbite / RenoDX standard).
// - For x <= 1.0 (at or below paper white): 100% uncompressed linear (f(x) = x).
// - For x > 1.0: smoothly rolls off highlights to reach peak_ratio at input = clip.
float F1ExponentialShoulder(float x, float peak_ratio, float clip = 5.5f) {
  if (x <= 1.0f) return x;
  float rolloff_size = peak_ratio - 1.0f;
  float overage = -(x - 1.0f);
  float clip_size = 1.0f - clip;
  float rolloff_val = 1.0f - exp(overage / rolloff_size);
  float clip_val = 1.0f - exp(clip_size / rolloff_size);
  return 1.0f + rolloff_size * (rolloff_val / clip_val);
}

// Applies the shoulder to HDR color.
// Per-channel compression naturally desaturates highlights towards white
// (matching vanilla SDR blowout and preventing oversaturated / unnatural highlight casts).
float3 F1ApplyHdrSoftShoulder(float3 color, float peak_ratio) {
  float3 tonemapped;
  tonemapped.r = F1ExponentialShoulder(color.r, peak_ratio, 5.5f);
  tonemapped.g = F1ExponentialShoulder(color.g, peak_ratio, 5.5f);
  tonemapped.b = F1ExponentialShoulder(color.b, peak_ratio, 5.5f);
  return tonemapped;
}

#endif  // SRC_GAMES_F12022_COMMON_HLSL_
