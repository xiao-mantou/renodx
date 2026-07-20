#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

float4 main(float4 vpos: SV_POSITION, float2 uv: TEXCOORD0)
    : SV_TARGET {
  using namespace renodx::draw;

  Config config = BuildConfig();
  config.swap_chain_decoding = ENCODING_SRGB;
  config.swap_chain_scaling_nits = RENODX_DIFFUSE_WHITE_NITS;
  config.swap_chain_encoding = ENCODING_SCRGB;
  config.swap_chain_output_preset = SWAP_CHAIN_OUTPUT_PRESET_NONE;

  return float4(SwapChainPass(t0.Sample(s0, uv).rgb, uv, config).rgb, 1.f);
}
