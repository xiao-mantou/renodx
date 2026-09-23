// Vanilla SDR baseline translated from the 0x06A2A81D SM3 instruction dump.
// No RenoDX tone mapping is applied in this validation pass.

float4 BloomTintAndScreenBlendThreshold : register(c0);
float4 MinZ_MaxZRatio : register(c2);
float4 ImageAdjustments1 : register(c7);
float4 ImageAdjustments2 : register(c8);
float4 HalfResMaskRect : register(c9);
float4 DNEColorStrokesLightSrcPos0 : register(c10);
float4 DNEColorStrokesLightSrcSize0 : register(c11);
float4 DNEColorStrokesLightSrcColor0 : register(c12);
float4 DNEColorStrokesLightSrcPos1 : register(c13);
float4 DNEColorStrokesLightSrcSize1 : register(c14);
float4 DNEColorStrokesLightSrcColor1 : register(c15);
float4 DNEColorStrokesBackBlendDistances : register(c16);
float4 DNEImageGrainParameter : register(c17);
float4 DNEVignetColor : register(c18);
float4 DNEVignetMaskFactors : register(c19);
float4 DepthTransition : register(c20);
float4 DepthDistances : register(c21);

sampler2D SceneColorTexture : register(s0);
sampler2D FilterColor1Texture : register(s1);
sampler2D DNEImageGrainTexture : register(s2);
sampler2D DNEVignetTexture : register(s3);
sampler2D ColorGradingLUT : register(s4);
sampler2D LowResPostProcessBuffer : register(s5);

struct PS_IN {
  float4 texcoord : TEXCOORD;
  float4 texcoord1 : TEXCOORD1;
  float4 texcoord2 : TEXCOORD2;
};

float4 SampleSceneLod(float2 uv, float lod) {
  return tex2Dlod(SceneColorTexture, float4(uv, 0.f, lod));
}

float4 main(PS_IN i) : COLOR {
  const float4 one_zero_zero_zero = float4(1.f, 0.f, 0.f, 0.f);
  const float4 one_zero_zero_one = float4(1.f, 0.f, 0.f, 1.f);

  float2 radial = i.texcoord2.zw - 0.5f;
  float2 radial_curve = radial * radial;
  radial_curve = radial_curve * -2.828f + 1.f;
  radial_curve *= radial_curve;
  radial_curve = radial_curve * -radial_curve + 1.f;

  float2 radial_exp = exp2(log2(radial_curve) * 1.5f);
  float4 radial_weight = float4(radial_exp.x, radial_exp.y, radial_exp.x, radial_exp.y);
  radial_weight *= -float4(radial.x, radial.y, radial.x, radial.y);

  float radial_mask = saturate((max(abs(radial.x), abs(radial.y)) - 0.36f) * 15.f);
  float4 tap_a = saturate(radial_weight.zwxy * float4(-0.007f, -0.007f, -0.0035f, -0.0035f) + i.texcoord1.xyxy);
  float4 tap_b = saturate(radial_weight * float4(0.0035f, 0.0035f, 0.007f, 0.007f) + i.texcoord1.xyxy);

  float4 sample_a = SampleSceneLod(tap_a.xy, tap_a.w);
  float4 sample_b = SampleSceneLod(tap_b.xy, tap_b.w);
  float4 sample_weight_a = radial_mask * float4(0.334f, 0.f, -0.333f, 0.f) + float4(0.666f, 0.666f, 0.f, 0.666f);
  float4 sample_weight_b = radial_mask * float4(0.334f, 0.334f, -0.666f, 0.334f) + 0.666f;

  float4 scene = sample_a.zzxy * sample_weight_a.zzyw;
  scene += sample_b.zzxy * sample_weight_b.zzww;
  scene += SampleSceneLod(radial_weight.xy, 0.f).zzxy * sample_weight_a;
  scene += SampleSceneLod(radial_weight.zw, 0.f).zzxy * sample_weight_b;
  float4 center = SampleSceneLod(i.texcoord1.xy, 0.f);
  scene += center.zzxy;

  float depth_delta = center.w - MinZ_MaxZRatio.y;
  float depth_rcp = (0.00001f - depth_delta >= 0.f) ? 100000.f : rcp(depth_delta);
  float depth = MinZ_MaxZRatio.x * depth_rcp;
  float stroke_blend = depth - DNEColorStrokesBackBlendDistances.x;
  stroke_blend = saturate(stroke_blend * rcp(DNEColorStrokesBackBlendDistances.y));
  stroke_blend = stroke_blend * stroke_blend - 1.f;

  float2 stroke_depth = saturate(float2(DNEColorStrokesLightSrcPos0.z, DNEColorStrokesLightSrcPos1.z));
  stroke_depth = stroke_depth * stroke_blend + 1.f;
  float4 stroke_delta = float4(i.texcoord1.xy - DNEColorStrokesLightSrcPos0.xy,
                               i.texcoord1.xy - DNEColorStrokesLightSrcPos1.xy);
  stroke_delta *= float4(rcp(DNEColorStrokesLightSrcSize0.x), rcp(DNEColorStrokesLightSrcSize0.y),
                         rcp(DNEColorStrokesLightSrcSize1.x), rcp(DNEColorStrokesLightSrcSize1.y)) * 2.f;
  float2 stroke_distance = float2(dot(stroke_delta.xy, stroke_delta.xy), dot(stroke_delta.zw, stroke_delta.zw));
  float2 stroke_mask = 1.f - saturate(stroke_distance * 4.f);
  stroke_mask *= stroke_mask;
  float2 stroke_weight = stroke_mask * stroke_depth;
  float4 stroke_color = DNEColorStrokesLightSrcColor0 * stroke_weight.x
                      + DNEColorStrokesLightSrcColor1 * stroke_weight.y;

  scene = scene * 0.33333f + stroke_color.w;
  scene = scene * (1.f + stroke_color.z);

  float2 low_res_uv = min(HalfResMaskRect.zw, max(i.texcoord1.zw, HalfResMaskRect.xy));
  float4 low_res = tex2D(LowResPostProcessBuffer, low_res_uv);
  float4 low_res_scaled = low_res.zzxy * 4.f;
  scene = scene * stroke_color.x - low_res_scaled.yzwx;
  scene = low_res.w * scene + low_res_scaled;

  float3 hdr_color = scene.zwy;
  float bloom_weight = saturate(exp2(-3.f * dot(hdr_color, float3(0.3f, 0.59f, 0.11f))) * BloomTintAndScreenBlendThreshold.w);
  float4 filter = tex2D(FilterColor1Texture, i.texcoord.zw);
  float3 bloom = filter.zzx * BloomTintAndScreenBlendThreshold.zzx * 4.f;
  hdr_color += bloom * bloom_weight;

  float3 lut_scaled = hdr_color * ImageAdjustments2.y + ImageAdjustments2.xxx;
  float3 lut_input_rgb = saturate(hdr_color * rcp(max(lut_scaled, 0.00001f)));
  float4 lut_input = float4(lut_input_rgb.b, lut_input_rgb.r, lut_input_rgb.g, lut_input_rgb.b);
  float3 lut_coord = lut_input.rgb * 15.f;
  float lut_slice = floor(lut_coord.x);
  float lut_fraction = lut_coord.x - lut_slice;
  float2 lut_uv0 = float2(lut_slice * 0.0625f + 0.05859375f * lut_input.a, lut_coord.y * 0.0625f + 0.0078125f);
  float2 lut_uv1 = float2((lut_slice + 1.f) * 0.0625f + 0.05859375f * lut_input.a, lut_coord.y * 0.0625f + 0.0078125f);
  float3 vanilla_sdr = lerp(tex2D(ColorGradingLUT, lut_uv0).rgb, tex2D(ColorGradingLUT, lut_uv1).rgb, lut_fraction);

  float vignette = saturate(dot(tex2D(DNEVignetTexture, i.texcoord2.zw), DNEVignetMaskFactors));
  float3 vignette_color = 1.f + vignette * (DNEVignetColor.rgb - 1.f);
  float grain = tex2D(DNEImageGrainTexture, i.texcoord2.zw * 4.f + DNEImageGrainParameter.xy).r * 2.f - 1.f;
  float3 output_color = saturate(vanilla_sdr * vignette_color + grain * ImageAdjustments1.w);

  return float4(output_color, 1.f);
}
