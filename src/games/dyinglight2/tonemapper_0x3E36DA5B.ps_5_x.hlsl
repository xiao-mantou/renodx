#include "./shared.h"

Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[2];
}

// The game's pass applies exposure and then compresses the scene to [0, 1].
// RenoDX retains that exposed scene signal for the following LUT / HDR pass.
void main(
    float4 v0 : SV_POSITION0,
    linear noperspective float2 v1 : TEXCOORD0,
    out float4 o0 : SV_TARGET0) {
  float exposure = t1.SampleLevel(s0_s, float2(0, 0), 0).x;
  float4 scene = t0.SampleLevel(s0_s, v1.xy, 0);

  if (RENODX_TONE_MAP_TYPE != 0.f) {
    o0 = float4(scene.rgb * exposure, scene.a);
    return;
  }

  // Original game SDR tonemapper, retained for the Vanilla setting.
  float3 color = scene.rgb * exposure;
  color *= float3(0.600000024, 0.600000024, 0.600000024);
  float3 numerator = cb0[0].xxx * color + cb0[0].yyy;
  numerator *= color;
  float3 denominator = cb0[0].zzz * color + cb0[0].www;
  denominator = color * denominator + cb0[1].xxx;
  o0 = float4(saturate(numerator / denominator), scene.a);
}
