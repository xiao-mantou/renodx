#include "./shared.h"

float3 ColorScale : register(c0);
float4 OverlayColor : register(c7);
sampler2D SceneColorTexture : register(s0);

float4 main(float2 texcoord : TEXCOORD) : COLOR {
  float4 scene = tex2D(SceneColorTexture, texcoord);
  float3 overlay = scene.rgb * -ColorScale + OverlayColor.rgb;
  float3 output_color = OverlayColor.w * overlay + scene.rgb * ColorScale;

  if (RENODX_TONE_MAP_TYPE != 0.f) {
    output_color = renodx::draw::RenderIntermediatePass(renodx::color::srgb::DecodeSafe(output_color));
  }

  return float4(output_color, 1.f);
}
