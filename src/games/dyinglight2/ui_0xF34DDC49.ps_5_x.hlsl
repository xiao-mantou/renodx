#include "./ui.hlsli"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    float2 v2 : TEXCOORD1,
    out float4 o0 : SV_TARGET0) {
  const float4 ui_color = v1 * t0.Sample(s0_s, v2.xy);
  o0.rgb = FinalizeDL2UI(ui_color.rgb);
  o0.a = ui_color.a;
}
