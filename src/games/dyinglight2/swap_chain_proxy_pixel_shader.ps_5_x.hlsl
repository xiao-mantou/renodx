#include "./shared.h"

SamplerState sourceSampler_s : register(s0);
Texture2D<float4> sourceTexture : register(t0);

void main(
    float4 vpos: SV_Position,
    float2 texcoord: TEXCOORD,
    out float4 output: SV_Target0) {
  float4 color = sourceTexture.Sample(sourceSampler_s, texcoord.xy);
  color.a = saturate(color.a);

  color.rgb = renodx::color::gamma::DecodeSafe(color.rgb, 2.2f);
  color.rgb = renodx::color::bt2020::from::BT709(color.rgb);
  color.rgb = renodx::color::pq::EncodeSafe(color.rgb, RENODX_DIFFUSE_WHITE_NITS);

  output.rgba = color;
}