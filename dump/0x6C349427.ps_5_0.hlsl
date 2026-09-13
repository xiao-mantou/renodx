// ---- Created with 3Dmigoto v1.3.16 on Fri Jul 24 00:37:31 2026
Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s1_s : register(s1);

SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0)
{
  float4 cb0[2];
}




// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_POSITION0,
  float4 v1 : TEXCOORD0,
  float4 v2 : TEXCOORD1,
  out float4 o0 : SV_TARGET0)
{
  float4 r0,r1;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = cb0[1].xx * v2.zw;
  r0.zw = -v2.zw * cb0[1].xx + float2(1,1);
  r0.xyzw = cmp(r0.xyzw < float4(0,0,0,0));
  r0.xy = (int2)r0.zw | (int2)r0.xy;
  r0.x = (int)r0.y | (int)r0.x;
  if (r0.x != 0) discard;
  r0.xyz = float3(1,1,1);
  r1.xyzw = t0.Sample(s0_s, v2.xy).xyzw;
  r0.w = r1.x;
  r1.xyzw = cb0[0].zzzz * r1.xyzw;
  r0.xyzw = r0.xyzw * cb0[0].wwww + r1.xyzw;
  r0.xyzw = v1.xyzw * r0.xyzw;
  r1.x = t1.Sample(s1_s, v2.zw).x;
  o0.w = r1.x * r0.w;
  o0.xyz = r0.xyz;
  return;
}