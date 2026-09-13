// ---- Created with 3Dmigoto v1.3.16 on Wed Jul 22 13:26:03 2026
Texture2D<float4> t0 : register(t0);

SamplerState s0_s : register(s0);




// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_POSITION0,
  linear noperspective float2 v1 : TEXCOORD0,
  out float4 o0 : SV_TARGET0)
{
  o0.xyzw = t0.SampleLevel(s0_s, v1.xy, 0).xyzw;
  return;
}