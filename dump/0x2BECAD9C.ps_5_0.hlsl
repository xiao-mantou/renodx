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
  float4 v2 : TEXCOORD1,
  out float4 o0 : SV_TARGET0)
{
  float4 r0,r1;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = float2(-0.5,-0.5) + v2.xy;
  r0.z = max(abs(r0.x), abs(r0.y));
  r0.z = 1 / r0.z;
  r0.w = min(abs(r0.x), abs(r0.y));
  r0.z = r0.w * r0.z;
  r0.w = r0.z * r0.z;
  r1.x = r0.w * 0.0208350997 + -0.0851330012;
  r1.x = r0.w * r1.x + 0.180141002;
  r1.x = r0.w * r1.x + -0.330299497;
  r0.w = r0.w * r1.x + 0.999866009;
  r1.x = r0.z * r0.w;
  r1.x = r1.x * -2 + 1.57079637;
  r1.y = cmp(abs(r0.y) < abs(r0.x));
  r1.x = r1.y ? r1.x : 0;
  r0.z = r0.z * r0.w + r1.x;
  r0.w = cmp(r0.y < -r0.y);
  r0.w = r0.w ? -3.141593 : 0;
  r0.z = r0.z + r0.w;
  r0.w = min(r0.x, r0.y);
  r0.w = cmp(r0.w < -r0.w);
  r1.x = max(r0.x, r0.y);
  r0.x = dot(r0.xy, r0.xy);
  r0.x = cmp(0 < r0.x);
  r0.y = cmp(r1.x >= -r1.x);
  r0.y = r0.y ? r0.w : 0;
  r0.y = r0.y ? -r0.z : r0.z;
  r0.y = -r0.y * 0.159154952 + 0.5;
  r0.y = -cb0[0].z + r0.y;
  r0.y = 1 + r0.y;
  r0.z = cmp(r0.y >= -r0.y);
  r0.y = frac(abs(r0.y));
  r0.y = r0.z ? r0.y : -r0.y;
  r0.yz = -cb0[0].xy + r0.yy;
  r0.yz = float2(10000,10000) * r0.yz;
  r0.yz = max(float2(-1,-1), r0.yz);
  r0.yz = min(float2(1,1), r0.yz);
  r0.y = 1 + -r0.y;
  r0.y = r0.y * r0.z;
  r0.x = r0.x ? r0.y : 0;
  r0.x = saturate(r0.x);
  r1.xyzw = t0.Sample(s0_s, v2.zw).xyzw;
  r1.w = r1.w * r0.x;
  o0.xyzw = v1.xyzw * r1.xyzw;
  return;
}