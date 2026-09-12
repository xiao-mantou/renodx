#ifndef SRC_GAMES_F122_SHARED_H_
#define SRC_GAMES_F122_SHARED_H_

// Must be 32bit aligned
// Should be 4x32
// Trimmed to fit the game's smallest pipeline layout root constant budget (20 dwords).
struct ShaderInjectData {
  float peak_white_nits;
  float diffuse_white_nits;
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
  float debug_mode;
};

#ifndef __cplusplus
#if ((__SHADER_TARGET_MAJOR == 5 && __SHADER_TARGET_MINOR >= 1) || __SHADER_TARGET_MAJOR >= 6)
cbuffer shader_injection : register(b13, space50) {
#elif (__SHADER_TARGET_MAJOR < 5) || ((__SHADER_TARGET_MAJOR == 5) && (__SHADER_TARGET_MINOR < 1))
cbuffer shader_injection : register(b13) {
#endif
  ShaderInjectData shader_injection : packoffset(c0);
}

#define RENODX_TONE_MAP_TYPE                 shader_injection.tone_map_type
#define RENODX_PEAK_WHITE_NITS               shader_injection.peak_white_nits
#define RENODX_DIFFUSE_WHITE_NITS            shader_injection.diffuse_white_nits
#define RENODX_GRAPHICS_WHITE_NITS           renodx::color::bt2408::GRAPHICS_WHITE
#define RENODX_GAMMA_CORRECTION              GAMMA_CORRECTION_NONE
#define RENODX_TONE_MAP_PER_CHANNEL          0.f
#define RENODX_TONE_MAP_WORKING_COLOR_SPACE  0.f
#define RENODX_TONE_MAP_HUE_PROCESSOR        0.f
#define RENODX_TONE_MAP_HUE_CORRECTION       shader_injection.tone_map_hue_correction
#define RENODX_TONE_MAP_HUE_SHIFT            shader_injection.tone_map_hue_shift
#define RENODX_TONE_MAP_CLAMP_COLOR_SPACE    renodx::color::convert::COLOR_SPACE_NONE
#define RENODX_TONE_MAP_CLAMP_PEAK           renodx::color::convert::COLOR_SPACE_NONE
#define RENODX_TONE_MAP_EXPOSURE             shader_injection.tone_map_exposure
#define RENODX_TONE_MAP_HIGHLIGHTS           shader_injection.tone_map_highlights
#define RENODX_TONE_MAP_SHADOWS              shader_injection.tone_map_shadows
#define RENODX_TONE_MAP_CONTRAST             shader_injection.tone_map_contrast
#define RENODX_TONE_MAP_SATURATION           shader_injection.tone_map_saturation
#define RENODX_TONE_MAP_HIGHLIGHT_SATURATION shader_injection.tone_map_highlight_saturation
#define RENODX_TONE_MAP_BLOWOUT              shader_injection.tone_map_blowout
#define RENODX_TONE_MAP_FLARE                shader_injection.tone_map_flare
#define RENODX_COLOR_GRADE_STRENGTH          shader_injection.color_grade_strength
#define RENODX_RENO_DRT_TONE_MAP_METHOD      renodx::tonemap::renodrt::config::tone_map_method::DANIELE
#define RENODX_DEBUG_MODE                    shader_injection.debug_mode

#include "../../shaders/renodx.hlsl"

#endif

#endif  // SRC_GAMES_F122_SHARED_H_
