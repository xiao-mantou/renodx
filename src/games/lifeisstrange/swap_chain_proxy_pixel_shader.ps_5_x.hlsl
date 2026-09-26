#include "./shared.h"

Texture2D source_texture : register(t0);
SamplerState source_sampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  uv.y = lerp(uv.y, 1.f - uv.y, CUSTOM_FLIP_UV_Y);
  float4 source_color = source_texture.Sample(source_sampler, uv);
  source_color.rgb = lerp(source_color.rgb, 4.f, step(0.5f, LIFEISSTRANGE_FORCE_PROXY_WHITE));
  return renodx::draw::SwapChainPass(source_color);
}
