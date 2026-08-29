#include "./ui.hlsli"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[1];
}

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    float4 v2 : TEXCOORD1,
    out float4 o0 : SV_TARGET0) {
  const float2 p = v2.xy - 0.5;
  const bool has_position = dot(p, p) > 0.0;
  const float max_abs = max(abs(p.x), abs(p.y));
  const float min_abs = min(abs(p.x), abs(p.y));
  const float ratio = max_abs > 0.0 ? min_abs / max_abs : 0.0;
  const float ratio2 = ratio * ratio;
  float atan_part = ratio2 * 0.0208350997 - 0.085133001;
  atan_part = ratio2 * atan_part + 0.180141002;
  atan_part = ratio2 * atan_part - 0.330299497;
  const float atan_scale = ratio2 * atan_part + 0.999866009;
  float angle = ratio * atan_scale;
  angle = angle * -2.0 + 1.57079637;
  angle = (abs(p.y) < abs(p.x)) ? angle : 0.0;
  angle += ratio * atan_scale;
  angle += p.y < 0.0 ? -3.141593 : 0.0;

  // Preserve the native quadrant orientation. The sign flip is based on the
  // signed min/max comparisons, not on p.y alone; this keeps the radial sweep
  // anchored at the same point as the original shader.
  const bool min_is_negative = min(p.x, p.y) < -min(p.x, p.y);
  const bool max_is_nonnegative = max(p.x, p.y) >= -max(p.x, p.y);
  const float signed_angle = (max_is_nonnegative && min_is_negative)
                                ? -angle
                                : angle;

  float phase = -signed_angle * 0.159154952 + 0.5 - cb0[0].z + 1.0;
  const float phase_sign = phase >= -phase ? 1.0 : -1.0;
  phase = phase_sign * frac(abs(phase));
  const float2 window = clamp((phase - cb0[0].xy) * 10000.0, -1.0, 1.0);
  const float mask = has_position
                         ? saturate((1.0 - window.x) * window.y)
                         : 0.0;

  const float4 texture_color = t0.Sample(s0_s, v2.zw);
  const float4 ui_color = v1 * texture_color;
  o0.rgb = FinalizeDL2UI(ui_color.rgb);
  o0.a = ui_color.a * mask;
}
