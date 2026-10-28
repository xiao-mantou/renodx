#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

// The transfer function of the composite arriving here is not obvious from the
// code alone: 0x268BAB6D ends with srgb::EncodeSafe, the ui_* writers decode,
// scale and encode again, and 0xAD085E81 then applies exp2(cb0[0].x * log2).
// Whether that lands linear or still sRGB-encoded depends on cb0[0].x, so it
// is exposed as a setting instead of being hardcoded. Getting it wrong shows
// up as a uniform brightness shift: treating an sRGB-encoded composite as
// linear over-brightens everything and blows out white UI, decoding a linear
// composite crushes mid grey (0.18 -> 0.0233).
//
// Scaling uses DL2's fixed 203-nit reference white, a unit conversion and not
// the runtime Game/UI brightness setting. The clamp is left open because the
// tone mapper upstream already handled the user peak.
float4 main(float4 vpos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  renodx::draw::Config config = renodx::draw::BuildConfig();
  config.swap_chain_decoding = RENODX_SWAP_CHAIN_DECODING;
  config.swap_chain_decoding_color_space = renodx::color::convert::COLOR_SPACE_BT709;
  // Paper white. This is the single knob that decides how bright 100% SDR
  // white comes out: scaling_nits is the nits value that the composite's 1.0
  // maps to, so at the 203 default a full white pixel encodes to 203 nits no
  // matter what the Windows "SDR content brightness" slider is set to. That is
  // why the mod looks darker than the game without it whenever the slider sits
  // above 203. Use Tone Mapping -> Game Brightness to raise it until the image
  // matches the no-addon output.
  config.swap_chain_scaling_nits = RENODX_DIFFUSE_WHITE_NITS;
  // Leave the clamp open: the tone mapper upstream already handled the user
  // peak, and compressing here would apply the rolloff twice.
  config.swap_chain_clamp_nits = 10000.f;
  config.swap_chain_encoding = renodx::draw::ENCODING_PQ;
  config.swap_chain_output_preset = renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_HDR10;

  const float3 color = t0.Sample(s0, uv).rgb;
  return float4(renodx::draw::SwapChainPass(color, uv, config).rgb, 1.f);
}
