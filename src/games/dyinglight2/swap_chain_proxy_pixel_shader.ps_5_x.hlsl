#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

float4 main(float4 vpos: SV_POSITION, float2 uv: TEXCOORD0)
    : SV_TARGET {
  renodx::draw::Config config = renodx::draw::BuildConfig();
  // DL2's FP16 bridge is relative to its fixed 203-nit reference white. This
  // is a unit conversion, not the runtime Game/UI brightness setting.
  config.swap_chain_scaling_nits = RENODX_GRAPHICS_WHITE_NITS;
  // Tone mapping already handled the user peak. Do not compress the completed
  // image a second time in the final HDR10 encoding pass.
  config.swap_chain_clamp_nits = 10000.f;
  // Encoding follows the runtime Swap Chain Format setting so the shader can
  // never disagree with the container DXGI actually created.
  config.swap_chain_encoding = RENODX_SWAP_CHAIN_USE_HDR10
                                   ? renodx::draw::ENCODING_PQ
                                   : renodx::draw::ENCODING_SCRGB;
  config.swap_chain_output_preset = RENODX_SWAP_CHAIN_USE_HDR10
                                        ? renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_HDR10
                                        : renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_SCRGB;
  if (RENODX_SWAP_CHAIN_SOURCE_IS_HDR10) {
    config.swap_chain_decoding = renodx::draw::ENCODING_PQ;
    config.swap_chain_decoding_color_space = renodx::color::convert::COLOR_SPACE_BT2020;
    // PQ decode returns absolute nits, so this source needs no reference-white
    // conversion before being encoded again.
    config.swap_chain_scaling_nits = 1.f;
  }

  if (RENODX_DEBUG_MODE > 4.5 && RENODX_DEBUG_MODE < 5.5) {
    return float4(renodx::draw::SwapChainPass(float3(6.25, 0.0, 0.0), uv, config).rgb, 1.f);
  }

  if (RENODX_DEBUG_MODE > 14.5 && RENODX_DEBUG_MODE < 15.5) {
    return float4(renodx::draw::SwapChainPass(float3(6.25, 6.25, 6.25), uv, config).rgb, 1.f);
  }

  // Diagnostic only: preserve the normal decode, nit scaling, and PQ encode,
  // but bypass the HDR10 preset's gamut-compression stage. This isolates
  // whether it magnifies the small chroma difference in DLSS SR output.
  if (RENODX_DEBUG_MODE > 23.5 && RENODX_DEBUG_MODE < 24.5) {
    config.swap_chain_output_preset = renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_NONE;
    config.swap_chain_clamp_color_space = renodx::color::convert::COLOR_SPACE_NONE;
    config.swap_chain_compress_color_space = renodx::color::convert::COLOR_SPACE_NONE;
    config.swap_chain_encoding = renodx::draw::ENCODING_PQ;
    config.swap_chain_encoding_color_space = renodx::color::convert::COLOR_SPACE_BT2020;
  }

  return float4(renodx::draw::SwapChainPass(t0.Sample(s0, uv).rgb, uv, config).rgb, 1.f);
}
