# Life Is Strange validation log

- API: native D3D9, shader model 3.0. `0x06A2A81D` is the final post-process/color-grading pass.
- DevKit captured the full-frame shader set and the `0x06A2A81D` `.cso`.
- The `HlslDecompiler` fork builds and decompiles this CSO in normal and `--ast` modes; both emit identical assembly.
- Normal-mode output and the DevKit `.msasm` are the authority for register masks and swizzles. AST output is inspection-only.
- This pass corrected nine confirmed SM3-to-HLSL differences in `0x06A2A81D`, including exact `def` constant bits.
- HDR and ToneMap behavior are unchanged. The readback pass temporarily enables only intermediate FP16 resource upgrades and disables the final proxy.
- Readback validation keeps only intermediate FP16 render-target upgrades, disables final swap/proxy, and samples every 120 draws.
- Readback validation uses create-time FP16 upgrades and disables runtime resource-view cloning to avoid stale D3D9 RTV clone handles.
- Stage 1 sets `readback_resource_upgrade = false`: callback only, no resource replacement, no GPU readback allocation, and no queue wait.
- `0x06A2A81D` is the scene post-process pass; its output is the first B8 intermediate in the native path.
- The original `0x51229A9B` CSO and decompiler outputs are retained in the external DevKit dump; no verified replacement shader is currently in `src/games/lifeisstrange`.

## Main-menu DevKit snapshot (2026-09-27)

- Captured with DevKit only; RenoDX/proxy was not active.
- The full-screen chain in the captured frame was `0x06A2A81D` draw 230, then `0x51229A9B` draws 231/234, then `0xFC2A0632` draw 235.
- `0x06A2A81D` samples the live `1920x1080 R16G16B16A16_FLOAT` scene resource and writes to a `1920x1080 B8G8R8A8_UNORM` resource.
- The live 06A2 input analysis reached R=28.328, G=19.141, B=15.633, and luminance=20.841, with 31,240 pixels above 1.0.
- `0x51229A9B` draw 231/234 samples resource `0x36D6FC40`, which is the 06A2 output, and writes resource `0x36D6F440`.
- `0xFC2A0632` samples resource `0x36D6F440`, which is the 512 output, and writes to the `1920x1080 B8G8R8A8_UNORM` swapchain surface.
- The other 512 draws in this frame target smaller intermediate resources and are separate passes.
- Therefore the verified main-menu full-screen order is `scene FP16 -> 06A2 -> 512 -> FC2A -> swap`.

## Current test conclusions

- `LifeIsStrange_ForceFC2AWhite=1` reaches `800+ nits` with the DX11 proxy enabled. This validates the FC2A-to-proxy output path only.
- `LifeIsStrange_Force06A2White=1` also reaches `800+ nits`. This does not prove that normal HDR values survive the intervening 512 pass.
- The 512 replacement callback now requests and binds the existing FP16 clone only for its `1920x1080 B8G8R8A8_UNORM` RTV; smaller UI/intermediate draws are left unchanged.
- Do not infer a pass chain from shader draw order alone. Use the producer RTV handle and the consumer SRV handle from the same snapshot.
- The 512 white probe is diagnostic only; because the shader modulates sampled color by interpolated `v1`, judge it using the bound FP16 RTV and downstream output, not an assumption that the whole screen must become white.
- The 512 MSASM has no `saturate` or `mad_sat`: it is exactly `texld r0`, `dp4 r0.w`, then `mul oC0, r0, v1`.
- A strict-equivalent 512 HLSL probe now exists, but is disabled by default. Enable only `LifeIsStrange_Force512White=1` to replace the 512 shader and force output `4.0`.

## Diagnostic configuration

- `LifeIsStrange_EnableDX11Proxy=1` enables the D3D11 HDR10 presentation proxy.
- `LifeIsStrange_AB_Disable06A2Replacement=1` removes only the 06A2 shader replacement.
- `LifeIsStrange_AB_DisableIntermediateUpgrade=1` removes the dedicated FP16 intermediate rules and automatically disables the proxy for a valid vanilla-path comparison.
- `LifeIsStrange_IntermediateBindingDiagnostic=1` enables the 512/FC2A binding diagnostics and SRV clone rebinding. It defaults to `0` so normal runs do not rewrite descriptor bindings.
- `LifeIsStrange_ForceProxyWhite`, `LifeIsStrange_Force06A2White`, `LifeIsStrange_ForceFC2AWhite`, and `LifeIsStrange_Force512White` default to `0`; they are probes only and should not be used for normal gameplay.
