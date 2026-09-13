// ---- Created with 3Dmigoto v1.3.16 on Wed Jul 22 13:26:05 2026
Texture2D<float4> t2 : register(t2);

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
  float2 v3 : TEXCOORD3,
  out float4 o0 : SV_TARGET0)
{
  float4 r0,r1;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = cb0[0].xx * v2.zw;
  r0.zw = -v2.zw * cb0[0].xx + float2(1,1);
  r0.xy = cmp(r0.xy < float2(0,0));
  r0.x = (int)r0.y | (int)r0.x;
  r0.yz = cmp(r0.zw < float2(0,0));
  r0.x = (int)r0.y | (int)r0.x;
  r0.x = (int)r0.z | (int)r0.x;
  r0.yz = cb0[0].yy * v3.xy;
  r1.xy = -v3.xy * cb0[0].yy + float2(1,1);
  r0.yz = cmp(r0.yz < float2(0,0));
  r0.y = (int)r0.z | (int)r0.y;
  r0.zw = cmp(r1.xy < float2(0,0));
  r0.y = (int)r0.z | (int)r0.y;
  r0.y = (int)r0.w | (int)r0.y;
  r0.z = r0.y ? r0.x : 0;
  if (r0.z != 0) discard;
  r1.xyzw = t0.Sample(s0_s, v2.xy).xyzw;
  r1.xyzw = v1.xyzw * r1.xyzw;
  if (r0.x == 0) {
    r0.x = t1.Sample(s0_s, v2.zw).x;
  } else {
    r0.x = 0;
  }
  if (r0.y == 0) {
    r0.y = t2.Sample(s0_s, v3.xy).x;
    r0.x = r0.x + r0.y;
  }
  r0.x = saturate(r0.x);
  o0.w = r1.w * r0.x;
  o0.xyz = r1.xyz;
  return;
}