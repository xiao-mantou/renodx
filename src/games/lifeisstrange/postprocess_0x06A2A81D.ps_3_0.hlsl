sampler2D SceneColorTexture : register(s0);

struct PS_IN {
  float4 texcoord : TEXCOORD;
  float4 texcoord1 : TEXCOORD1;
  float4 texcoord2 : TEXCOORD2;
};

float4 main(PS_IN i) : COLOR {
  return tex2D(SceneColorTexture, i.texcoord1.xy);
}
