// ---- Created with 3Dmigoto v1.3.16 on Wed Jul 22 13:25:57 2026
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
  float2 v2 : TEXCOORD1,
  out float4 o0 : SV_TARGET0)
{
  float4 r0;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = t0.Sample(s0_s, v2.xy).xw;
  r0.x = dot(r0.yx, cb0[0].zw);
  o0.w = v1.w * r0.x;
  o0.xyz = v1.xyz;
  return;
}