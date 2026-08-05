#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

float4 main(float4 vpos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  // Streamline's RGB10 tray is interpreted as normalized Linear/BT.709. Keep
  // that semantic here and restore the absolute nit scale in the final proxy.
  const float range = max(RENODX_PEAK_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS, 1e-4f);
  return float4(saturate(t0.Sample(s0, uv).rgb / range), 1.f);
}
