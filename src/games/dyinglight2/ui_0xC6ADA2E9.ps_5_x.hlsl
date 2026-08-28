#include "./ui.hlsli"

Texture2D<float4> t2 : register(t2);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[1];
}

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    float4 v2 : TEXCOORD1,
    float2 v3 : TEXCOORD3,
    out float4 o0 : SV_TARGET0) {
  const float2 edge0 = cb0[0].xx * v2.zw;
  const float2 edge1 = 1.0 - v2.zw * cb0[0].xx;
  const float2 edge2 = cb0[0].yy * v3.xy;
  const float2 edge3 = 1.0 - v3.xy * cb0[0].yy;
  if (any(edge0 < 0.0) || any(edge1 < 0.0)
      || any(edge2 < 0.0) || any(edge3 < 0.0)) discard;

  const float mask0 = t1.Sample(s0_s, v2.zw).x;
  const float4 texture_color = t0.Sample(s0_s, v2.xy);
  const float4 ui_color = v1 * texture_color;
  const float mask1 = t2.Sample(s0_s, v3.xy).x;
  o0.rgb = FinalizeDL2UI(ui_color.rgb);
  o0.a = ui_color.a * mask0 * mask1;
}
