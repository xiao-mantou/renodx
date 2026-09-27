#include "./shared.h"

float4 TextureComponentReplicateAlpha : register(c0);
sampler2D InputTexture : register(s0);

struct PS_IN {
  float2 texcoord : TEXCOORD0;
  float4 modulation : TEXCOORD1;
};

float4 main(PS_IN i) : COLOR {
  // Exact SM3 sequence from the DevKit MSASM:
  // texld r0, v0, s0; dp4 r0.w, r0, c0; mul oC0, r0, v1.
  float4 color = tex2D(InputTexture, i.texcoord);
  color.w = dot(color, TextureComponentReplicateAlpha);
  float4 output_color = color * i.modulation;
  output_color = lerp(output_color, 4.f, step(0.5f, LIFEISSTRANGE_FORCE_512_WHITE));
  return output_color;
}
