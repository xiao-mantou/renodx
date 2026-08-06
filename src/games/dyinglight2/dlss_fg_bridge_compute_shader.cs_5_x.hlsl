#include "./shared.h"

Texture2D<float4> t0 : register(t0);
RWTexture2D<float4> u0 : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint width = 0;
  uint height = 0;
  t0.GetDimensions(width, height);
  if (dispatch_thread_id.x >= width || dispatch_thread_id.y >= height) return;

  // DLSS-G requires RGB10 in the HDR10/BT.2100 contract. The FP16 clone is
  // linear BT.709 with one unit equal to DL2's 203-nit graphics white.
  const float3 linear_bt709 = max(0.f, t0.Load(int3(dispatch_thread_id.xy, 0)).rgb);
  const float3 linear_bt2020 = max(0.f, renodx::color::bt2020::from::BT709(linear_bt709));
  const float3 pq_bt2020 = renodx::color::pq::EncodeSafe(
      linear_bt2020, RENODX_GRAPHICS_WHITE_NITS);
  u0[dispatch_thread_id.xy] = float4(saturate(pq_bt2020), 1.f);
}
