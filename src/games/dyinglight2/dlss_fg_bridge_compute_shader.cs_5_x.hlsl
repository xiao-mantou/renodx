#include "./shared.h"

Texture2D<float4> t0 : register(t0);
RWTexture2D<float4> u0 : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint width = 0;
  uint height = 0;
  t0.GetDimensions(width, height);
  if (dispatch_thread_id.x >= width || dispatch_thread_id.y >= height) return;

  // The FP16 clone contains the linear scene signal. RGB10 UAV stores are
  // normalized, so divide by the fixed bridge reference before the copy into
  // Streamline's HDR10 tray. The final proxy restores the absolute peak.
  const float range = max(RENODX_PEAK_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS, 1e-4f);
  const float3 value = saturate(t0.Load(int3(dispatch_thread_id.xy, 0)).rgb / range);
  u0[dispatch_thread_id.xy] = float4(value, 1.f);
}
