#include "../shared.h"

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
  const float2 mask = t0.Sample(s0_s, v2.xy).xw;
  o0.rgb = v1.rgb;
  o0.xyz = renodx::color::gamma::DecodeSafe(o0.xyz, 2.2f);
  o0.xyz *= RENODX_GRAPHICS_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS;
  o0.xyz = renodx::color::gamma::EncodeSafe(o0.xyz, 2.2f);

  o0.a = v1.a * dot(mask.yx, cb0[0].zw);
}
