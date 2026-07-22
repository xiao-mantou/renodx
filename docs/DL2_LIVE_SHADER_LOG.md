# Dying Light 2 Live Shader Verification Log

## 2026-07-22: `0x268BAB6D` visual verification

- Game state: DX12 game running in a rendered scene, RenoDX DevKit bridge connected.
- Safety constraints: no trace, snapshot draw capture, auto dump, or bulk shader dump was used.
- Live source: an isolated copy of the decompiled `0x268BAB6D.ps_5_0.hlsl` only.
- DevKit result: `devkit_load_live_shaders` compiled and activated one replacement.
- Visual result: a deliberate `float3(1.35, 0.80, 0.80)` output multiplier caused an obvious full-screen red tint.
- Recovery: `devkit_unload_live_shaders` succeeded and restored the original image.

Conclusion: `0x268BAB6D` is active in the final visible display chain and can be replaced safely through the isolated live-shader workflow. It is a LUT/color-grade pass; this evidence alone does not prove it is the primary pre-HDR tonemapper.

## 2026-07-22: `0xAD085E81` visual verification

- Live source: a separate directory containing only the decompiled `0xAD085E81.ps_5_0.hlsl`.
- Probe: the same temporary `float3(1.35, 0.80, 0.80)` output multiplier.
- DevKit result: one live shader compiled and activated.
- Visual result: an obvious full-screen red tint.
- Recovery: live replacements were unloaded successfully.

Conclusion: `0xAD085E81` is also active in the visible final chain. Its original code is a single texture sample followed by `log2`, a configurable RGB multiplier, and `exp2`, so it is a gamma/power pass rather than a complete tonemapper.

## Next safe investigation

Determine the ordering and output color space of `0x268BAB6D` (LUT/color-grade) and `0xAD085E81` (gamma/power) before modifying the shipping HDR replacement. Do not re-enable trace or DevKit bulk shader dumping for this work.

## 2026-07-22: combined order probe

- `0x268BAB6D` was changed to preserve red only; `0xAD085E81` was changed to preserve green only.
- Both isolated live replacements compiled and activated together.
- Observed result: the world image was black while UI was green.
- Replacements were unloaded immediately after observation.

Interpretation: the world path is consistent with `0x268BAB6D` producing red-only output before `0xAD085E81` later preserves green only, yielding black. The green UI indicates a separate composition path. This is strong evidence that `0x268BAB6D` is the earlier world-color/LUT candidate and `0xAD085E81` is a later gamma/power stage; the HDR intervention should continue to target `0x268BAB6D`.

## 2026-07-22: `0x268BAB6D` input-range probe

- Probe output: each channel was set to one only when the corresponding sampled `t0` channel was greater than `1.0`.
- Visual result: most of the image was black, while bright regions retained dark color.
- Interpretation: at least one `t0` channel exceeded `1.0` in visible highlights. The colored result is expected from the per-channel threshold, rather than a binary all-channel white test.

Conclusion: `0x268BAB6D` receives a linear scene/post-process signal with SDR-above highlight headroom. It is a viable shader-side HDR bridge capture point; it is not merely a completed SDR swapchain image.
