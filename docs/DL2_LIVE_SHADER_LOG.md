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

## 2026-07-23: deployed addon verification

- The deployed `renodx-dyinglight2.addon64` was updated at 19:51 and the matching ReShade session began at 19:51.
- ReShade logged `Registered API-based runtime replacement: 0x268bab6d`, `Added replacement 0x268bab6d`, and `shader hash seen: 0x268bab6d, matched: YES`.
- Therefore the deployed addon and the `0x268BAB6D` replacement are active; a missing or stale replacement is not the reason Peak Brightness has little effect.
- After the range probe was corrected to use `max(t0.r, max(t0.g, t0.b))`, the whole scene was still blue. For this current scene, every sampled `t0` component at this LUT pass is at or below `1.0`.

Conclusion: `0x268BAB6D` is a verified active SDR-bounded LUT/color-grade pass in the current render path. It should not be treated as the pre-SDR HDR source for Peak Brightness recovery. Continue upstream from this pass; do not re-enable the old copied LUT templates for `0x4D2B3F4D`, `0x79B3C079`, `0x8A1C8855`, or `0xA766966E` without matching current-game bytecode and interfaces.

## 2026-07-23: ReShade log noise

The remaining log flood was not DevKit shader tracking. It was the D3D12 successful fast-clone path:

`utils::resource::upgrade::OnInitResourceView(copied d3d12 fast clone descriptor, ...)`

The per-view success log was removed. Failure and warning logging remain available.

## 2026-07-23: upstream HDR boundary candidate

- Offline screening found `0x3E36DA5B` to be a one-target full-screen pass that samples a scene texture, applies a scalar exposure texture, then uses a rational curve followed by `saturate`.
- An isolated live probe showed mostly SDR-range values, with green/yellow and occasional red highlights. Therefore its input retains values above 1.0 (and above 4.0 in some lights) before the game clamps them.
- A live gain probe did not change the `0x268BAB6D` input-range visualization. This does not establish a direct resource dependency because DevKit live replacement/unload rebuilds the device replacement set, so `0x3E36DA5B` remains an independent candidate rather than a shipping replacement.

## 2026-07-23: test-M crash fix (shader signature mismatch)

### Problem
- Deployed `renodx-dyinglight2.addon64` built from commit `a5527e0` (with `SV_POSITION0` fix).
- Game window became unresponsive ("未响应") instead of crashing.

### Diagnosis from ReShade.log (21614 lines, 01:19:43 ~ 01:20:01)
- **0x268BAB6D was registered 3 times** (`Registered API-based runtime replacement: 0x268bab6d`) but **never matched by any pipeline** — no `checking 0x268bab6d, found: YES`, no `PipelineShaderDetails(hash: 0x268bab6d`, no `Replacing 0x268bab6d`.
- **Game never completed a single present** — no present/render logs at all.
- **Log abruptly stops at 01:20:01:093** mid pipeline-creation; game hung.
- **Swapchain modification succeeded**: `r8g8b8a8_unorm => r16g16b16a16_float` on D3D12 (1920x1080 primary, 1x1 temp, 1024x768 secondary).
- **DX11 dummy window (EOSOVHDummyWindowClass) correctly skipped**.
- **No ERROR/FATAL/exception** — only WARN (`LoadLibrary deadlock`, `No swapchain desc`).

### Interpretation
- The `SV_POSITION0` signature fix likely resolved the `create_pipeline` crash (no crash this run).
- However, the game hung during render initialization **before** reaching any scene that uses `0x268BAB6D`.
- The hang is **not** caused by the tonemapper replacement (it never triggered).
- Likely cause: swapchain modification (R16G16B16A16_FLOAT upgrade + proxy shader + resource cloning) may be incompatible with DL2's D3D12 render pipeline at this loading stage.

### Next steps
1. Wait longer (1-2 min) to confirm it's a true hang, not slow loading.
2. If persistent, try disabling `swap_chain_proxy_vertex/pixel_shader` or `use_resource_cloning` to isolate.
3. Compare with pre-fix run: previously the game reached the tonemapper scene and crashed at `create_pipeline`; now it hangs earlier. The swapchain config itself is unchanged between runs, so the hang may be timing/state-dependent rather than config-dependent.

## 2026-07-23: test-N black screen diagnosis (create_pipeline in constructor)

### Problem
- Deployed `renodx-dyinglight2.addon64` (commit a5527e0, with SV_POSITION0 fix).
- Game can enter menu, but entering game scene shows black screen and hangs.
- ReShade.log now has 67048 lines (vs 21614 in previous run), running 01:29:53 ~ 01:30:55 (62s).

### Diagnosis from ReShade.log
- **0x268BAB6D was successfully found and entered replacement flow** (unlike previous run):
  - `PipelineShaderDetails(hash: 0x268bab6d, in_inverse_map: no, use_replace_async: no)` (line 21477)
  - `ClonePipelineSubObjects(cloning pixel_shader with 0x268bab6d)` (line 21491)
  - `BuildReplacementPipeline(Added replacement 0x268bab6d)` (line 21498) — this is from PipelineShaderDetails constructor (Path A), NOT from BuildReplacementPipeline function
  - `OnCommandAction(shader hash seen: 0x268bab6d, matched: YES, stage: true)` (line 21707)
- **But NO `Replacing 0x268bab6d`, NO `New pipeline`, NO `Failed to replace` logs** — the create_pipeline call result was invisible because:
  - Constructor (Path A) success log was under `DEBUG_LEVEL_2` (not enabled)
  - Constructor (Path A) failure log was under `DEBUG_LEVEL_0` (not enabled)
  - `BuildReplacementPipeline` function was skipped because `initialized_replacement = true` was set in constructor
- **Swapchain modification succeeded**: `r8g8b8a8_unorm => r16g16b16a16_float`
- **No OnPresent/render logs** — game hung before completing first present after entering scene

### Root cause (initial hypothesis)
`create_pipeline` is called inside `PipelineShaderDetails` constructor (shader.hpp:334-399, Path A) when `!use_replace_async` and shader hash matches `runtime_replacements`. The call either:
1. Returns false (shader signature/layout mismatch) — but failure log was hidden under DEBUG_LEVEL_0
2. Hangs the GPU (driver-level issue)

The SV_POSITION0 signature fix did NOT resolve the underlying create_pipeline issue — it only changed where the crash manifests (from BuildReplacementPipeline function to PipelineShaderDetails constructor).

### Fix applied
Added pre/post `create_pipeline` logging in both code paths (constructor Path A and BuildReplacementPipeline function) under `DEBUG_LEVEL_0 || DEBUG_LEVEL_1`:
- `calling create_pipeline` (before) — to detect hangs (if this appears but no "returned" log, it's a hang)
- `create_pipeline returned: true/false` (after) — to detect failures
- Changed constructor success log from `DEBUG_LEVEL_2` to `DEBUG_LEVEL_0 || DEBUG_LEVEL_1`
- Changed constructor failure log from `DEBUG_LEVEL_0` to `DEBUG_LEVEL_0 || DEBUG_LEVEL_1`
- Removed `assert(built_pipeline_ok)` (meaningless in Release builds)

### Verification status
- [x] Code changes applied to `src/utils/shader.hpp`
- [x] Commit and push (commit a112dd9 — DestroyPipelineSubobjects typo fix)
- [x] GitHub Actions clang build
- [x] Deploy and test — game no longer crashes, shader replacement works
- [x] `create_pipeline` succeeds — replacement pipeline created

## 2026-07-23: slider behavior fix (swap_chain_scaling_nits double-scaling)

### Problem
After `0x268BAB6D` replacement started working, slider behavior was incorrect:
1. **Peak Brightness** acted as a ceiling (normal), but setting it below Game Brightness made the image gray.
2. **Game Brightness** controlled *overall* brightness (both game and UI), as if it were the global scaling factor.
3. **UI Brightness** was *inverted* — turning it up made the scene darker, turning it down made the scene brighter.

### Root cause analysis
The DL2 path has two scaling stages for game content:
1. **Tonemapper replacement**: `RenderIntermediatePass()` applies `intermediate_scaling = DIFFUSE_WHITE / GRAPHICS_WHITE` to game pixels before encoding to intermediate format.
2. **Swapchain proxy**: `SwapChainPass()` applies `swap_chain_scaling_nits` to all pixels (both game and UI) after decoding.

`swap_chain_proxy_pixel_shader.ps_5_x.hlsl` line 10 was set to `RENODX_DIFFUSE_WHITE_NITS`.
This caused:
- **UI pixels**: SDR 1.0 → `* DIFFUSE_WHITE` → UI brightness tied to Game Brightness slider.
- **Game pixels**: SDR 1.0 → `* (DIFFUSE_WHITE / GRAPHICS_WHITE)` (intermediate) → `* DIFFUSE_WHITE` (swapchain) → net `DIFFUSE_WHITE² / GRAPHICS_WHITE`.
  - When `GRAPHICS_WHITE` (UI Brightness) increases, the denominator grows → game gets darker (inverted UI slider).
  - When `DIFFUSE_WHITE` (Game Brightness) increases, both factors grow → everything gets brighter (Game controls global).

### Fix
Changed `config.swap_chain_scaling_nits` from `RENODX_DIFFUSE_WHITE_NITS` to `RENODX_GRAPHICS_WHITE_NITS` in `swap_chain_proxy_pixel_shader.ps_5_x.hlsl`.

After the fix:
- **UI pixels**: SDR 1.0 → `* GRAPHICS_WHITE` → UI brightness correctly controlled by UI Brightness slider.
- **Game pixels**: SDR 1.0 → `* (DIFFUSE_WHITE / GRAPHICS_WHITE)` (intermediate) → `* GRAPHICS_WHITE` (swapchain) → net `DIFFUSE_WHITE` → game brightness correctly controlled by Game Brightness slider.

### Files changed
- `src/games/dyinglight2/swap_chain_proxy_pixel_shader.ps_5_x.hlsl` — line 10: `DIFFUSE_WHITE_NITS` → `GRAPHICS_WHITE_NITS`

### Verification status
- [x] Code change applied
- [ ] Commit and push
- [ ] GitHub Actions clang build
- [ ] Deploy and test sliders

## 2026-07-24: Confirmed main-scene tonemapper candidate

- `0x268BAB6D` was traced with the producer probe and found to follow a LUT/bloom-related resource chain; it is not the best HDR source candidate.
- `0x3E36DA5B` was tested using a reversible live shader replacement that preserved only the source red channel.
- The result was a red 3D scene while UI remained unchanged, proving that this shader owns the main world-color path rather than UI or an isolated post-process.
- Its original assembly contains exposure texture sampling, a rational compression curve, and `saturate`, matching a tonemapper structure.
- The temporary live replacement was unloaded and removed after verification.

Conclusion: use `0x3E36DA5B` as the primary DL2 HDR tonemapper target. A final HDR-preserving replacement test is the remaining validation; do not spend further cycles searching unrelated hashes unless that test fails.

## 2026-07-24: HDR range verification in SDR mode

- A temporary false-color replacement for `0x3E36DA5B` was tested with Windows in SDR mode.
- The probe encoded `t0` peak values as blue/green/yellow/red/white bands, so the monitor output mode did not affect the measurement.
- Outdoor sky produced red and white; indoor scenes were mostly blue while lamps reached green/yellow/red.
- In this mapping, red means `t0` peak above 4.0 and white means above 12.0; ordinary SDR white would remain blue/green.
- The temporary replacement was unloaded and deleted.

Conclusion: `0x3E36DA5B` receives genuine HDR scene values before its original compression curve. The candidate is validated for the formal HDR-preserving replacement; no further shader search is warranted.

## 2026-07-25: Single HDR bridge correction

- The first formal build registered both `0x3E36DA5B` and the older `0x268BAB6D` replacement.
- ReShade confirmed that both replacements matched in the same session.
- Both paths call `RenderIntermediatePass`, causing a second HDR intermediate encoding and white-point scale. This explained the blue debug overlay, ineffective Peak Brightness, Game White flicker, and UI White inversely affecting the scene.
- `0x268BAB6D` is now retained only as a documented later LUT/color-grade reference; it is no longer registered as a runtime replacement.

Next test: run only `0x3E36DA5B` as the HDR bridge. Investigate a separate UI path only if UI behavior remains incorrect after this correction.

## 2026-07-25: Preserve pre-exposure HDR for RenoDX

- With the single bridge active, the range debug changed from red to green after DL2 auto-exposure settled on the sky.
- The original `t0` remains HDR, but the game's `t1` exposure scalar normalizes it before RenoDX receives it.
- Vanilla continues to use the original `t1` path. RenoDX modes now use the pre-exposure scene signal, leaving final exposure, diffuse white, and peak mapping to RenoDX.
