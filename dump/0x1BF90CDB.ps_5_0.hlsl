// ---- Created with 3Dmigoto v1.3.16 on Fri Jul 24 00:37:22 2026
Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0)
{
  float4 cb0[1];
}




// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_POSITION0,
  float4 v1 : TEXCOORD0,
  float4 v2 : TEXCOORD1,
  linear noperspective float2 v3 : TEXCOORD2,
  out float4 o0 : SV_TARGET0)
{
  float4 r0;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = t0.Sample(s0_s, v2.xy).w;
  r0.w = saturate(r0.x * cb0[0].x + cb0[0].y);
  r0.xyz = t1.SampleLevel(s0_s, v3.xy, 0).xyz;
  o0.xyzw = v1.xyzw * r0.xyzw;
  return;
}