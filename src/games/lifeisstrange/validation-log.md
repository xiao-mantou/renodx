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
- The next diagnostic boundary is therefore `06A2 -> 512 -> FC2A`, especially the 512 output and its B8 target.
- Do not infer a pass chain from shader draw order alone. Use the producer RTV handle and the consumer SRV handle from the same snapshot.
- Do not add another 512 replacement until its original SM3 assembly has been checked for output saturation and its replacement has an equivalent register/swizzle mapping.
