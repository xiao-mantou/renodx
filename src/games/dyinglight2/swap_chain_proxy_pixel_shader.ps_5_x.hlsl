#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

// Ported from codex/dl2-hdr-next, where this exact configuration was verified
// against vanilla with a six-way proxy grid.
//
// The previous version decoded gamma 2.2 before the PQ encode. The composite
// arriving here is already linear BT.709: the view upgrade maps the game's
// r8g8b8a8_unorm_srgb views onto plain FP16 views, so no sRGB transfer is
// applied on the write, and 0xAD085E81 already ran its exp2(cb0[0].x * log2).
// Decoding a second time turned mid grey 0.18 into 0.0233, which is why the
// whole image came out far too dark.
//
// Scaling uses DL2's fixed 203-nit reference white, which is a unit conversion
// and not the runtime Game/UI brightness setting. The clamp is left open
// because the tone mapper upstream already handled the user peak; clamping
// again here would compress the finished image twice.
float4 main(float4 vpos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  renodx::draw::Config config = renodx::draw::BuildConfig();
  config.swap_chain_decoding = renodx::draw::ENCODING_NONE;
  config.swap_chain_decoding_color_space = renodx::color::convert::COLOR_SPACE_BT709;
  config.swap_chain_scaling_nits = RENODX_GRAPHICS_WHITE_NITS;
  config.swap_chain_clamp_nits = 10000.f;
  config.swap_chain_encoding = renodx::draw::ENCODING_PQ;
  config.swap_chain_output_preset = renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_HDR10;

  const float3 color = t0.Sample(s0, uv).rgb;
  return float4(renodx::draw::SwapChainPass(color, uv, config).rgb, 1.f);
}
