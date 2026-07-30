#include "./ui.hlsli"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[1];
}

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    float2 v2 : TEXCOORD1,
    out float4 o0 : SV_TARGET0) {
  const float4 texture_color = t0.Sample(s0_s, v2.xy);
  const float4 base_color = float4(1.0, 1.0, 1.0, texture_color.x);
  const float4 ui_color = v1 * (base_color * cb0[0].w + texture_color * cb0[0].z);
  o0.rgb = FinalizeDL2UI(ui_color.rgb);
  o0.a = ui_color.a;
}
