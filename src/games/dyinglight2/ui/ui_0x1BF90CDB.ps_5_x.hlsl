#include "../shared.h"

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
    linear noperspective float2 v3 : TEXCOORD2,
    out float4 o0 : SV_TARGET0) {
  const float coverage = saturate(t0.Sample(s0_s, v2.xy).w * cb0[0].x + cb0[0].y);
  const float4 ui_color = float4(t1.SampleLevel(s0_s, v3.xy, 0).xyz, coverage);
  o0.rgb = v1.rgb * ui_color.rgb;
  o0.xyz = renodx::color::gamma::DecodeSafe(o0.xyz, 2.2f);
  o0.xyz *= RENODX_GRAPHICS_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS;
  o0.xyz = renodx::color::gamma::EncodeSafe(o0.xyz, 2.2f);

  o0.a = v1.a * ui_color.a;
}
