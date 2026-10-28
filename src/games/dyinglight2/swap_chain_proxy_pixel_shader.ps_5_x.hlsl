#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

// The composite arriving here is LINEAR. That is not what the vanilla chain
// produced on its own: the back buffer view is requested as
// r8g8b8a8_unorm_srgb, and the swapchain upgrade rewrites it to
// r10g10b10a2_unorm, which is not an sRGB format, so the hardware encode the
// game relied on no longer happens. Every custom shader in this folder
// therefore writes linear too, so one decoding setting covers both the vanilla
// shaders and the replacements. Decoding sRGB here decodes a second time and
// crushes mid grey from 0.18 to 0.023, which reads as a near black image.
//
// Paper white. swap_chain_scaling_nits is the nits value that the composite's
// 1.0 maps to, so at the 203 default a full white pixel encodes to exactly 203
// nits no matter what the Windows "SDR content brightness" slider is set to.
// Vanilla does not work that way: it writes SDR and Windows multiplies the
// whole image by that slider, so vanilla white is whatever the slider says and
// is usually well above 203. That, and only that, is why this mod reads darker
// than the game without it. Raise Tone Mapping -> Game Brightness to close the
// gap; the same value also anchors the tone map shoulder upstream, so the two
// stay consistent instead of fighting each other.
float4 main(float4 vpos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  renodx::draw::Config config = renodx::draw::BuildConfig();
  config.swap_chain_decoding = RENODX_SWAP_CHAIN_DECODING;
  config.swap_chain_decoding_color_space = renodx::color::convert::COLOR_SPACE_BT709;
  // Paper white, see above. Must stay equal to the diffuse white the tone
  // mapper normalised against: the shoulder tops out at peak/diffuse, and the
  // final nits are that ratio times this value, so a mismatch either overshoots
  // the user peak or throws away headroom.
  config.swap_chain_scaling_nits = RENODX_DIFFUSE_WHITE_NITS;
  // Leave the clamp open: the tone mapper upstream already handled the user
  // peak, and compressing here would apply the rolloff twice.
  config.swap_chain_clamp_nits = 10000.f;
  config.swap_chain_encoding = renodx::draw::ENCODING_PQ;
  config.swap_chain_output_preset = renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_HDR10;

  const float3 color = t0.Sample(s0, uv).rgb;
  return float4(renodx::draw::SwapChainPass(color, uv, config).rgb, 1.f);
}
