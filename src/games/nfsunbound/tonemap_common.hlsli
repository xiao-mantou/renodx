#include "./shared.h"

Texture2D<float4> t0 : register(t0);

Texture2D<float4> t1 : register(t1);

Texture3D<float4> t9 : register(t9);

Texture2D<float4> t10 : register(t10);

cbuffer cb0 : register(b0) {
  float2 Constants_000 : packoffset(c000.x);
  float2 Constants_008 : packoffset(c000.z);
  float2 Constants_016 : packoffset(c001.x);
  float2 Constants_024 : packoffset(c001.z);
  float2 Constants_032 : packoffset(c002.x);
  float2 Constants_040 : packoffset(c002.z);
  float4 Constants_048 : packoffset(c003.x);
  int Constants_064 : packoffset(c004.x);
  int Constants_068 : packoffset(c004.y);
  float2 Constants_072 : packoffset(c004.z);
  float2 Constants_080 : packoffset(c005.x);
  float2 Constants_088 : packoffset(c005.z);
  struct ColorBlindParameters {
    float ColorBlindParameters_000;
    float ColorBlindParameters_004;
    float ColorBlindParameters_008;
    float ColorBlindParameters_012;
    float ColorBlindParameters_016;
    float ColorBlindParameters_020;
    float ColorBlindParameters_024;
    float ColorBlindParameters_028;
    float ColorBlindParameters_032;
    float ColorBlindParameters_036;
    float ColorBlindParameters_040;
    float ColorBlindParameters_044;
  } Constants_096 : packoffset(c006.x);
};

SamplerState s0 : register(s0);

SamplerState s1 : register(s1);

SamplerState s2 : register(s2);

struct OutputSignature {
  float4 SV_Target : SV_Target;
  float4 SV_Target_1 : SV_Target1;
};

OutputSignature main(
  noperspective float4 SV_Position : SV_Position,
  linear float2 TEXCOORD : TEXCOORD
) {
  float4 SV_Target;
  float4 SV_Target_1;
  float4 _11 = t0.SampleLevel(s0, float2(TEXCOORD.x, TEXCOORD.y), 0.0f);
  float _20 = max((Constants_048.x * _11.x), 0.0f);
  float _21 = max((Constants_048.x * _11.y), 0.0f);
  float _22 = max((Constants_048.x * _11.z), 0.0f);
  float _48;
  float _49;
  float _50;
  float _235;
  float _236;
  float _237;
  float _335;
  float _336;
  float _337;
  float _364;
  float _365;
  float _366;
  float _589;
  float _590;
  float _591;
  float _690;
  float _691;
  float _692;
  float _719;
  float _720;
  float _721;
  [branch]
  if (!((Constants_068 & 8) == 0)) {
    float4 _28 = t1.SampleLevel(s2, float2(TEXCOORD.x, TEXCOORD.y), 0.0f);
    float _40 = 1.0f - (Constants_048.z * _28.w);
    _48 = ((_40 * _20) + (Constants_048.y * _28.x));
    _49 = ((_40 * _21) + (Constants_048.y * _28.y));
    _50 = ((_40 * _22) + (Constants_048.y * _28.z));
  } else {
    _48 = _20;
    _49 = _21;
    _50 = _22;
  }
  int _56 = int(Constants_032.x * TEXCOORD.x);
  int _57 = int(Constants_032.y * TEXCOORD.y);
  [branch]
  if (!((Constants_068 & 1) == 0)) {
    float _98 = (Constants_096.ColorBlindParameters_024 * Constants_096.ColorBlindParameters_012) + 1.0f;
    float _106 = Constants_096.ColorBlindParameters_012 * Constants_096.ColorBlindParameters_028;
    float _107 = ((_98 * (select((_48 <= 0.0031308000907301903f), (_48 * 12.920000076293945f), ((exp2(log2(abs(_48)) * 0.4166666567325592f) * 1.0549999475479126f) + -0.054999999701976776f)) + -0.5f)) + 0.5f) - _106;
    float _108 = ((_98 * (select((_49 <= 0.0031308000907301903f), (_49 * 12.920000076293945f), ((exp2(log2(abs(_49)) * 0.4166666567325592f) * 1.0549999475479126f) + -0.054999999701976776f)) + -0.5f)) + 0.5f) - _106;
    float _109 = ((_98 * (select((_50 <= 0.0031308000907301903f), (_50 * 12.920000076293945f), ((exp2(log2(abs(_50)) * 0.4166666567325592f) * 1.0549999475479126f) + -0.054999999701976776f)) + -0.5f)) + 0.5f) - _106;
    float _114 = ((_107 * 17.882400512695312f) + (_108 * 43.5161018371582f)) + (_109 * 4.119349956512451f);
    float _119 = ((_107 * 3.4556500911712646f) + (_108 * 27.155399322509766f)) + (_109 * 3.867140054702759f);
    float _124 = ((_107 * 0.029956599697470665f) + (_108 * 0.1843090057373047f)) + (_109 * 1.4670900106430054f);
    float _132 = (((_119 * 2.023439884185791f) - (_124 * 2.52810001373291f)) * Constants_096.ColorBlindParameters_000) + (_114 * (1.0f - Constants_096.ColorBlindParameters_000));
    float _140 = (((_114 * 0.4942069947719574f) + (_124 * 1.248270034790039f)) * Constants_096.ColorBlindParameters_004) + (_119 * (1.0f - Constants_096.ColorBlindParameters_004));
    float _148 = (((_119 * 0.8011090159416199f) - (_114 * 0.3959130048751831f)) * Constants_096.ColorBlindParameters_008) + ((1.0f - Constants_096.ColorBlindParameters_008) * _124);
    float _161 = ((((_140 * 0.13050441443920135f) + _107) - (_132 * 0.08094444870948792f)) - (_148 * 0.11672106385231018f)) * 0.699999988079071f;
    float _181 = 1.0f - Constants_096.ColorBlindParameters_012;
    float _192 = Constants_096.ColorBlindParameters_020 + 1.0f;
    float _203 = (Constants_096.ColorBlindParameters_012 * Constants_096.ColorBlindParameters_032) + Constants_096.ColorBlindParameters_016;
    float _207 = max(((((((Constants_096.ColorBlindParameters_012 * min(max(_107, 0.0f), 1.0f)) + -0.5f) + (_181 * _107)) * _192) + 0.5f) + _203), 0.0f);
    float _208 = max(((((((Constants_096.ColorBlindParameters_012 * min(max((((((_140 * -0.05401932820677757f) + (_132 * 0.010248533450067043f)) + (_148 * 0.11361470818519592f)) + _161) + (_108 * 2.0f)), 0.0f), 1.0f)) + -0.5f) + (_181 * _108)) * _192) + 0.5f) + _203), 0.0f);
    float _209 = max(((((((Constants_096.ColorBlindParameters_012 * min(max((((((_140 * 0.004121614620089531f) + (_132 * 0.0003652969317045063f)) - (_148 * 0.693511426448822f)) + _161) + (_109 * 2.0f)), 0.0f), 1.0f)) + -0.5f) + (_181 * _109)) * _192) + 0.5f) + _203), 0.0f);
    _235 = select((_207 <= 0.040449999272823334f), (_207 * 0.07739938050508499f), exp2(log2((_207 + 0.054999999701976776f) * 0.9478673338890076f) * 2.4000000953674316f));
    _236 = select((_208 <= 0.040449999272823334f), (_208 * 0.07739938050508499f), exp2(log2((_208 + 0.054999999701976776f) * 0.9478673338890076f) * 2.4000000953674316f));
    _237 = select((_209 <= 0.040449999272823334f), (_209 * 0.07739938050508499f), exp2(log2((_209 + 0.054999999701976776f) * 0.9478673338890076f) * 2.4000000953674316f));
  } else {
    _235 = _48;
    _236 = _49;
    _237 = _50;
  }
  [branch]
  if (!((Constants_068 & 16) == 0)) {
    float _265 = select((_235 <= 0.0031308000907301903f), (_235 * 12.920000076293945f), ((exp2(log2(abs(_235)) * 0.4166666567325592f) * 1.0549999475479126f) + -0.054999999701976776f));
    float _266 = select((_236 <= 0.0031308000907301903f), (_236 * 12.920000076293945f), ((exp2(log2(abs(_236)) * 0.4166666567325592f) * 1.0549999475479126f) + -0.054999999701976776f));
    float _267 = select((_237 <= 0.0031308000907301903f), (_237 * 12.920000076293945f), ((exp2(log2(abs(_237)) * 0.4166666567325592f) * 1.0549999475479126f) + -0.054999999701976776f));
    float4 _274 = t10.SampleLevel(s1, float2((Constants_040.x * float((int)(_56))), (Constants_040.y * float((int)(_57)))), 0.0f);
    float _278 = dot(float3(_265, _266, _267), float3(0.29899999499320984f, 0.5870000123977661f, 0.11400000005960464f));
    float _279 = dot(float3(_265, _266, _267), float3(-0.16869999468326569f, -0.3312999904155731f, 0.5f));
    float _280 = dot(float3(_265, _266, _267), float3(0.5f, -0.4187000095844269f, -0.08129999786615372f));
    float _283 = saturate((_274.x + _274.y) + _274.z);
    float _288 = _283 * 0.0555555559694767f;
    float _290 = (_288 * ((_274.y * 0.5f) - frac(_279 * 18.0f))) + _279;
    float _296 = (_288 * ((_274.z * 0.5f) - frac(_280 * 18.0f))) + _280;
    float _297 = 1.0f - _283;
    float _307 = (dot(float3(_278, _290, _296), float3(1.0f, 0.0f, 1.4019999504089355f)) * _283) + (_297 * _265);
    float _308 = (dot(float3(_278, _290, _296), float3(1.0f, -0.3441399931907654f, -0.714139997959137f)) * _283) + (_297 * _266);
    float _309 = (dot(float3(_278, _290, _296), float3(1.0f, 1.7719999551773071f, 0.0f)) * _283) + (_297 * _267);
    _335 = select((_307 <= 0.040449999272823334f), (_307 * 0.07739938050508499f), exp2(log2((_307 + 0.054999999701976776f) * 0.9478673338890076f) * 2.4000000953674316f));
    _336 = select((_308 <= 0.040449999272823334f), (_308 * 0.07739938050508499f), exp2(log2((_308 + 0.054999999701976776f) * 0.9478673338890076f) * 2.4000000953674316f));
    _337 = select((_309 <= 0.040449999272823334f), (_309 * 0.07739938050508499f), exp2(log2((_309 + 0.054999999701976776f) * 0.9478673338890076f) * 2.4000000953674316f));
  } else {
    _335 = _235;
    _336 = _236;
    _337 = _237;
  }
  [branch]
  if (!((Constants_068 & 2) == 0)) {
    float4 _359 = t9.SampleLevel(s1, float3(((float((uint)f32tof16(min(max(_335, 0.0f), 65504.0f))) * 3.051853855140507e-05f) + 0.015625f), ((float((uint)f32tof16(min(max(_336, 0.0f), 65504.0f))) * 3.051853855140507e-05f) + 0.015625f), ((float((uint)f32tof16(min(max(_337, 0.0f), 65504.0f))) * 3.051853855140507e-05f) + 0.015625f)), 0.0f);
    _364 = _359.x;
    _365 = _359.y;
    _366 = _359.z;
  } else {
    _364 = _335;
    _365 = _336;
    _366 = _337;
  }
  float3 graded_hdr = float3(_364, _365, _366);
  float3 tonemapped = renodx::draw::ToneMapPass(graded_hdr);
  float3 output_color = renodx::color::pq::EncodeSafe(
      renodx::color::bt2020::from::BT709(tonemapped) * RENODX_DIFFUSE_WHITE_NITS,
      1.f);
  SV_Target.x = output_color.x;
  SV_Target.y = output_color.y;
  SV_Target.z = output_color.z;
  SV_Target.w = 1.0f;
  SV_Target_1.x = output_color.x;
  SV_Target_1.y = output_color.y;
  SV_Target_1.z = output_color.z;
  SV_Target_1.w = 1.0f;
  OutputSignature output_signature = { SV_Target, SV_Target_1 };
  return output_signature;
}
