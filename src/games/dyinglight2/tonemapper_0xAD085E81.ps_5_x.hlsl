#include "./shared.h"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[1];
}

void main(
    float4 v0 : SV_POSITION0,
    linear noperspective float2 v1 : TEXCOORD0,
    out float4 o0 : SV_TARGET0) {
  const float4 input_color = t0.SampleLevel(s0_s, v1.xy, 0);

  // The original pass is a per-channel power operation. Bypassing it for
  // this one diagnostic mode establishes whether it remaps HDR highlights
  // after the LUT composite.
  if (RENODX_DEBUG_MODE > 11.5 && RENODX_DEBUG_MODE < 12.5) {
    o0 = input_color;
    return;
  }

  o0.rgb = exp2(cb0[0].xxx * log2(abs(input_color.rgb)));
  o0.a = input_color.a;
}
