#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

float4 main(float4 vpos: SV_POSITION, float2 uv: TEXCOORD0)
    : SV_TARGET {
  renodx::draw::Config config = renodx::draw::BuildConfig();
  config.swap_chain_scaling_nits = RENODX_GRAPHICS_WHITE_NITS;
  config.swap_chain_encoding = renodx::draw::ENCODING_SCRGB;
  config.swap_chain_output_preset = renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_NONE;

  // Diagnostic only: scRGB 1.0 is 80 nits, so this is a 500-nit red patch.
  // It verifies the actual final-present shader independently of DL2's scene
  // resources, exposure, and tone-mapping passes.
  if (RENODX_DEBUG_MODE > 4.5 && RENODX_DEBUG_MODE < 5.5) {
    return float4(6.25, 0.0, 0.0, 1.0);
  }

  return float4(renodx::draw::SwapChainPass(t0.Sample(s0, uv).rgb, uv, config).rgb, 1.f);
}
