#include "./shared.h"

Texture2D source_texture : register(t0);
SamplerState source_sampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  uv.y = lerp(uv.y, 1.f - uv.y, CUSTOM_FLIP_UV_Y);
  // Proxy-only validation: bypass the game frame and send a uniform HDR value
  // through the actual HDR10 output path. RGB=4 corresponds to about 812 nits
  // with the current 203-nit reference white.
  return renodx::draw::SwapChainPass(float3(4.f, 4.f, 4.f));
}
