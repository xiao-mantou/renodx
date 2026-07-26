#include "./shared.h"

Texture3D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[2];
}

#define cmp -

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    out float4 o0 : SV_TARGET0) {
  // Keep the scene/input diagnostic colors independent from this later LUT.
  if ((RENODX_DEBUG_MODE > 0.5 && RENODX_DEBUG_MODE < 4.5) ||
      (RENODX_DEBUG_MODE > 9.5 && RENODX_DEBUG_MODE < 11.5)) {
    o0 = t0.SampleLevel(s0_s, v1.xy, 0);
    return;
  }

  // This writes immediately before the LUT pass returns. It determines
  // whether this late output target can preserve HDR independently from the
  // earlier scene bridge and its intermediate target.
  if (RENODX_DEBUG_MODE > 8.5 && RENODX_DEBUG_MODE < 9.5) {
    float3 output = 0.0;
    if (v1.x > 0.55 && v1.x < 0.95 && v1.y > 0.82 && v1.y < 0.92) {
      const uint index = min((uint)((v1.x - 0.55) * 10.0), 3u);
      const float levels[4] = {0.25, 1.0, 4.0, 16.0};
      output = levels[index].xxx;
    }
    o0 = float4(output, 1.0);
    return;
  }

  // Original DL2 LUT/color-grade shader, retained verbatim below.
  float4 r0, r1, r2, r3;
  r0.x = -1000 + cb0[1].y;
  r0.x = saturate(-0.00100000005 * r0.x);
  r0.y = r0.x * -2 + 3;
  r0.x = r0.x * r0.x;
  r0.x = r0.y * r0.x;
  r0.y = cmp(6500 >= cb0[1].y);
  r1.xyz = r0.yyy ? float3(0, 1669.58032, 2575.28271) : float3(-2666.34741, -2173.10132, 2575.28271);
  r1.xyz = cb0[1].yyy + r1.xyz;
  r2.xyz = r0.yyy ? float3(0, -2902.19556, -8257.7998) : float3(1745.04248, 1216.61682, -8257.7998);
  r0.yzw = r0.yyy ? float3(1, 1.33026743, 1.89937544) : float3(0.509953916, 0.703812003, 1.89937544);
  r1.xyz = r2.xyz / r1.xyz;
  r0.yzw = saturate(r1.xyz + r0.yzw);
  r1.xyz = float3(1, 1, 1) + -r0.yzw;
  r0.xyz = r0.xxx * r1.xyz + r0.yzw;
  r1.xyzw = t0.SampleLevel(s0_s, v1.xy, 0).xyzw;
  o0.w = r1.w;

  // The scene bridge (0x3E36DA5B) supplies HDR here. The original LUT has
  // an SDR domain and ends in a saturate, so grade its SDR reference only;
  // the HDR magnitude is restored after the original grading code below.
  const float3 input_hdr = max(renodx::draw::InvertIntermediatePass(r1.xyz), 0.0);
  const float3 input_sdr = saturate(input_hdr);
  if (RENODX_TONE_MAP_TYPE != 0.0) {
    r1.xyz = input_sdr;
  }

  r2.xyz = log2(abs(r1.xyz));
  r2.xyz = float3(0.416666657, 0.416666657, 0.416666657) * r2.xyz;
  r2.xyz = exp2(r2.xyz);
  r2.xyz = r2.xyz * float3(1.05499995, 1.05499995, 1.05499995) + float3(-0.0549999997, -0.0549999997, -0.0549999997);
  r3.xyz = float3(12.9200001, 12.9200001, 12.9200001) * r1.xyz;
  r1.xyz = cmp(float3(0.00313080009, 0.00313080009, 0.00313080009) >= r1.xyz);
  r1.xyz = r1.xyz ? r3.xyz : r2.xyz;
  r1.xyz = r1.xyz * float3(0.96875, 0.96875, 0.96875) + float3(0.015625, 0.015625, 0.015625);
  r1.xyz = t1.SampleLevel(s1_s, r1.xyz, 0).xyz;
  r2.xyz = r1.xyz * float3(0.947867274, 0.947867274, 0.947867274) + float3(0.0521326996, 0.0521326996, 0.0521326996);
  r2.xyz = log2(abs(r2.xyz));
  r2.xyz = float3(2.4000001, 2.4000001, 2.4000001) * r2.xyz;
  r2.xyz = exp2(r2.xyz);
  r3.xyz = float3(0.0773993805, 0.0773993805, 0.0773993805) * r1.xyz;
  r1.xyz = cmp(float3(0.0404499993, 0.0404499993, 0.0404499993) >= r1.xyz);
  r1.xyz = r1.xyz ? r3.xyz : r2.xyz;
  r0.xyz = r1.xyz * r0.xyz;
  r0.w = dot(r0.xyz, float3(0.212599993, 0.715200007, 0.0722000003));
  r0.w = max(9.99999975e-005, r0.w);
  r1.w = dot(r1.xyz, float3(0.212599993, 0.715200007, 0.0722000003));
  r0.w = r1.w / r0.w;
  r0.xyz = r0.xyz * r0.www;
  r0.w = cmp(cb0[1].y != 6500.000000);
  r0.xyz = r0.www ? r0.xyz : r1.xyz;
  r1.xyz = saturate(r0.xyz);
  r2.xyz = r1.xyz * float3(-2, -2, -2) + float3(3, 3, 3);
  r1.xyz = r1.xyz * r1.xyz;
  r1.xyz = r2.xyz * r1.xyz + -r0.xyz;
  r0.xyz = cb0[0].www * r1.xyz + r0.xyz;
  r0.w = saturate(dot(float3(0.212500006, 0.715399981, 0.0720999986), r0.xyz));
  r0.xyz = r0.xyz + -r0.www;
  r0.xyz = cb0[1].xxx * r0.xyz + r0.www;
  r1.xy = cmp(cb0[0].yx < abs(v1.wz));
  r0.w = (int)r1.y | (int)r1.x;
  r1.xyz = r0.www ? float3(0, 0, 0) : r0.xyz;
  r0.w = cmp(0 < cb0[0].z);
  o0.xyz = r0.www ? r1.xyz : r0.xyz;

  if (RENODX_TONE_MAP_TYPE != 0.0) {
    // The input was decoded above, so this re-encodes rather than applying a
    // second intermediate transform. Preserve the HDR magnitude while using
    // the original LUT/color-grade result for hue and saturation.
    o0.rgb = renodx::draw::RenderIntermediatePass(
        renodx::tonemap::UpgradeToneMap(input_hdr, input_sdr, o0.rgb, 1.0));
  }
  return;
}
