#include "./shared.h"

float3 FinalizeDL2UI(float3 color_srgb) {
  const float ui_scale = RENODX_UI_WHITE_NITS / 203.0;
  return renodx::color::srgb::DecodeSafe(max(color_srgb, 0.0)) * ui_scale;
}
