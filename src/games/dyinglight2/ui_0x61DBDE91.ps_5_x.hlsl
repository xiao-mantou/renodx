#include "./ui.hlsli"

Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[2];
}

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    float4 v2 : TEXCOORD1,
    out float4 o0 : SV_TARGET0) {
  const float2 edge0 = cb0[0].xx * v2.zw;
  const float2 edge1 = 1.0 - v2.zw * cb0[0].xx;
  if (any(edge0 < 0.0) || any(edge1 < 0.0)) discard;

  const float mask = saturate(t1.Sample(s0_s, v2.zw).x * cb0[1].y + cb0[1].x);
  const float4 ui_color = v1 * t0.Sample(s0_s, v2.xy);
  o0.rgb = FinalizeDL2UI(ui_color.rgb * mask);
  o0.a = ui_color.a;
}
