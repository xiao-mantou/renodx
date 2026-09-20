#ifndef SRC_GAMES_NFSUNBOUND_SHARED_H_
#define SRC_GAMES_NFSUNBOUND_SHARED_H_

// NFS Unbound's DX12 root signatures leave too little root-constant budget for injected settings,
// so this mod replaces the tonemap output shader with baked-in defaults instead of a shader
// injection cbuffer. Peak and paper white are baked into each preset variant.
#ifndef RENODX_PEAK_WHITE_NITS
#define RENODX_PEAK_WHITE_NITS 450.f
#endif
#ifndef RENODX_DIFFUSE_WHITE_NITS
#define RENODX_DIFFUSE_WHITE_NITS 203.f
#endif
#ifndef RENODX_GRAPHICS_WHITE_NITS
#define RENODX_GRAPHICS_WHITE_NITS 203.f
#endif
#ifndef RENODX_SDR_SHOULDER_START
#define RENODX_SDR_SHOULDER_START 0.25f
#endif

#define RENODX_GAMMA_CORRECTION GAMMA_CORRECTION_NONE
#define RENODX_TONE_MAP_TYPE renodx::draw::TONE_MAP_TYPE_RENO_DRT
#define RENODX_RENO_DRT_TONE_MAP_METHOD renodx::tonemap::renodrt::config::tone_map_method::NEUTWO
#define RENODX_RENO_DRT_SCALING_METHOD renodx::tonemap::renodrt::config::scaling_method::MAX_CHANNEL
#define RENODX_TONE_MAP_PER_CHANNEL 0.f
#define RENODX_TONE_MAP_WORKING_COLOR_SPACE 0.f
#define RENODX_TONE_MAP_HUE_PROCESSOR 0.f
#define RENODX_TONE_MAP_HUE_CORRECTION 1.f
#define RENODX_TONE_MAP_HUE_SHIFT 0.f
#define RENODX_TONE_MAP_CLAMP_COLOR_SPACE renodx::color::convert::COLOR_SPACE_NONE
#define RENODX_TONE_MAP_CLAMP_PEAK renodx::color::convert::COLOR_SPACE_NONE
#define RENODX_TONE_MAP_EXPOSURE 1.f
#define RENODX_TONE_MAP_HIGHLIGHTS 1.f
#define RENODX_TONE_MAP_SHADOWS 1.f
#define RENODX_TONE_MAP_CONTRAST 1.f
#define RENODX_TONE_MAP_SATURATION 1.f
#define RENODX_TONE_MAP_HIGHLIGHT_SATURATION 1.f
#define RENODX_TONE_MAP_BLOWOUT 0.f
#define RENODX_TONE_MAP_FLARE 0.f
#define RENODX_COLOR_GRADE_STRENGTH 1.f
#define RENODX_SWAP_CHAIN_OUTPUT_PRESET 1.f

#include "../../shaders/renodx.hlsl"

#endif  // SRC_GAMES_NFSUNBOUND_SHARED_H_
