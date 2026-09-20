#include "./shared.h"

// ---- Created with 3Dmigoto v1.3.16 on Wed Jul 22 13:25:57 2026
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0)
{
  float4 cb0[2];
}




// 3Dmigoto declarations
#define cmp -

// Vanilla display transform: ((A*x + B) * x) / ((C*x + D) * x + E)
// A Narkowicz-style ACES fit. It asymptotes to A/C (~1.033), so every pass
// downstream of it is display referred and has no headroom left to expand.
float3 VanillaToneMap(float3 x) {
  float3 numerator = (cb0[0].xxx * x + cb0[0].yyy) * x;
  float3 denominator = x * (cb0[0].zzz * x + cb0[0].www) + cb0[1].xxx;
  return numerator / denominator;
}

float3 VanillaToneMapExtended(float3 x) {
  const float pivot = 0.18;

  // curve, evaluated only up to the pivot
  float3 c = min(x, pivot);
  float3 numerator = (cb0[0].xxx * c + cb0[0].yyy) * c;
  float3 denominator = c * (cb0[0].zzz * c + cb0[0].www) + cb0[1].xxx;
  float3 curve = numerator / denominator;

  // tangent slope at the pivot, analytic (scalar — same for all channels)
  float n = (cb0[0].x * pivot + cb0[0].y) * pivot;
  float d = pivot * (cb0[0].z * pivot + cb0[0].w) + cb0[1].x;
  float dn = 2.0 * cb0[0].x * pivot + cb0[0].y;
  float dd = 2.0 * cb0[0].z * pivot + cb0[0].w;
  float slope = (dn * d - n * dd) / (d * d);

  return curve + slope * max(x - pivot, 0.0);
}

void main(
  float4 v0 : SV_POSITION0,
  linear noperspective float2 v1 : TEXCOORD0,
  out float4 o0 : SV_TARGET0)
{
  float exposure = t1.SampleLevel(s0_s, float2(0, 0), 0).x;
  float4 source = t0.SampleLevel(s0_s, v1.xy, 0);

  float3 scene = source.xyz * exposure;
  float3 exposed = 0.6f * scene;
  o0.w = source.w;

  if (RENODX_TONE_MAP_TYPE == 0.f) {
    o0.xyz = VanillaToneMap(exposed);
  } else {
    o0.xyz = VanillaToneMapExtended(exposed);
  }
  return;
}
