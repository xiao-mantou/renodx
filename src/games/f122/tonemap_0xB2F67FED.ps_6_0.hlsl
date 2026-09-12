/*
 * Copyright (C) 2026 xiao-mantou
 * SPDX-License-Identifier: MIT
 */

#include "./common.hlsl"

// Vanilla bindings preserved from the original 0xB2F67FED shader.
struct HdrParams {
  uint hdr;
  uint hdrExpandedGamut;
  uint hdrNativeUI;
  float hdrScale;
  float uiHdrScale;
  float maxBrightnessOfHDRScene;
  float maxBrightnessOfHDRSceneNormalizedLinear;
  float maxBrightnessOfTV;
  float softShoulderPercentage;
  float softShoulderStartNormalizedLinear;
  float softShoulderStart2084;
  float maxBrightnessOfTV2084;
  float maxBrightnessOfHDRScene2084;
  float hdrReconstructionLerpFactor;
  float uiScRgbScale;
  float uiPaperWhiteNits;
  float4 contentToMonitor[4];
};

struct ViewportInfo {
  float4 viewportXyWhUNorm;
  uint4 viewportXyWhPixels;
  float4 rtPixelCoordToNdcScaleBias;
  float4 vpPixelCoordToNdcScaleBias;
  float4 ndcToRenderTargetUvScaleBias;
  float4 vpUvToRenderTargetUvScaleBias;
  float4 vpUvToRenderTargetUvScaleBiasRecip;
  float4 rtUvToNdcScaleBias;
  float4 rtPixelCoordToRenderTargetUvScaleBias;
};

StructuredBuffer<float> g_sceneLuminanceAndExposure : register(t39);
Texture2D<float4> inputRT : register(t0);
Texture2D<float4> inputGodRays : register(t1);
Texture2D<float4> inputBloom : register(t3);
Texture2D<float4> inputLensStreaks : register(t4);

cbuffer g_scene : register(b4) {
  HdrParams hdr : packoffset(c255.x);
  ViewportInfo viewportInfo : packoffset(c263.x);
};

cbuffer toneMapSettings : register(b0) {
  float4 colorXFormLUT[4] : packoffset(c000.x);
  float lutGammaAdjustment : packoffset(c004.x);
  uint lutUseTexture : packoffset(c004.y);
  uint lutTextureSlice : packoffset(c004.z);
  uint lutLateInPhotomode : packoffset(c004.w);
  float toneMapMaxLum : packoffset(c005.x);
  float toneMapMinLum : packoffset(c005.y);
  float toneMapKeyValue : packoffset(c005.z);
  float toneMapThreshold : packoffset(c005.w);
  float toneMapAdaptationSpeedDL : packoffset(c006.x);
  float toneMapAdaptationSpeedLD : packoffset(c006.y);
  float toneMapLumScale : packoffset(c006.z);
  float toneMapMaxSceneLum : packoffset(c006.w);
  uint toneMapEnabled : packoffset(c007.x);
  uint toneMapEnableLumMeter : packoffset(c007.y);
  uint toneMapTotalNumOfPixels : packoffset(c007.z);
  float toneMapHistogramLowPercent : packoffset(c007.w);
  float toneMapHistogramHighPercent : packoffset(c008.x);
  uint toneMapHistogramMode : packoffset(c008.y);
  float tonemapBloomRecipMax : packoffset(c008.z);
  float tonemapBloomScale : packoffset(c008.w);
  float tonemapLensStreaksScale : packoffset(c009.x);
  float inputTargetWidth : packoffset(c009.y);
  float inputTargetHeight : packoffset(c009.z);
  float hdrMagicNumber : packoffset(c009.w);
  uint useAcesForSdr : packoffset(c010.x);
  uint useAcesForHdr : packoffset(c010.y);
  uint exposureMode : packoffset(c010.z);
  float cameraShutterSpeed : packoffset(c010.w);
  float cameraFStop : packoffset(c011.x);
  float cameraISO : packoffset(c011.y);
  float cameraTmin : packoffset(c011.z);
  float cameraTmax : packoffset(c011.w);
  float inputTargetX : packoffset(c012.x);
  float inputTargetY : packoffset(c012.y);
  float2 bloomMinUv : packoffset(c012.z);
  float2 bloomMaxUv : packoffset(c013.x);
  float2 godRaysMinUv : packoffset(c013.z);
  float2 godRaysMaxUv : packoffset(c014.x);
  float toneMapRecipMaxSceneLum : packoffset(c014.z);
  int enableGodRays : packoffset(c014.w);
  int enableStreaks : packoffset(c015.x);
};

cbuffer vignetteParams : register(b1) {
  float4 vignetteParams1 : packoffset(c000.x);
  float4 vignetteParams2 : packoffset(c001.x);
  float4 vignetteColourTL : packoffset(c002.x);
  float4 vignetteTransition : packoffset(c003.x);
  float vignetteShape : packoffset(c004.x);
  float vignetteAmount : packoffset(c004.y);
  float vignetteBlend : packoffset(c004.z);
  float vignettePad : packoffset(c004.w);
};

SamplerState g_linear : register(s7);

float3 F1Vignette(float3 color, float2 uv) {
  float2 ndc = (viewportInfo.vpUvToRenderTargetUvScaleBiasRecip.xy * 2.0f)
                   * (uv - viewportInfo.vpUvToRenderTargetUvScaleBias.zw)
               + -1.0f;
  float angle = vignetteParams2.x * 0.01745329424738884f;
  float sin_angle = sin(angle);
  float cos_angle = cos(angle);
  float x = (vignetteParams1.x * 0.8899999856948853f) * ((dot(ndc, float2(cos_angle, -sin_angle)) * vignetteShape) + vignetteParams1.z);
  float y = (vignetteParams1.y * 0.8899999856948853f) * ((dot(ndc, float2(sin_angle, cos_angle)) * vignetteShape) + vignetteParams1.w);
  float falloff = saturate(mad(saturate(dot(float2(x, y), float2(x, y))), vignetteTransition.x, vignetteTransition.y));
  float strength = vignetteBlend * saturate((falloff * falloff) * (3.0f - (falloff * 2.0f)) * vignetteAmount);
  return lerp(color, vignetteColourTL.rgb, strength);
}

float4 main(
  noperspective float4 SV_Position : SV_Position,
  nointerpolation uint SV_SampleIndex : SV_SampleIndex,
  linear float2 TEXCOORD : TEXCOORD
) : SV_Target {
  float exposure = g_sceneLuminanceAndExposure.Load(1) * g_sceneLuminanceAndExposure.Load(2);

  float3 godRays = 0.0f;
  if (enableGodRays != 0) {
    godRays = inputGodRays.Sample(g_linear, clamp(TEXCOORD, godRaysMinUv, godRaysMaxUv)).rgb;
  }
  float3 streaks = 0.0f;
  if (enableStreaks != 0) {
    streaks = inputLensStreaks.Sample(g_linear, TEXCOORD.xy).rgb;
  }
  float3 bloom = inputBloom.Sample(g_linear, clamp(TEXCOORD, bloomMinUv, bloomMaxUv)).rgb;

  float3 scene = inputRT.Load(int3(int(SV_Position.x), int(SV_Position.y), 0)).rgb;
  float3 untonemapped = (scene + godRays) * exposure;

  // Vanilla EGO tonemap: filmic curve + bloom/streaks + color transform.
  float3 vanilla_curve = F1FilmicCurve(untonemapped);
  float3 neutral_sdr = vanilla_curve + tonemapBloomScale * bloom + tonemapLensStreaksScale * streaks;
  float3 graded_sdr = (lutLateInPhotomode == 0) ? F1ColorXFormLUT(neutral_sdr, colorXFormLUT) : neutral_sdr;

  float display_scale = 0.012500000186264515f * hdr.maxBrightnessOfTV * hdr.hdrScale;

  // TEMPORARY injection diagnostic: R = peak/4000, G = game/500, B = type/3.
  if (hdr.hdr != 0) {
    return float4(
        saturate(RENODX_PEAK_WHITE_NITS / 4000.f),
        saturate(RENODX_DIFFUSE_WHITE_NITS / 500.f),
        saturate(RENODX_TONE_MAP_TYPE / 3.f),
        1.f);
  }

  // Validate the injected settings before using them; unbound or garbage root constants must fall back
  // to the vanilla path instead of producing a broken image.
  const bool valid_injection = (RENODX_TONE_MAP_TYPE >= 1.f && RENODX_TONE_MAP_TYPE <= 3.f)
                               && (RENODX_PEAK_WHITE_NITS >= 48.f && RENODX_PEAK_WHITE_NITS <= 4000.f)
                               && (RENODX_DIFFUSE_WHITE_NITS >= 48.f && RENODX_DIFFUSE_WHITE_NITS <= 500.f)
                               && (RENODX_DEBUG_MODE >= 0.f && RENODX_DEBUG_MODE <= 3.f);

  if (valid_injection && RENODX_DEBUG_MODE > 0.f) {
    if (RENODX_DEBUG_MODE == 1.f) {
      return float4(
          RENODX_PEAK_WHITE_NITS / 1000.f,
          RENODX_DIFFUSE_WHITE_NITS / 203.f,
          RENODX_TONE_MAP_TYPE / 10.f,
          1.f);
    }
    if (RENODX_DEBUG_MODE == 2.f) {
      return float4(
          hdr.hdr / 10.f,
          display_scale / 10.f,
          hdr.hdrScale * 10.f,
          1.f);
    }
    return float4(
        saturate(exposure / 10.f),
        renodx::color::y::from::BT709(untonemapped) / 10.f,
        renodx::color::y::from::BT709(neutral_sdr) / 10.f,
        1.f);
  }

  float3 output = graded_sdr;  // Vanilla fallback (SDR output, vanilla preset, or invalid injection).
  if (hdr.hdr != 0
      && display_scale > 0.0f
      && valid_injection) {
    // Normalize the scene so the game's paper white (F1FilmicCurve(F1_PAPER_WHITE) == 1.0)
    // becomes 1.0, matching the Hable output reference used by UpgradeToneMap and RenoDRT.
    float3 normalized_untonemapped = untonemapped / F1_PAPER_WHITE;
    float3 tonemapped = renodx::draw::ToneMapPass(normalized_untonemapped, graded_sdr, neutral_sdr);
    // The game's final copy pass scales this buffer by 0.0125 * maxBrightnessOfTV * hdrScale
    // into scRGB, so scale for the requested paper white and let peaks go above 1.0.
    output = tonemapped * (RENODX_DIFFUSE_WHITE_NITS / (80.0f * display_scale));
  }

  output = F1Vignette(output, TEXCOORD);
  return float4(output, 1.0f);
}
