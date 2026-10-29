#include "../shared.h"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    float2 v2 : TEXCOORD1,
    out float4 o0 : SV_TARGET0) {
  const float4 ui_color = v1 * t0.Sample(s0_s, v2.xy);
  o0.rgb = ui_color.rgb;
  // The composite is linear at this point (0x268BAB6D ends linear), and the
  // swapchain upgrade to r10g10b10a2_unorm stops the hardware from encoding on
  // write, so the buffer the UI blends into holds linear light. A gamma round
  // trip is therefore both unnecessary and wrong: decode -> scale -> encode
  // collapses to a gain of (ui/game)^2.2, which over-drives the UI slider by a
  // power of 2.2 and is only identity when the two nits values are equal.
  // A plain linear gain makes the slider mean what it says and keeps the UI
  // and the scene sharing a single decode at the swapchain.
  o0.xyz *= RENODX_GRAPHICS_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS;

  o0.a = ui_color.a;
}
