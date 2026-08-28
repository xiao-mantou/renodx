#include "./ui.hlsli"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[1];
}

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    float4 v2 : TEXCOORD1,
    out float4 o0 : SV_TARGET0) {
  const float4 edge = v2.zwzw * float4(1.0, 1.0, -1.0, -1.0) + float4(0.0, 0.0, 1.0, 1.0);
  if (any(edge < 0.0)) discard;

  const float4 texture_color = t0.Sample(s0_s, v2.xy);
  const float alpha = texture_color.x * cb0[0].z + cb0[0].w;
  o0.rgb = FinalizeDL2UI(v1.rgb);
  o0.a = v1.a * alpha;
}
