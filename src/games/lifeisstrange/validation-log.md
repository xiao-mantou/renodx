# Life Is Strange validation log

- API: native D3D9, shader model 3.0. `0x06A2A81D` is the final post-process/color-grading pass.
- DevKit captured the full-frame shader set and the `0x06A2A81D` `.cso`.
- The `HlslDecompiler` fork builds and decompiles this CSO in normal and `--ast` modes; both emit identical assembly.
- Normal-mode output and the DevKit `.msasm` are the authority for register masks and swizzles. AST output is inspection-only.
- This pass corrected nine confirmed SM3-to-HLSL differences in `0x06A2A81D`, including exact `def` constant bits.
- HDR and ToneMap behavior are unchanged. The readback pass temporarily enables only intermediate FP16 resource upgrades and disables the final proxy.
- Readback validation keeps only intermediate FP16 render-target upgrades, disables final swap/proxy, and samples every 120 draws.
- Readback validation uses create-time FP16 upgrades and disables runtime resource-view cloning to avoid stale D3D9 RTV clone handles.
