#ifndef SRC_GAMES_HELLRAISER_SHARED_H_
#define SRC_GAMES_HELLRAISER_SHARED_H_

// Must be 32bit aligned
struct ShaderInjectData {
  float black_fix;
  float crush_nits;
  float shadow_neutral;
  float neutral_hi_nits;
};

#ifndef __cplusplus

cbuffer shader_injection : register(b13, space50) {
  ShaderInjectData shader_injection : packoffset(c0);
}

#define RENODX_HDR_BLACK_FIX        shader_injection.black_fix
#define RENODX_HDR_CRUSH_NITS       shader_injection.crush_nits
#define RENODX_HDR_SHADOW_NEUTRAL   shader_injection.shadow_neutral
#define RENODX_HDR_NEUTRAL_HI_NITS  shader_injection.neutral_hi_nits

#include "../../shaders/renodx.hlsl"

#endif  // __cplusplus

#endif  // SRC_GAMES_HELLRAISER_SHARED_H_
