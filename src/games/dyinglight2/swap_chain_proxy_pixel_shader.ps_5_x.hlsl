#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

float4 main(float4 vpos : SV_POSITION, float2 uv : TEXCOORD0)
    : SV_TARGET {
  renodx::draw::Config config = renodx::draw::BuildConfig();
  // The swapchain proxy samples DL2's linear BT.709 composite. BuildConfig's
  // generic default inherits the intermediate sRGB encoding, which decodes the
  // already-linear composite a second time and shifts the common menu/scene
  // color. The six-way proxy grid confirmed Linear + BT.709 against vanilla.
  config.swap_chain_decoding = renodx::draw::ENCODING_NONE;
  config.swap_chain_decoding_color_space = renodx::color::convert::COLOR_SPACE_BT709;
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
  if (RENODX_SWAP_CHAIN_SOURCE_OVERRIDE) {
    const uint source_semantic = (uint)(RENODX_DLSS_FG_SOURCE_SEMANTIC + 0.5f);
    const bool source_is_pq = source_semantic == 1u;
    config.swap_chain_decoding = source_is_pq
                                     ? renodx::draw::ENCODING_PQ
                                     : renodx::draw::ENCODING_NONE;
    config.swap_chain_decoding_color_space = source_is_pq
                                                 ? renodx::color::convert::COLOR_SPACE_BT2020
                                                 : renodx::color::convert::COLOR_SPACE_BT709;
    // A normalized linear RGB10 source cannot exceed 1.0. The previous
    // 203-nit assumption therefore imposed the observed 203-nit ceiling.
    // Keep transfer function and gamut fixed while testing its absolute scale.
    const float linear_source_scale = source_semantic == 11u   ? RENODX_PEAK_WHITE_NITS
                                      : source_semantic == 3u  ? 400.f
                                      : source_semantic == 4u  ? 600.f
                                      : source_semantic == 5u  ? 800.f
                                      : source_semantic == 6u  ? 1000.f
                                      : source_semantic == 7u  ? 1200.f
                                      : source_semantic == 8u  ? 1600.f
                                      : source_semantic == 9u  ? RENODX_PEAK_WHITE_NITS
                                      : source_semantic == 10u ? 4000.f
                                                               : 203.f;
    config.swap_chain_scaling_nits = source_is_pq ? 1.f : linear_source_scale;
  }

  float3 stage_probe;
  if (Dl2RenderLuminanceStageProbe(uv, 6.0, stage_probe)) {
    return float4(renodx::draw::SwapChainPass(stage_probe, uv, config).rgb, 1.f);
  }

  if (RENODX_DEBUG_MODE > 4.5 && RENODX_DEBUG_MODE < 5.5) {
    return float4(renodx::draw::SwapChainPass(float3(6.25, 0.0, 0.0), uv, config).rgb, 1.f);
  }

  if (RENODX_DEBUG_MODE > 14.5 && RENODX_DEBUG_MODE < 15.5) {
    return float4(renodx::draw::SwapChainPass(float3(6.25, 6.25, 6.25), uv, config).rgb, 1.f);
  }

  // Diagnostic only: preserve the normal decode, nit scaling, and PQ encode,
  // but bypass the preset's gamut-compression stage. Keep the actual
  // swapchain container (HDR10/PQ or scRGB) and clamp converted channels to
  // non-negative values before encoding; otherwise out-of-gamut BT.2020
  // components can reach PQ as negative values and produce an all-white/NaN
  // diagnostic frame.
  if (RENODX_DEBUG_MODE > 23.5 && RENODX_DEBUG_MODE < 24.5) {
    config.swap_chain_output_preset = renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_NONE;
    config.swap_chain_clamp_color_space = RENODX_SWAP_CHAIN_USE_HDR10
                                              ? renodx::color::convert::COLOR_SPACE_BT2020
                                              : renodx::color::convert::COLOR_SPACE_BT709;
    config.swap_chain_compress_color_space = renodx::color::convert::COLOR_SPACE_NONE;
    config.swap_chain_encoding = RENODX_SWAP_CHAIN_USE_HDR10
                                     ? renodx::draw::ENCODING_PQ
                                     : renodx::draw::ENCODING_SCRGB;
    config.swap_chain_encoding_color_space = RENODX_SWAP_CHAIN_USE_HDR10
                                                 ? renodx::color::convert::COLOR_SPACE_BT2020
                                                 : renodx::color::convert::COLOR_SPACE_BT709;
  }

  // Six-way final-proxy diagnostic. Each tile repeats the full source image so
  // transfer-function and source-gamut assumptions can be compared in one
  // capture while sharing the exact same HDR10/PQ output conversion.
  if (RENODX_DEBUG_MODE > 24.5 && RENODX_DEBUG_MODE < 25.5) {
    const float2 grid_size = float2(3.f, 2.f);
    const uint column = min((uint)(uv.x * grid_size.x), 2u);
    const uint row = min((uint)(uv.y * grid_size.y), 1u);
    const float2 tile_uv = frac(uv * grid_size);

    config.swap_chain_decoding = column == 0u
                                     ? renodx::draw::ENCODING_NONE
                                 : column == 1u
                                     ? renodx::draw::ENCODING_SRGB
                                     : renodx::draw::ENCODING_GAMMA_2_2;
    config.swap_chain_decoding_color_space = row == 0u
                                                 ? renodx::color::convert::COLOR_SPACE_BT709
                                                 : renodx::color::convert::COLOR_SPACE_BT2020;
    return float4(renodx::draw::SwapChainPass(t0.Sample(s0, tile_uv).rgb, tile_uv, config).rgb, 1.f);
  }

  if (RENODX_DEBUG_MODE > 30.5 && RENODX_DEBUG_MODE < 31.5) {
    float3 input_color = t0.Sample(s0, uv).rgb;
    const float luminance = renodx::color::y::from::BT709(max(input_color, 0.0));
    const uint quadrant = (uv.x >= 0.5 ? 1u : 0u) + (uv.y >= 0.5 ? 2u : 0u);
    const float strengths[4] = {1.0, 1.1, 1.2, 1.3};
    input_color = lerp(luminance.xxx, input_color, strengths[quadrant]);
    return float4(renodx::draw::SwapChainPass(input_color, uv, config).rgb, 1.f);
  }

  // Final-proxy source probe. This is intentionally before any decode,
  // gamut conversion, scaling, or PQ encoding. The four quadrants expose
  // independent range summaries so an upstream clamp can be distinguished
  // from a downstream display-interpretation problem in one frame.
  if (RENODX_DEBUG_MODE > 35.5 && RENODX_DEBUG_MODE < 36.5) {
    const float3 source = max(t0.Sample(s0, uv).rgb, 0.0);
    const float max_channel = max(source.r, max(source.g, source.b));
    const float luminance = renodx::color::y::from::BT709(source);
    const float2 local_uv = frac(uv * 2.0);
    const uint quadrant = (uv.x >= 0.5 ? 1u : 0u) + (uv.y >= 0.5 ? 2u : 0u);
    const uint channel = min((uint)(local_uv.x * 3.0), 2u);
    float value = quadrant == 0u   ? max_channel
                  : quadrant == 1u ? luminance
                  : quadrant == 2u ? source[channel]
                                   : max_channel;
    // Log-like bands retain useful separation after the final HDR encoding.
    const float3 band = value < 0.01   ? float3(0.0, 0.0, 1.0)
                        : value < 0.05 ? float3(0.0, 1.0, 1.0)
                        : value < 0.2  ? float3(0.0, 1.0, 0.0)
                        : value < 0.5  ? float3(1.0, 1.0, 0.0)
                        : value < 1.0  ? float3(1.0, 0.5, 0.0)
                                       : float3(1.0, 0.0, 0.0);
    // Bottom-right retains the unmodified source image as a spatial reference.
    if (quadrant == 3u) {
      return float4(renodx::draw::SwapChainPass(source, uv, config).rgb, 1.f);
    }
    const float channel_edge = quadrant == 2u
                                       && (abs(frac(local_uv.x * 3.0)) < 0.04)
                                   ? 0.35
                                   : 1.0;
    const float edge = (local_uv.x < 0.025 || local_uv.y < 0.025) ? 0.35 : channel_edge;
    return float4(renodx::draw::SwapChainPass(band * edge, uv, config).rgb, 1.f);
  }

  return float4(renodx::draw::SwapChainPass(t0.Sample(s0, uv).rgb, uv, config).rgb, 1.f);
}
