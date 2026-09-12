/*
 * Copyright (C) 2026 xiao-mantou
 * SPDX-License-Identifier: MIT
 */

// Port of F1 22's CAS sharpening pass (0x2EA7EE8A).
// Identical math to the original, except the final saturate() is replaced with max(0, .),
// so HDR values above 1.0 survive to the final copy while undershoot is still clamped.
Texture2D<float4> srvInputTexture : register(t0);

RWTexture2D<float4> uavOutputTexture : register(u0);

cbuffer cb : register(b0) {
  uint4 const0 : packoffset(c000.x);
  uint4 const1 : packoffset(c001.x);
  uint4 const2 : packoffset(c002.x);
};

float3 ApplyCas(float3 center, float3 up, float3 left, float3 right, float3 down, float strength) {
  float3 mn = min(min(left, min(right, min(up, down))), center);
  float3 mx = max(max(left, max(right, max(up, down))), center);
  float3 weight = saturate(mn * (1.0f - mx) * asfloat(2129690299u - asuint(mx)));
  float3 a = asfloat((asuint(weight) >> 1) + 532432441u) * strength;
  float3 b = a * 4.0f + 1.0f;
  float3 b_recip = asfloat(2129764351u - asuint(b));
  return max(0.0f, b_recip * (a * (up + left + right + down) + center) * (2.0f - b_recip * b));
}

[numthreads(64, 1, 1)]
void main(
  uint3 SV_GroupID : SV_GroupID,
  uint3 SV_GroupThreadID : SV_GroupThreadID
) {
  uint x0 = ((SV_GroupThreadID.x >> 1) & 7u) | (SV_GroupID.x << 4);
  uint y0 = (((SV_GroupThreadID.x >> 3) & 6u) | (SV_GroupThreadID.x & 1u)) | (SV_GroupID.y << 4);
  uint x1 = x0 | 8u;
  uint y1 = y0 | 8u;
  float strength = asfloat(const1.x);

  float3 c00 = srvInputTexture.Load(int3(x0, y0, 0)).rgb;
  uavOutputTexture[int2(x0, y0)] = float4(
      ApplyCas(
          c00,
          srvInputTexture.Load(int3(x0, y0 - 1u, 0)).rgb,
          srvInputTexture.Load(int3(x0 - 1u, y0, 0)).rgb,
          srvInputTexture.Load(int3(x0 + 1u, y0, 0)).rgb,
          srvInputTexture.Load(int3(x0, y0 + 1u, 0)).rgb,
          strength),
      1.0f);

  float3 c10 = srvInputTexture.Load(int3(x1, y0, 0)).rgb;
  uavOutputTexture[int2(x1, y0)] = float4(
      ApplyCas(
          c10,
          srvInputTexture.Load(int3(x1, y0 - 1u, 0)).rgb,
          srvInputTexture.Load(int3(x1 - 1u, y0, 0)).rgb,
          srvInputTexture.Load(int3(x1 + 1u, y0, 0)).rgb,
          srvInputTexture.Load(int3(x1, y0 + 1u, 0)).rgb,
          strength),
      1.0f);

  float3 c11 = srvInputTexture.Load(int3(x1, y1, 0)).rgb;
  uavOutputTexture[int2(x1, y1)] = float4(
      ApplyCas(
          c11,
          srvInputTexture.Load(int3(x1, y1 - 1u, 0)).rgb,
          srvInputTexture.Load(int3(x1 - 1u, y1, 0)).rgb,
          srvInputTexture.Load(int3(x1 + 1u, y1, 0)).rgb,
          srvInputTexture.Load(int3(x1, y1 + 1u, 0)).rgb,
          strength),
      1.0f);

  float3 c01 = srvInputTexture.Load(int3(x0, y1, 0)).rgb;
  uavOutputTexture[int2(x0, y1)] = float4(
      ApplyCas(
          c01,
          srvInputTexture.Load(int3(x0, y1 - 1u, 0)).rgb,
          srvInputTexture.Load(int3(x0 - 1u, y1, 0)).rgb,
          srvInputTexture.Load(int3(x0 + 1u, y1, 0)).rgb,
          srvInputTexture.Load(int3(x0, y1 + 1u, 0)).rgb,
          strength),
      1.0f);
}
