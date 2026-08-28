#include "./ui.hlsli"

Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[2];
}

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    float4 v2 : TEXCOORD1,
    out float4 o0 : SV_TARGET0) {
  const float2 edge0 = cb0[1].xx * v2.zw;
  const float2 edge1 = 1.0 - v2.zw * cb0[1].xx;
  if (any(edge0 < 0.0) || any(edge1 < 0.0)) discard;

  const float4 texture_color = t0.Sample(s0_s, v2.xy);
  const float alpha = texture_color.x * cb0[0].z + cb0[0].w;
  const float mask = t1.Sample(s1_s, v2.zw).x;
  o0.rgb = FinalizeDL2UI(v1.rgb);
  o0.a = v1.a * alpha * mask;
}
