#include "./shared.h"
#include "./common.hlsl"

Texture2D<float4> t0 : register(t0);
Texture3D<float4> t1 : register(t1);
SamplerState s0_s : register(s0);
SamplerState s1_s : register(s1);

cbuffer cb0 : register(b0)
{
  float4 cb0[2];
}

#define LetterboxHalfWidth   cb0[0].x
#define LetterboxHalfHeight  cb0[0].y
#define LetterboxEnable      cb0[0].z
#define SoftClipStrength     cb0[0].w
#define Saturation           cb0[1].x
#define WhiteTemperatureK    cb0[1].y

void main(
  float4 v0 : SV_POSITION0,
  float4 v1 : TEXCOORD0,
  out float4 o0 : SV_TARGET0)
{
  float4 col = t0.SampleLevel(s0_s, v1.xy, 0);
  o0.w = col.w;
  float4 colIn = col;

  if (RENODX_TONE_MAP_TYPE == 0.f) { // vanilla
    // lut
    col.xyz = renodx::color::srgb::EncodeSafe(col.xyz);
    col.xyz = col.rgb * (31.0 / 32.0) + (0.5 / 32.0);
    col.xyz = t1.SampleLevel(s1_s, col.rgb, 0).rgb;
    col.xyz = renodx::color::srgb::DecodeSafe(col.xyz);

    // luminance-preserving white balance (vanilla)
    float3 whiteBalance = BlackbodyTint(WhiteTemperatureK);
    float3 tinted = col.xyz * whiteBalance;
    tinted *= renodx::color::y::from::BT709(col.xyz) / max(1e-4, renodx::color::y::from::BT709(tinted));
    col.xyz = (WhiteTemperatureK != 6500.0) ? tinted : col.xyz;

    // tonemap
    col.xyz = softclip(col.xyz, SoftClipStrength);

    // saturation by Y (vanilla)
    float luma = saturate(renodx::color::y::from::BT709(col.xyz));
    col.xyz = lerp(luma, col.xyz, Saturation);
  } else {  // vanilla plus (hdr)
    // lut, scaled around the max channel
    float scale = renodx::tonemap::neutwo::ComputeMaxChannelScale(colIn.rgb);
    col.rgb *= scale;

    col.xyz = renodx::color::srgb::EncodeSafe(col.xyz);
    col.xyz = col.rgb * (31.0 / 32.0) + (0.5 / 32.0);
    col.xyz = t1.SampleLevel(s1_s, col.rgb, 0).rgb;
    col.xyz = renodx::color::srgb::DecodeSafe(col.xyz);

    col.rgb = col.rgb / scale;

    // better white balance
    col.xyz = (abs(WhiteTemperatureK - 6500.f) > 0.5f)
                  ? AdaptBT709Kelvin(col.xyz, WhiteTemperatureK)
                  : col.xyz;

    // tonemap
    col.xyz = softclipExtended(col.xyz, SoftClipStrength);

    renodx::color::grade::Config cg_config = renodx::color::grade::config::Create();
    cg_config.exposure = RENODX_TONE_MAP_EXPOSURE;
    cg_config.highlights = RENODX_TONE_MAP_HIGHLIGHTS;
    cg_config.shadows = RENODX_TONE_MAP_SHADOWS;
    cg_config.contrast = RENODX_TONE_MAP_CONTRAST;
    cg_config.flare = 0.10f * pow(RENODX_TONE_MAP_FLARE, 10.f);
    cg_config.saturation = RENODX_TONE_MAP_SATURATION;
    cg_config.dechroma = RENODX_TONE_MAP_BLOWOUT;
    cg_config.blowout = -1.f * (RENODX_TONE_MAP_HIGHLIGHT_SATURATION - 1.f);
    cg_config.hue_correction_strength = 0.f;
    col.xyz = renodx::color::grade::config::ApplyUserColorGrading(col.xyz, cg_config);

    col.xyz = anchoredCInfinityShoulder(col.xyz, RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS, 0.18f, 1.5f);

    // saturation by oklab (not perfect but better than vanilla)
    float3 oklab = renodx::color::oklab::from::BT709(col.xyz);
    oklab.yz *= saturate(Saturation);
    col.xyz = renodx::color::bt709::from::OkLab(oklab);
  }

  // letterbox / pillarbox mask
  bool outside = (abs(v1.w) > LetterboxHalfHeight) || (abs(v1.z) > LetterboxHalfWidth);
  o0.xyz = (LetterboxEnable > 0.0) ? (outside ? 0.0 : col.xyz) : col.xyz;
  o0.xyz = renodx::color::srgb::EncodeSafe(o0.xyz);
}
