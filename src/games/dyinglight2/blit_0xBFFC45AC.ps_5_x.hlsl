#include "./shared.h"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

void main(
    float4 v0 : SV_POSITION0,
    linear noperspective float2 v1 : TEXCOORD0,
    out float4 o0 : SV_TARGET0) {
  float3 stage_probe;
  if (Dl2RenderLuminanceStageProbe(v1.xy, 5.0, stage_probe)) {
    o0 = float4(stage_probe, 1.0);
    return;
  }

  if (RENODX_DEBUG_MODE > 26.5 && RENODX_DEBUG_MODE < 27.5) {
    o0 = float4(6.25, 6.25, 6.25, 1.0);
    return;
  }

  o0 = t0.SampleLevel(s0_s, v1.xy, 0);
}
