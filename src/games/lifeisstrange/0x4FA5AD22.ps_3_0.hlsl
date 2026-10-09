#include "./shared.h"

float4 BloomTintAndScreenBlendThreshold : register(c0);
float4 MinZ_MaxZRatio : register(c2);
float4 ImageAdjustments1 : register(c7);
float4 ImageAdjustments2 : register(c8);
float4 HalfResMaskRect : register(c9);
float4 DNEImageGrainParameter : register(c10);
float4 DNEVignetColor : register(c11);
float4 DNEVignetMaskFactors : register(c12);
float4 DepthTransition : register(c13);
float4 DepthDistances : register(c14);

sampler2D SceneColorTexture : register(s0);
sampler2D FilterColor1Texture : register(s1);
sampler2D DNEImageGrainTexture : register(s2);
sampler2D DNEVignetTexture : register(s3);
sampler2D ColorGradingLUT : register(s4);
sampler2D LowResPostProcessBuffer : register(s5);

static const float4 c1 = float4(1.f, 0.f, 0.00001f, 100000.f);
static const float4 c3 = float4(-0.5f, 2.82800007f, 1.f, 1.5f);
static const float4 c4 = float4(0.001953125f, 0.064453125f, 2.f, -1.f);
static const float4 c5 = float4(0.0070000002f, 0.0035000001f, -0.36000001f, 15.f);
static const float4 c6 = float4(0.333999991f, -0.666000009f, 0.666000009f, 4.f);
static const float4 c15 = float4(0.333999991f, 0.f, -0.333000004f, 0.666000009f);
static const float4 c16 = float4(0.333330005f, 0.300000012f, 0.589999974f, 0.109999999f);
static const float4 c17 = float4(-3.f, 65504.f, -1.f, -2.f);
static const float4 c18 = float4(0.25f, 0.0078125f, 0.0625f, 0.f);
static const float4 c19 = float4(14.9998999f, 0.05859375f, 0.234375f, 0.f);

struct PS_IN {
  float4 texcoord : TEXCOORD;
  float4 texcoord1 : TEXCOORD1;
  float4 texcoord2 : TEXCOORD2;
};

half4 main(PS_IN i) : COLOR {
  half4 o;

  float4 r0;
  float4 r1;
  float4 r2;
  float4 r3;
  float4 r4;
  float4 r5;
  float4 r6;

  r0.xy = half2(c3.xx + i.texcoord2.zw);
  r0.zw = half2(r0.xy * r0.xy);
  r0.zw = half2(r0.zw * -c3.yy + c3.zz);
  r0.zw = half2(r0.zw * r0.zw);
  r0.zw = half2(r0.zw * -r0.zw + c1.xx);
  r1.x = log2(r0.z);
  r1.y = log2(r0.w);
  r0.zw = r1.xy * c3.ww;
  r1.xz = half2(exp2(r0.zz));
  r1.yw = half2(exp2(r0.ww));
  r1 = half4(-r0.xyxy * r1);

  r2.x = max(abs(r0.x), abs(r0.y));
  r0.x = r2.x + c5.z;
  r0.x = saturate(r0.x * c5.w);
  r2 = saturate(r1.zwxy * -c5.xxyy + i.texcoord1.xyxy);
  r1 = saturate(r1 * c5.yyxx + i.texcoord1.xyxy);

  r3 = r2.zwxx * c1.xxyy;
  r2 = r2.xyxx * c1.xxyy;
  r2 = half4(tex2Dlod(SceneColorTexture, r2));
  r3 = half4(tex2Dlod(SceneColorTexture, r3));
  r4 = r0.x * c6.xxyx + c6.z;
  r5 = c15;
  r0 = r0.x * r5.xxyz + abs(r5.wwyz);
  r3 = r3.zzxy * r4.zzww;
  r2 = r2.zzxy * r0.zzyw + r3;

  r3 = r1.xyxx * c1.xxyy;
  r1 = r1.zwxx * c1.xxyy;
  r1 = half4(tex2Dlod(SceneColorTexture, r1));
  r3 = half4(tex2Dlod(SceneColorTexture, r3));
  r2 = r3.zzxy * r4 + r2;
  r0 = r1.zzxy * r0 + r2;
  r1 = c1.xxyy * i.texcoord1.xyxx;
  r1 = half4(tex2Dlod(SceneColorTexture, r1));
  r0 = half4(r0 + r1.zzxy);

  r1.x = r1.w - MinZ_MaxZRatio.y;
  r1.yz = max(i.texcoord1.zw, HalfResMaskRect.xy);
  r2.xy = min(HalfResMaskRect.zw, r1.yz);
  r2 = half4(tex2D(LowResPostProcessBuffer, r2.xy));
  r3 = r2.zzxy * c6.wwww;
  r0 = r0 * c16.x + -r3.yyzw;
  r0 = half4(r2.w * r0 + r3);
  r1.y = half(dot(r0.zwy, c16.yzw));
  r1.y = half(r1.y * c17.x);
  r1.y = half(exp2(r1.y));
  r1.y = half(saturate(r1.y * BloomTintAndScreenBlendThreshold.w));
  r2 = tex2D(FilterColor1Texture, i.texcoord.zw);
  r2 = r2.zzxy * BloomTintAndScreenBlendThreshold.zzxy;
  r2 = half4(r2 * c6.wwww);
  r0 = half4(r2 * r1.y + r0);

  r1.yzw = half3(r0.zwy * ImageAdjustments2.yyy + ImageAdjustments2.xxx);
  r2.z = 1.f / r1.y;
  r2.w = 1.f / r1.z;
  r2.xy = 1.f / r1.ww;
  r0 = half4(saturate(r0 * r2));

  r2.xyw = r0.xwz * c19.xzy;
  r0.x = frac(r2.x);
  r0.x = -r0.x + r2.x;
  r0.y = r0.y * c5.w - r0.x;
  r2.x = r0.x * c18.z + r2.w;
  r0.x = -r1.x + c1.z;
  r0.z = 1.f / r1.x;
  r0.x = r0.x >= 0.f ? c1.w : r0.z;
  r0.x = r0.x * MinZ_MaxZRatio.x;
  r1.x = min(r0.x, c17.y);

  r3 = r1.x - DepthDistances;
  r3 = half4((r3 >= 0.f) ? c1.y : c1.x);
  r4.xy = c1.xy;
  r5 = DepthDistances.xxyz * -r4.yxxx + r1.x;
  r0.xzw = half3(saturate(r1.xxx * DepthTransition.www - DepthTransition.xyz));
  r1 = half4((r5 >= 0.f) ? r3 : 0.f);
  // HlslDecompiler scalarized this dp3. The assembly vector -c17.xzw is (3, 1, 2).
  r1.w = half(dot(r1.wyz, float3(3.f, 1.f, 2.f)));
  r0.x = half(dot(r1.xyz, r0.xzw));
  r0.z = half(r1.w + c1.x);

  r1.x = r1.w * c18.x + c18.y;
  r3.x = r0.z * c18.x + c18.y;
  r3.yz = c4.xy;
  r3 = r2.xyxy + r3.yxzx;
  r5 = half4(tex2D(ColorGradingLUT, r3.xy));
  r3 = half4(tex2D(ColorGradingLUT, r3.zw));
  r6 = half4(lerp(r5, r3, r0.y));

  r1.yz = c4.xy;
  r1 = r2.xyxy + r1.yxzx;
  r2 = half4(tex2D(ColorGradingLUT, r1.xy));
  r1 = half4(tex2D(ColorGradingLUT, r1.zw));
  r3 = half4(lerp(r2, r1, r0.y));
  r1 = half4(lerp(r3, r6, r0.x));

  r0 = tex2D(DNEVignetTexture, i.texcoord2.zw);
  r0.x = saturate(dot(r0, DNEVignetMaskFactors));
  r0.yzw = -r4.xxx + DNEVignetColor.xyz;
  r0.xyz = r0.xxx * r0.yzw + c1.xxx;
  r0.w = c6.w;
  r2.xy = half2(i.texcoord2.zw * r0.ww + DNEImageGrainParameter.xy);
  r2 = half4(tex2D(DNEImageGrainTexture, r2.xy));
  r0.w = r2.x * c4.z + c4.w;
  r0.w = r0.w * ImageAdjustments1.w;

  // Preserve the original final mad_sat and LUT alpha output.
  o.xyz = half3(saturate(r1.xyz * r0.xyz + r0.www));
  o.w = half(r1.w);
  return o;
}
