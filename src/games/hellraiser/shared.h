#ifndef SRC_GAMES_HELLRAISER_SHARED_H_
#define SRC_GAMES_HELLRAISER_SHARED_H_

// Must be 32bit aligned
struct ShaderInjectData {
  float black_fix;
  float crush_nits;
  float shadow_gain;
  float shadow_knee_nits;

  float shadow_neutral;
  float neutral_hi_nits;
  float keep_grain;
  float padding;
};

#ifndef __cplusplus

cbuffer shader_injection : register(b13, space50) {
  ShaderInjectData shader_injection : packoffset(c0);
}

#define RENODX_HDR_BLACK_FIX        shader_injection.black_fix
#define RENODX_HDR_CRUSH_NITS       shader_injection.crush_nits
#define RENODX_HDR_SHADOW_GAIN      shader_injection.shadow_gain
#define RENODX_HDR_SHADOW_KNEE_NITS shader_injection.shadow_knee_nits
#define RENODX_HDR_SHADOW_NEUTRAL   shader_injection.shadow_neutral
#define RENODX_HDR_NEUTRAL_HI_NITS  shader_injection.neutral_hi_nits
#define RENODX_HDR_KEEP_GRAIN       shader_injection.keep_grain

#include "../../shaders/renodx.hlsl"

#endif  // __cplusplus

#endif  // SRC_GAMES_HELLRAISER_SHARED_H_
