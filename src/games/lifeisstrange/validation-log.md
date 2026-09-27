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
- Relevant draw order in the captured frame was `0x51229A9B` draw 246, `0x06A2A81D` draw 247, then `0xFC2A0632` draw 252.
- `0x51229A9B` draw 246 reads and writes a `482x272 R16G16B16A16_UNORM` resource.
- `0x06A2A81D` draw 247 follows 512 and samples a `1920x1080 R16G16B16A16_FLOAT` scene resource.
- `0xFC2A0632` draw 252 is the final relevant pass and writes to the `1920x1080 B8G8R8A8_UNORM` swapchain surface.
- Therefore the tested main-menu order is `512 -> 06A2 -> FC2A -> swap`, not `06A2 -> 512 -> FC2A`.
