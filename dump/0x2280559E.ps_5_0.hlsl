// ---- Created with 3Dmigoto v1.3.16 on Wed Jul 22 13:25:56 2026
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
  float4 r0,r1;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xyz = float3(1,1,1);
  r1.xyzw = t0.Sample(s0_s, v2.xy).xyzw;
  r0.w = r1.x;
  r1.xyzw = cb0[0].zzzz * r1.xyzw;
  r0.xyzw = r0.xyzw * cb0[0].wwww + r1.xyzw;
  o0.xyzw = v1.xyzw * r0.xyzw;
  return;
}