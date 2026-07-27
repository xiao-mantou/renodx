#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

float4 main(float4 vpos: SV_POSITION, float2 uv: TEXCOORD0)
    : SV_TARGET {
  renodx::draw::Config config = renodx::draw::BuildConfig();
  // config.swap_chain_scaling_nits = RENODX_GRAPHICS_WHITE_NITS;  // Use default 1.0
  if (RENODX_DEBUG_MODE > 4.5 && RENODX_DEBUG_MODE < 5.5) {
    return float4(renodx::draw::SwapChainPass(float3(6.25, 0.0, 0.0), uv, config).rgb, 1.f);
  }

  if (RENODX_DEBUG_MODE > 14.5 && RENODX_DEBUG_MODE < 15.5) {
    return float4(renodx::draw::SwapChainPass(float3(6.25, 6.25, 6.25), uv, config).rgb, 1.f);
  }
  float3 output = renodx::draw::SwapChainPass(t0.Sample(s0, uv).rgb, uv, config).rgb;
  if (RENODX_CLAMP_SWAPCHAIN_OUTPUT > 0.5f) {
    output = saturate(output);
  }
  return float4(output, 1.f);
}
