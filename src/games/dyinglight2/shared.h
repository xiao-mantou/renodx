#ifndef SRC_DYINGLIGHT2_SHARED_H_
#define SRC_DYINGLIGHT2_SHARED_H_

// Must be 32bit aligned
// Should be 4x32
struct ShaderInjectData {
  float peak_white_nits;
  float diffuse_white_nits;
  float graphics_white_nits;
  float color_grade_strength;
  float tone_map_type;
  float tone_map_exposure;
  float tone_map_highlights;
  float tone_map_shadows;
  float tone_map_contrast;
  float tone_map_saturation;
  float tone_map_highlight_saturation;
  float tone_map_blowout;
  float tone_map_flare;
  float tone_map_hue_correction;
  float tone_map_hue_shift;
  float tone_map_working_color_space;
  float tone_map_clamp_color_space;
  float tone_map_clamp_peak;
  float tone_map_hue_processor;
  float tone_map_per_channel;
  float gamma_correction;
  float custom_auto_exposure;
  float custom_lens_flare;
  float custom_lut_scaling;
  float swap_chain_encoding;
  float swap_chain_encoding_color_space;
  float clamp_swapchain_output;
  float debug_mode;
  float tone_map_white_clip;
  float auto_exposure_min;
  float auto_exposure_max;
  float hdr_exposure_protection_start;
  float hdr_exposure_protection_end;
  float renodrt_tone_map_method;
  float swap_chain_output_preset;
  float renodrt_padding_1;
  float renodrt_padding_2;
  float luminance_stage_probe;
  float luminance_stage_padding_1;
  float luminance_stage_padding_2;
  float night_scene_gain;
};

#ifndef __cplusplus
#if ((__SHADER_TARGET_MAJOR == 5 && __SHADER_TARGET_MINOR >= 1) || __SHADER_TARGET_MAJOR >= 6)
cbuffer shader_injection : register(b13, space50) {
#elif (__SHADER_TARGET_MAJOR < 5) || ((__SHADER_TARGET_MAJOR == 5) && (__SHADER_TARGET_MINOR < 1))
cbuffer shader_injection : register(b13) {
#endif
  ShaderInjectData shader_injection : packoffset(c0);
}

#define RENODX_TONE_MAP_TYPE                   shader_injection.tone_map_type
#define RENODX_PEAK_WHITE_NITS                 shader_injection.peak_white_nits
#define RENODX_DIFFUSE_WHITE_NITS              shader_injection.diffuse_white_nits
// DL2's current HDR bridge is upstream of the UI composite. Keep the
// intermediate/output reference white fixed until a real UI composite pass
// is identified; exposing this field made it alter the whole scene.
#define RENODX_GRAPHICS_WHITE_NITS             203.f
// Actual UI control. Keep GRAPHICS_WHITE fixed for the common Linear BT.709
// proxy unit, while matched gamma-domain UI shaders use this value explicitly.
#define RENODX_UI_WHITE_NITS                   shader_injection.graphics_white_nits
#define RENODX_GAMMA_CORRECTION                shader_injection.gamma_correction
#define RENODX_TONE_MAP_HUE_PROCESSOR          shader_injection.tone_map_hue_processor
#define RENODX_TONE_MAP_HUE_CORRECTION         shader_injection.tone_map_hue_correction
#define RENODX_TONE_MAP_PER_CHANNEL            shader_injection.tone_map_per_channel
#define RENODX_TONE_MAP_EXPOSURE               shader_injection.tone_map_exposure
#define RENODX_TONE_MAP_HIGHLIGHTS             shader_injection.tone_map_highlights
#define RENODX_TONE_MAP_SHADOWS                shader_injection.tone_map_shadows
#define RENODX_TONE_MAP_CONTRAST               shader_injection.tone_map_contrast
#define RENODX_TONE_MAP_SATURATION             shader_injection.tone_map_saturation
#define RENODX_TONE_MAP_HIGHLIGHT_SATURATION   shader_injection.tone_map_highlight_saturation
#define RENODX_TONE_MAP_BLOWOUT                shader_injection.tone_map_blowout
#define RENODX_TONE_MAP_FLARE                  shader_injection.tone_map_flare
#define RENODX_COLOR_GRADE_STRENGTH            shader_injection.color_grade_strength
#define RENODX_SWAP_CHAIN_ENCODING             shader_injection.swap_chain_encoding
#define RENODX_SWAP_CHAIN_OUTPUT_PRESET        shader_injection.swap_chain_output_preset
#define RENODX_SWAP_CHAIN_USE_HDR10            shader_injection.renodrt_padding_1
#define RENODX_SWAP_CHAIN_SOURCE_OVERRIDE      shader_injection.renodrt_padding_2
#define RENODX_DLSS_FG_SOURCE_SEMANTIC         shader_injection.luminance_stage_padding_1
#define RENODX_SWAP_CHAIN_ENCODING_COLOR_SPACE shader_injection.swap_chain_encoding_color_space
#define RENODX_CLAMP_SWAPCHAIN_OUTPUT             shader_injection.clamp_swapchain_output
#define RENODX_RENO_DRT_TONE_MAP_METHOD        shader_injection.renodrt_tone_map_method
#define RENODX_RENO_DRT_SCALING_METHOD         shader_injection.tone_map_per_channel
#define CUSTOM_AUTO_EXPOSURE                   shader_injection.custom_auto_exposure
#define RENODX_AUTO_EXPOSURE_MIN               shader_injection.auto_exposure_min
#define RENODX_AUTO_EXPOSURE_MAX               shader_injection.auto_exposure_max
#define RENODX_HDR_EXPOSURE_PROTECTION_START   shader_injection.hdr_exposure_protection_start
#define RENODX_HDR_EXPOSURE_PROTECTION_END     shader_injection.hdr_exposure_protection_end
#define CUSTOM_LENS_FLARE                      shader_injection.custom_lens_flare
#define CUSTOM_LUT_SCALING                     shader_injection.custom_lut_scaling
#define RENODX_DEBUG_MODE                      shader_injection.debug_mode
#define RENODX_LUMINANCE_STAGE_PROBE           shader_injection.luminance_stage_probe
#define RENODX_RENO_DRT_WHITE_CLIP             shader_injection.tone_map_white_clip
#define RENODX_NIGHT_SCENE_GAIN                shader_injection.night_scene_gain

#include "../../shaders/renodx.hlsl"

// Shared stage signature for the DL2 HDR path. Each participating shader only
// responds to its own stage, so a missing replacement cannot be mistaken for
// a successful downstream probe.
bool Dl2RenderLuminanceStageProbe(float2 uv, float stage, out float3 output) {
  if (abs(RENODX_LUMINANCE_STAGE_PROBE - stage) > 0.25) {
    output = 0.0;
    return false;
  }

  if (uv.x > 0.02 && uv.x < 0.08 && uv.y > 0.02 && uv.y < 0.08) {
    const float3 stage_colors[6] = {
        float3(1.0, 0.0, 0.0),
        float3(0.0, 1.0, 0.0),
        float3(0.0, 0.4, 1.0),
        float3(1.0, 1.0, 0.0),
        float3(1.0, 0.0, 1.0),
        float3(0.0, 1.0, 1.0),
    };
    output = stage_colors[min((uint)stage - 1u, 5u)];
    return true;
  }

  if (uv.x > 0.55 && uv.x < 0.95 && uv.y > 0.82 && uv.y < 0.92) {
    const uint index = min((uint)((uv.x - 0.55) * 10.0), 3u);
    const float levels[4] = {0.25, 1.0, 4.0, 16.0};
    output = levels[index].xxx;
    return true;
  }

  output = 0.0;
  return false;
}

#endif

#endif  // SRC_DYINGLIGHT2_SHARED_H_
