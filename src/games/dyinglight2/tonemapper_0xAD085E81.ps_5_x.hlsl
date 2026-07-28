#include "./shared.h"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[1];
}

float3 DebugFalseColor(float value) {
  const float log_value = log2(max(0.001, max(0.0, value)));
  if (log_value < 0.0) {
    return lerp(float3(0.0, 0.0, 0.2), float3(0.0, 1.0, 0.3), saturate((log_value + 3.0) / 3.0));
  }
  if (log_value < 1.0) {
    return lerp(float3(0.0, 1.0, 0.3), float3(1.0, 1.0, 0.0), log_value);
  }
  if (log_value < 2.0) {
    return lerp(float3(1.0, 1.0, 0.0), float3(1.0, 0.0, 0.0), log_value - 1.0);
  }
  return lerp(float3(1.0, 0.0, 0.0), float3(1.0, 1.0, 1.0), saturate(log_value - 2.0));
}

void main(
    float4 v0 : SV_POSITION0,
    linear noperspective float2 v1 : TEXCOORD0,
    out float4 o0 : SV_TARGET0) {
  const float4 input_color = t0.SampleLevel(s0_s, v1.xy, 0);

  // The original pass is a per-channel power operation. Bypassing it for
  // this one diagnostic mode establishes whether it remaps HDR highlights
  // after the LUT composite.
  if (RENODX_DEBUG_MODE > 11.5 && RENODX_DEBUG_MODE < 12.5) {
    o0 = input_color;
    return;
  }

  // The same fixed HDR-white value, but after the gamma/power operation.
  // Together with modes 13 and 15 this makes the final flicker boundary
  // identifiable without resource tracing or shader enumeration.
  if (RENODX_DEBUG_MODE > 13.5 && RENODX_DEBUG_MODE < 14.5) {
    o0 = float4(6.25, 6.25, 6.25, 1.0);
    return;
  }

  // These two modes isolate the final game Gamma pass without any resource
  // inspection or readback. A changing image in mode 16 means its input is
  // already changing; a changing full-screen color in mode 17 means the
  // game's power parameter is changing instead.
  if (RENODX_DEBUG_MODE > 15.5 && RENODX_DEBUG_MODE < 16.5) {
    const float input_range = max(input_color.r, max(input_color.g, input_color.b));
    o0 = float4(renodx::draw::RenderIntermediatePass(DebugFalseColor(input_range)), 1.0);
    return;
  }
  if (RENODX_DEBUG_MODE > 16.5 && RENODX_DEBUG_MODE < 17.5) {
    o0 = float4(renodx::draw::RenderIntermediatePass(DebugFalseColor(abs(cb0[0].x))), 1.0);
    return;
  }

  if (RENODX_DEBUG_MODE > 17.5 && RENODX_DEBUG_MODE < 18.5) {
    const float3 gamma_output = exp2(cb0[0].xxx * log2(abs(input_color.rgb)));
    o0.rgb = v1.y < 0.5 ? input_color.rgb : gamma_output;
    o0.a = 1.0;
    return;
  }

  const float3 gamma_output = exp2(cb0[0].xxx * log2(abs(input_color.rgb)));
  if (RENODX_SWAP_CHAIN_USE_HDR10) {
    // DLSS-G intercepts final color before RenoDX's Present callback. Encode
    // here so the real RGB10 backbuffer already contains this frame's PQ
    // output when Streamline captures it.
    renodx::draw::Config config = renodx::draw::BuildConfig();
    config.swap_chain_scaling_nits = RENODX_GRAPHICS_WHITE_NITS;
    config.swap_chain_encoding = renodx::draw::ENCODING_PQ;
    config.swap_chain_output_preset = renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_HDR10;
    o0.rgb = renodx::draw::SwapChainPass(gamma_output, v1.xy, config).rgb;
  } else {
    o0.rgb = gamma_output;
  }
  o0.a = input_color.a;
}
