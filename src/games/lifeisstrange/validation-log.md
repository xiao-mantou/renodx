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
- `0x06A2A81D` output is confirmed bound to an `R16G16B16A16_FLOAT` clone.
- `0x51229A9B` samples the intermediate through `s0`; key B8 input instances require explicit rebinding to their FP16 clone.
- The next investigation stage is the chain after `0x51229A9B` toward the final output/proxy.

## Main-menu DevKit snapshot (2026-09-27)

- Captured with DevKit only; RenoDX/proxy was not active.
- The full-screen chain in the captured frame was `0x06A2A81D` draw 230, then `0xFC2A0632` draw 235.
- `0x06A2A81D` samples the live `1920x1080 R16G16B16A16_FLOAT` scene resource and writes to a `1920x1080 B8G8R8A8_UNORM` resource.
- The live 06A2 input analysis reached R=28.328, G=19.141, B=15.633, and luminance=20.841, with 31,240 pixels above 1.0.
- `0xFC2A0632` then samples that `1920x1080 B8G8R8A8_UNORM` resource and writes to the `1920x1080 B8G8R8A8_UNORM` swapchain surface.
- `0x51229A9B` had 12 draws in this frame. Its final listed draw 234 reads and writes a separate `1920x1080 B8G8R8A8_UNORM` resource; its other draws include `482x272 R16G16B16A16_UNORM` targets. These 512 draws are not the direct full-screen predecessor of 06A2 in this snapshot.
- Therefore the tested main-menu full-screen order is `scene FP16 -> 06A2 -> B8 intermediate -> FC2A -> swap`.
