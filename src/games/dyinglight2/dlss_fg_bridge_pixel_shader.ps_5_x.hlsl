#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

float4 main(float4 vpos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  renodx::draw::Config config = renodx::draw::BuildConfig();
  config.swap_chain_decoding = renodx::draw::ENCODING_NONE;
  config.swap_chain_decoding_color_space = renodx::color::convert::COLOR_SPACE_BT709;
  config.swap_chain_scaling_nits = RENODX_GRAPHICS_WHITE_NITS;
  config.swap_chain_clamp_nits = 10000.f;
  config.swap_chain_encoding = renodx::draw::ENCODING_PQ;
  config.swap_chain_output_preset = renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_HDR10;
  return float4(renodx::draw::SwapChainPass(t0.Sample(s0, uv).rgb, uv, config).rgb, 1.f);
}
