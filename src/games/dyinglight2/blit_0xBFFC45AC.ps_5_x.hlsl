Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

void main(
    float4 v0 : SV_POSITION0,
    linear noperspective float2 v1 : TEXCOORD0,
    out float4 o0 : SV_TARGET0) {
  // Temporary boundary probe. This shader deliberately avoids shared.h so it
  // cannot depend on a RenoDX cbuffer that may not fit this root signature.
  o0 = float4(6.25, 6.25, 6.25, 1.0);
}
