#include "./shared.h"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

void main(
    float4 v0 : SV_POSITION0,
    linear noperspective float2 v1 : TEXCOORD0,
    out float4 o0 : SV_TARGET0) {
  const float4 input_color = t0.SampleLevel(s0_s, v1.xy, 0);

  // The upper half preserves this copy pass's true input. The lower half is
  // independent HDR white. If the lower half flickers, the instability is
  // after this pass; if only the upper half flickers, its input is unstable.
  if (RENODX_DEBUG_MODE > 18.5 && RENODX_DEBUG_MODE < 19.5) {
    o0 = v1.y < 0.5 ? input_color : float4(6.25, 6.25, 6.25, 1.0);
    return;
  }

  o0 = input_color;
}
