/*
 * Copyright (C) 2026 xiao-mantou
 * SPDX-License-Identifier: MIT
 */

// Modern HDR-normalized RCAS (Robust Contrast Adaptive Sharpening) for F1 22 (0x2EA7EE8A).
// Replaces the legacy SDR CAS pass which clamped outputs > 1.0 with saturate().
// Operates on BT.709 relative luminance with local dynamic range normalization,
// 0.99 overshoot limiter, noise suppression, and scale-invariant proportional RGB resolve.

Texture2D<float4> srvInputTexture : register(t0);
RWTexture2D<float4> uavOutputTexture : register(u0);

cbuffer cb : register(b0) {
  uint4 const0 : packoffset(c000.x);
  uint4 const1 : packoffset(c001.x);
  uint4 const2 : packoffset(c002.x);
};

float3 ApplyRCASHdr(float3 c, float3 b, float3 d, float3 f, float3 h, float sharpness) {
  // b = up (0, -1)
  // d = left (-1, 0)
  // c = center (0, 0)
  // f = right (1, 0)
  // h = down (0, 1)

  float b_lum = dot(b, float3(0.212639f, 0.715169f, 0.072192f));
  float d_lum = dot(d, float3(0.212639f, 0.715169f, 0.072192f));
  float c_lum = dot(c, float3(0.212639f, 0.715169f, 0.072192f));
  float f_lum = dot(f, float3(0.212639f, 0.715169f, 0.072192f));
  float h_lum = dot(h, float3(0.212639f, 0.715169f, 0.072192f));

  // Local normalization factor: guarantees relative contrast evaluation in [0, 1]
  // without clipping or distortion in HDR highlights.
  float norm = max(max(max(b_lum, d_lum), max(f_lum, h_lum)), max(c_lum, 1e-4f));
  float rcp_norm = rcp(norm);

  float b_n = b_lum * rcp_norm;
  float d_n = d_lum * rcp_norm;
  float c_n = c_lum * rcp_norm;
  float f_n = f_lum * rcp_norm;
  float h_n = h_lum * rcp_norm;

  float min4_lum = min(min(b_n, d_n), min(f_n, h_n));
  float max4_lum = max(max(b_n, d_n), max(f_n, h_n));

  // 0.99 limiter from AMD/Lilium prevents extreme overshoot ringing
  float limited_max4_lum = min(max4_lum, 0.99f);

  float hit_min_lum = min4_lum * rcp(4.0f * limited_max4_lum);
  float hit_max_lum = (1.0f - limited_max4_lum) * rcp(4.0f * min4_lum - 4.0f);
  float local_lobe = max(-hit_min_lum, hit_max_lum);

  // AMD RCAS limit: 0.1875f = 0.25f - 1/16
  static const float FSR_RCAS_LIMIT = 0.1875f;
  float lobe = max(-FSR_RCAS_LIMIT, min(local_lobe, 0.0f)) * sharpness;

  // Noise detection and suppression
  float b_2x = b_n * 2.0f;
  float d_2x = d_n * 2.0f;
  float c_2x = c_n * 2.0f;
  float f_2x = f_n * 2.0f;
  float h_2x = h_n * 2.0f;

  float nz = 0.25f * (b_2x + d_2x + f_2x + h_2x) - c_2x;
  float max_2x = max(max(b_2x, d_2x), max(max(f_2x, h_2x), c_2x));
  float min_2x = min(min(b_2x, d_2x), min(min(f_2x, h_2x), c_2x));
  float range_2x = max_2x - min_2x;

  if (range_2x > 1e-5f) {
    float nz_clamped = saturate(abs(nz) * rcp(range_2x));
    nz = -0.5f * nz_clamped + 1.0f;
    lobe *= nz;
  }

  // Resolve sharpened luminance
  float rcp_l = rcp(4.0f * lobe + 1.0f);
  float pix_lum = ((b_lum + d_lum + f_lum + h_lum) * lobe + c_lum) * rcp_l;

  // Proportional RGB scaling preserves exact chromaticity, hue, and HDR intensity
  float scale = (c_lum > 1e-5f) ? clamp(pix_lum / c_lum, 0.0f, 4.0f) : 1.0f;
  return max(0.0f, c * scale);
}

float3 LoadSample(int2 coord, int2 max_coord) {
  return srvInputTexture.Load(int3(clamp(coord, int2(0, 0), max_coord), 0)).rgb;
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

  float raw_strength = asfloat(const1.x);
  float sharpness = 0.0f;
  if (abs(raw_strength) >= 0.125f && abs(raw_strength) <= 0.25f) {
    sharpness = saturate((8.0f - 1.0f / abs(raw_strength)) / 3.0f);
  } else {
    sharpness = saturate(abs(raw_strength));
  }

  uint width, height;
  srvInputTexture.GetDimensions(width, height);
  int2 max_coord = int2(int(width) - 1, int(height) - 1);

  int2 coords[4] = {
    int2(x0, y0),
    int2(x1, y0),
    int2(x1, y1),
    int2(x0, y1)
  };

  [unroll]
  for (int i = 0; i < 4; ++i) {
    int2 coord = coords[i];
    float3 c = LoadSample(coord, max_coord);
    if (sharpness <= 0.0f) {
      uavOutputTexture[coord] = float4(max(0.0f, c), 1.0f);
    } else {
      float3 b = LoadSample(coord + int2(0, -1), max_coord);
      float3 d = LoadSample(coord + int2(-1, 0), max_coord);
      float3 f = LoadSample(coord + int2(1, 0), max_coord);
      float3 h = LoadSample(coord + int2(0, 1), max_coord);
      uavOutputTexture[coord] = float4(ApplyRCASHdr(c, b, d, f, h, sharpness), 1.0f);
    }
  }
}
