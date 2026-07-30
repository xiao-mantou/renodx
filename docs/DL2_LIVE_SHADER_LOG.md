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
## 2026-07-25: RenoDRT White Clip control
- DL2 raw `t0` values reached roughly 8 or more in the sky, while most indoor lights were in the 1-4 range.
- Added the Advanced-only RenoDRT `White Clip` setting with a default of 10. It controls the input scene value that rolls into Peak Brightness; it is intentionally separate from Game White and Peak.
## 2026-07-25: White Clip response probe
- Added Debug Mode 4, `RenoDRT Output`, to false-color the result of the actual RenoDRT `ToneMapPass` before DL2's later composite passes.
- This is intended to compare White Clip values in a fixed scene. It is a curve-response probe, not a final-display luminance measurement.
## 2026-07-25: Preserve controlled auto exposure
- RenoDX modes now apply a global log-space compression to DL2's `t1` exposure (`0.35` strength, clamped to `0.5..2.0`) instead of fully using or fully discarding it.
- The UI/intermediate reference is fixed at 203 nits in the shader until a true late UI composite pass is identified; the old UI slider was affecting the whole scene because the bridge runs before UI composition.
- Vanilla mode still uses the original full `t1` path.
## 2026-07-25: Exposed controlled exposure parameters
- Added `Auto Exposure Strength` (default 35%) for live tuning of the log-space `t1` retention.
- Added Advanced-only `Auto Exposure Minimum` (default 0.5) and `Auto Exposure Maximum` (default 2.0).
- These controls affect RenoDX modes only; Vanilla keeps the original full exposure path.
## 2026-07-25: SDR-reference HDR upgrade
- Refactored the exact reverse-engineered DL2 curve into `ApplyDL2SDRCurve()`. It retains the original `t1` exposure, rational curve parameters, and final `saturate` limit.
- RenoDRT now receives this exact vanilla SDR result as its graded SDR reference rather than using `NeutralSDR` as both reference inputs.
- HDR input now keeps full DL2 exposure for shadow and midtone scene values, then transitions to protected exposure only above configurable raw-scene luminance thresholds.
- Added Advanced-only `HDR Protection Start` (0.75) and `HDR Protection End` (4.0); the existing exposure retention/min/max controls now apply only to the highlighted protected range.
## 2026-07-25: RenoDRT curve controls
- Replaced the temporary fixed Reinhard method with a runtime `RenoDRT Curve` selector. `Daniele` is the default; Reinhard, Hermite Spline, and Neutwo remain available for comparison.
- Added Advanced-only `RenoDRT Scaling`: Luminance (default), Per Channel, and Max Channel.
- These controls change only the RenoDRT display mapping. The exact DL2 vanilla SDR reference and the adaptive HDR input remain unchanged.
## 2026-07-25: Linearized highlight exposure retention
- Replaced the logarithmic `t1 ^ strength` high-light control with a linear blend from unexposed HDR (`0`) to clamped game exposure (`100`).
- This retains the previous endpoints but makes intermediate slider values visually usable in DL2 instead of appearing nearly binary.
## 2026-07-25: Fractional HDR protection controls
- Set the slider display precision for highlight exposure minimum/maximum and HDR protection start/end to two decimals.
- These settings were already float values in the shader, but the UI's inherited integer display format quantized the slider interaction to whole numbers.
## 2026-07-25: DevKit idle-mode performance repair
- DL2 remained heavily stuttery even after the producer probe itself was made opt-in.
- The remaining cause was DevKit's idle render hooks: pipeline binds plus descriptor-table updates, copies, binds, and push-descriptor updates still built full inspection state on every frame while no snapshot or probe was active.
- DevKit now leaves all of those high-frequency paths idle unless a snapshot is actively capturing or an MCP caller explicitly starts the DL2 producer probe with `devkit_set_dl2_probe_target`.
- The probe is disabled by default and its writer map is cleared when a new target is selected. No snapshot, shader dump, texture readback, resource clone, or bulk dump is involved.
- DevKit now stops its named-pipe MCP session when the last tracked device is destroyed, rather than waiting for DLL process detach. This avoids joining the pipe worker under the loader lock and fixes the game process lingering after exit.
Verification:
- Built DevKit deployment was tested in DL2 with the bridge idle.
- Scene performance recovered to normal.
- Next safe investigation step: explicitly start the producer probe only for the known scene pass `0x3E36DA5B`, wait a few frames, then query its writer status.
### First live probe result and follow-up
- The first live query hit `0x3E36DA5B` thousands of times but resolved no `t0` resource. The D3D12 descriptor cache was not populated from its pre-existing tables.
- That failure exposed a second safety issue: before `t0` was known, the probe still processed all descriptor-table copies and all resource writes. It accumulated tens of millions of entries in seconds and reintroduced severe stutter.
- The probe now ignores global copy, clear, and writer events until the target input resource has actually been resolved. Descriptor binds and pipeline tracking are restricted to the target shader while probing.
- Passing `shaderHash: 0` to `devkit_set_dl2_probe_target` now immediately disables and clears the probe, so future tests do not require a game restart to return to idle mode.
- The descriptor-resolution problem remains separate: the next investigation must add a bounded, target-specific D3D12 descriptor lookup rather than restoring global table-copy tracking.
### Bounded probe verification
- Deployed the bounded DevKit build and confirmed normal scene performance while idle (`active: false`, no probe events).
- With `0x3E36DA5B` / `t0` enabled for three seconds, the shader was hit 274 times and exactly 274 probe events were submitted.
- Crucially, descriptor-table copies remained at zero and no resource writer history was created. The former unbounded path would have created tens of millions of entries in the same interval.
- `t0` was still unresolved (`pixelSrvCount: 0`, `t0ResourceHandle: 0`), so no producer hash can yet be inferred. This is a descriptor-cache coverage issue, not evidence that `0x3E36DA5B` is the wrong scene pass.
- The probe was then stopped through `devkit_set_dl2_probe_target` with `shaderHash: 0`; status confirmed `active: false` and all target/event counters reset.
### Target-table chain result
- The bounded reverse table-chain probe was tested for three seconds against `0x3E36DA5B` / `t0` and then automatically disabled.
- It remained bounded and smooth: 301 target draws, 301 probe events, and zero descriptor copies or resource writer events.
- It reached the 64-table candidate cap without finding a persistent copy destination or a `t0` view. DL2 therefore uses short-lived descriptor-table handles; table identity cannot connect target bindings to earlier copy operations across frames.
- Do not repeat this table-chain test. The next resolver must key candidates by stable descriptor heap plus offset range, with a strict copy-inspection budget, rather than by descriptor-table handle.
### Heap-range and descriptor-seed result
- The heap-range probe confirmed 373 copy operations overlapping the target heap ranges, so the target descriptor path was reached without restoring global writer tracking.
- A startup descriptor-view seed then recorded 93,845 updates and produced 1,023 valid descriptor matches for the target. `t0` still remained empty.
- This rules out missing source-view metadata as the blocker. ReShade's public descriptor events do not preserve the command-list association needed to identify the actual SRV bound to this DL2 draw.
- The startup seed is too expensive to retain during normal play and was removed immediately. Do not add further ReShade-level descriptor cache variants; a future attempt must use a D3D12-native descriptor hook or a different non-DevKit investigation route.
## 2026-07-27: DLSS Frame Generation Present cadence
- With DLSS Super Resolution enabled, the HDR path is stable when Frame Generation is off; high-light flicker returns only when Frame Generation is enabled.
- The bounded 16-Present probe recorded one Streamline HUDLessColor/UI tag serial for exactly two consecutive Present callbacks, with four backbuffers alternating as two pairs. This identifies a real rendered frame followed by a DLSS-generated frame without draw/resource dumping or GPU readback.
- The strongest current hypothesis is that RenoDX runs its final swapchain proxy on both Presents while the scene/proxy source is refreshed only for the rendered frame. Reprocessing the generated frame can therefore cause the alternating high-light result.
- Added a default-off Compatibility A/B, `DLSS FG Skip Generated Proxy`: after a new color tag has been observed on the rendered-frame Present, it arms a one-shot skip of only the immediately following `DrawSwapChainProxy`. Resource upgrades, clone state, game Present handling, and all shader replacements remain active.
- Test only with DLSS Frame Generation enabled. If flicker disappears without a black frame, this confirms the Present-cadence handoff as the next fix target. If it persists unchanged, turn the switch back off and continue at the tagged resource/Frame Generation handoff rather than changing the scene HDR shader.
## 2026-07-27: DLSS Frame Generation color handoff
- A Streamline handoff audit proved that `slSetTagForFrame` accepts the RenoDX FP16 clone when explicitly routed (`result=0`, submitted resource equals the clone).
- A matching one-frame transfer audit proved the normal display path is `0xAD085E81` Gamma FP16 clone -> FP16 intermediate -> FP16 swapchain backbuffer.
- In the same resource lifetime, the DLSS FG tag original/clone was distinct from the Gamma clone, intermediate, and swapchain resources. DLSS FG therefore uses a separate color producer rather than the already proven Gamma-to-Present HDR chain.
- Do not treat the successful FP16 tag route as proof that this separate producer contains HDR scene values. The graphics-writer audit found only UI/mask producers; the remaining target-specific path is compute/UAV.
### FG tag graphics and transfer audit
- The one-frame graphics writer audit found seven pixel shaders writing the tagged surface (`0xF34DDC49`, `0x43B22618`, `0x2280559E`, `0x7D1BA5D4`, `0xEDC2563A`, `0x2BECAD9C`, and `0xC6ADA2E9`). Offline decompilation classified all seven as UI, mask, or primitive compositing; none is a scene HDR/tonemap producer.
- A later transfer capture targeted `original=0x2E40D1CEF90`, `clone=0x2E40D1CF920` and found no direct CopyResource, CopyTexture, or ResolveTexture operation (`count=0`). The immediately following handoff audit reported exactly the same original/clone pair and two Presents, proving this was not a rotating-resource false negative.
- The first compute/UAV audit ended at the next Present with `count=0`. This is not conclusive: DLSS FG alternates real and generated Present calls, and the relevant compute descriptors may have been bound before the one-Present window. The follow-up diagnostic stays bounded but covers three Presents, records the Streamline tag serial range and aggregate compute-UAV descriptor updates/views, and still records a hash only when the exact tag original/clone is bound. It remains default-off and performs no resource readback, mutation, tag routing, or global dispatch tracing.
## 2026-07-27: DLSS Frame Generation HDR10 output contract
- Repeated Streamline tag captures split DLSS Super Resolution (`ScalingInputColor` / `ScalingOutputColor`) from DLSS-G UI assistance (`HUDLessColor` / `UIColorAndAlpha`). The scaling colors use format `0x1A`, which is `R11G11B10_FLOAT`; they are floating HDR resources and are not the missing Backbuffer tag.
- The bundled DLSS-G integration guide states that final color is automatically intercepted through the Streamline swapchain. A Backbuffer tag is only required for a final-color subrect. The HUD-less and UI tags are auxiliary full-size inputs, which explains why their observed graphics writers were UI/mask passes.
- The same guide requires RGB10/HDR10/BT.2100 PQ for DLSS-G HDR and explicitly excludes FP16/scRGB output. DL2 was presenting with `R16G16B16A16_FLOAT` plus `extended_srgb_linear`, matching the unsupported combination and the observed real/generated-frame highlight instability.
- A previous HDR10 attempt changed the swapchain container without changing the final proxy encoding, so scRGB values were written into an HDR10/PQ swapchain. That invalid mixed configuration explains the color explosion/black output and must not be used as evidence against HDR10.
- The HDR10 compatibility change now keeps all working resources/clones in FP16 but uses `SetUseHDR10()` for the final swapchain and `SwapChainPass` HDR10/PQ output preset for the final proxy, including final-output debug probes. Test only with a Windows HDR-enabled HDR10 display and DLSS Frame Generation enabled.
### Follow-up: corrected swapchain module version mismatch
The initial HDR10 commit accidentally called `swapchain::v1::SetUseHDR10()` while
`src/mods/swapchain.hpp` selects v2 by default. The runtime creation callbacks
therefore continued reading v2's default FP16/scRGB state, even though the final
proxy had already been changed to PQ. This created a PQ-to-scRGB encoding mismatch:
the proxy shader outputs BT.2100 PQ values, but they are written into an
`extended_srgb_linear` swapchain, resulting in washed-out, low-contrast visuals.
The DL2 addon now calls `swapchain::SetUseHDR10()` through the active module
alias. This configures v2's `target_format` to `r10g10b10a2_unorm` and its
`target_color_space` to `hdr10_st2084`. The next runtime log must show both
values in OnCreateSwapchain and OnInitEffectRuntime before any visual judgement
of HDR10/PQ output is meaningful.
Verification: next game session log should report:
```text
swap: r8g8b8a8_unorm => r10g10b10a2_unorm
format: r10g10b10a2_unorm
colorspace: hdr10_st2084
```
### Revert: remove SetUseHDR10, keep FP16 swapchain with PQ proxy encoding
The HDR10 container (R10G10B10A2) caused black screen because the game's scene
pipeline still renders SDR [0,1] at early stages. A 10-bit container immediately
clips those values. The correct flow is:
```
Game early passes [0,1] SDR
→ 0x3E36DA5B injects HDR >1.0 (proven by red debug at 6.0+)
→ R8→R16F clones preserve HDR losslessly
→ 0xAD Gamma FP16 output
→ FP16 swapchain backbuffer (linear HDR)
→ Proxy shader: FP16 input → RenoDRT/Peak → PQ encode
→ Windows HDR display
```
Removed `SetUseHDR10()`. The swapchain now stays `r16g16b16a16_float` with
`extended_srgb_linear`, matching DL1 and most RenoDX games. The proxy shader
already uses `SWAP_CHAIN_OUTPUT_PRESET_HDR10` to encode PQ correctly.
This is the standard RenoDX pattern: FP16 intermediate pipeline + final PQ
encoding in the proxy, not a 10-bit container from swapchain creation.
### Critical fix: set HDR10 colorspace metadata on FP16 swapchain
The previous revert kept `target_format = R16G16B16A16_FLOAT` (correct) but
left `target_color_space = extended_srgb_linear` (incorrect). This caused
Windows to interpret the proxy's PQ-encoded output as linear scRGB, resulting
in washed-out, low-contrast visuals identical to the earlier HDR10 mismatch.
**Root cause:**
The proxy shader outputs BT.2100 PQ non-linear values `[0,1]`, but Windows saw
the `extended_srgb_linear` colorspace tag and treated them as linear scRGB.
**Solution:**
Explicitly set `target_color_space = hdr10_st2084` while keeping the FP16
container. The swapchain module's `ChangeColorSpace()` will call
`IDXGISwapChain3::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)`,
signaling Windows that the content is HDR10/PQ.
**This is the correct hybrid configuration for DLSS-G HDR:**
- Container: `R16G16B16A16_FLOAT` (preserves game's HDR pipeline)
- Colorspace metadata: `hdr10_st2084` (tells Windows it's PQ)
- Proxy encoding: `ENCODING_PQ` + `HDR10_PRESET` (already correct)
Next log should show:
```text
format: r16g16b16a16_float
colorspace: hdr10_st2084
```
This matches reports from other games (Metro Exodus, Exodus 33) where DLSS-G
requires PQ colorspace signaling but the underlying container can stay FP16.

## 2026-07-28: Fix FG proxy skip binding for active swapchain module

The DL2 compatibility switch `DLSS FG Skip Generated Proxy` was arming
`swapchain::v1::skip_next_proxy_draw`, while the repository default and the
confirmed runtime path use `swapchain_v2.hpp` (D3D12). Therefore the previous
tests did not exercise the intended skip at all. The switch now targets the
active swapchain alias, and v2 skips only one proxy draw after a new Streamline
color tag, with a one-shot confirmation log. Resource upgrades, clone state,
and Present handling remain unchanged.

The cadence capture then showed two Presents per Streamline tag, with a
different rotating backbuffer on each Present. The next bounded audit expands
those 16 samples with resource format, usage, clone state, and view count so
the real/generated pair can be compared without GPU readback or mutation.

All four rotating backbuffers proved identical (`R10G10B10A2_UNORM`, matching
usage, clone enabled/target, and view count). A separate one-shot barrier audit
now records at most 128 transitions for full-size swapchain resources/clones,
including tag serial and old/new usage state. It does not insert barriers or
modify resources.

The barrier audit confirmed symmetric `PRESENT -> RENDER_TARGET -> PRESENT`
transitions for every sampled backbuffer/clone. The bundled Streamline SDK has
no `colorBuffersHDR` or frame-end callback on `DLSSGOptions`, but it does expose
`colorBufferFormat`, `hudLessBufferFormat`, and `uiBufferFormat`. DL2 now hooks
the cached `slDLSSGSetOptions` function and aligns the declared color buffer to
the actual HDR10 swapchain (`R10G10B10A2_UNORM`); when tagged clone routing is
enabled, the two auxiliary color formats are declared as FP16 as well.
## 2026-07-28: FP16/scRGB restoration after HDR10 colorspace API rejection
**Problem:** cc14071 attempted to set `hdr10_st2084` colorspace on the FP16 swapchain while keeping the proxy's PQ encoding. DXGI rejected this with `E_INVALIDARG` (0x80070057), causing the runtime colorspace to fall back to `unknown`. Windows then interpreted the proxy's PQ-encoded output as linear scRGB, resulting in washed-out, low-contrast visuals identical to the earlier a2e1016 mismatch.
**Root cause:** `IDXGISwapChain3::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)` requires an HDR10-compatible container format (R10G10B10A2 or R16G16B16A16_FLOAT in specific scenarios). The FP16 container with `extended_srgb_linear` is the standard path; forcing `hdr10_st2084` metadata without changing the container is unsupported.
**Cross-game evidence:**
- Space Marine 2 uses `SetUseHDR10()` (R10G10B10A2 container) but its proxy outputs scRGB (no `RENODX_SWAP_CHAIN_ENCODING` override in shared.h), proving HDR10 container + scRGB encoding is valid.
- DL2 attempted FP16 container + PQ colorspace metadata, which DXGI rejects at the API level.
**Solution:** Removed both the `target_color_space = hdr10_st2084` assignment in addon.cpp and the PQ encoding overrides in the proxy shader. The swapchain now uses:
- Container: `R16G16B16A16_FLOAT` (preserves game HDR pipeline)
- Colorspace: `extended_srgb_linear` (default, accepted by DXGI)
- Proxy encoding: scRGB (default from `draw.hlsl BuildConfig()`)
This matches the pre-HDR10-experiment stable state (before 6e92f4a). DLSS Frame Generation flicker remains as the active fix target. HDR10 output (R10G10B10A2 + PQ) is deferred; it requires diagnosing the a2e1016 black-screen root cause separately.
**Commit:** 489c302
**Verification pending:** User will test with DLSS-G enabled to confirm stable visuals and persistent flicker, then continue FG diagnostic.

## 2026-07-28: HDR10 as the runtime-selectable presentation container

**Decision:** NVIDIA's DLSS-G documentation excludes FP16/scRGB for HDR output, so
HDR10 (R10G10B10A2 + BT.2100 PQ) is the only viable target. All FP16/scRGB
scaling experiments (`91c1432` through `aeba2b0`) are abandoned.

**Static review of the a2e1016 black screen (evidence, not speculation):**
- `swapchain_v2.hpp:146` `RenderPass` derives RTVs from the resource's own format,
  and `render.hpp:516-533` auto-derives pipeline render-target formats from those
  views. An RGB10 backbuffer therefore cannot produce an invalid RTV or a
  pipeline format mismatch. That rules out the most obvious black-screen cause.
- `resource.hpp:71-82` `VIEW_UPGRADES_R10G10B10A2_UNORM` covers every R8/B8/RGB10
  source format, so view upgrades are not missing entries either.
- `swapchain_v2.hpp:74-81` pins the backbuffer clone to FP16 regardless of the
  container, so scene resources keep full HDR headroom under HDR10.
- `draw.hpp:163-171`: with `swapchain_proxy_compatibility_mode = false`, a missing
  clone makes `SwapchainProxyPass::Render` return false, which destroys the pass
  and presents nothing. This is the one remaining code path that produces exactly
  a black screen, and it is only observable at runtime.
- Correction to the previous entry: the claim that Space Marine 2 proves
  "HDR10 container + scRGB encoding" is wrong. `spacemarine2/addon.cpp:320` has
  `SetUseHDR10()` commented out; that game runs FP16 + scRGB.
- Correction to the `5ce5061` entry: a 10-bit container cannot clip the game's
  early SDR stages, because it only applies to the final backbuffer.

**Change:** `SwapChainFormat` is now a global Compatibility setting read once in
DllMain (a container cannot change after swapchain creation). HDR10 is the
default. The proxy shader no longer hardcodes its encoding; it reads
`RENODX_SWAP_CHAIN_ENCODING` and `RENODX_SWAP_CHAIN_OUTPUT_PRESET` from the
injection buffer, so the shader can never disagree with the container DXGI
created. `swap_chain_scaling_nits` returns to `RENODX_GRAPHICS_WHITE_NITS` (203),
dropping the temporary 150 test value.

A one-shot, first-Present-only log reports the final presentation state:
backbuffer handle/format/size, clone handle, `clone_enabled`, `clone_target`,
clone format, and the active format setting. It reads descriptors only, with no
readback, dump, or redirection.

**Next log must be read for:**
```text
DL2 swapchain format: HDR10 (RGB10+PQ)
swap: r8g8b8a8_unorm => r10g10b10a2_unorm
colorspace: hdr10_st2084
DL2 present state(... format=r10g10b10a2_unorm, clone=0x..., clone_enabled=1 ...)
```
If `clone=0x0` or `clone_enabled=0`, the black screen is the `draw.hpp:163` abort
and the fix belongs in clone activation for an RGB10 backbuffer. If the clone is
valid and the screen is still black, the cause is downstream of the proxy draw
 (Streamline swapchain interception), not the container.

The DLSS-G guide also requires Hudless to use the same color space and
post-processing as the final color backbuffer. DL2's captured HUD/UI tags are
pre-proxy resources while the final output is HDR10/PQ, so an optional
`DLSS FG Suppress Pre-PQ Color Tags` switch now omits those auxiliary tags and
lets Streamline use its automatically intercepted final color. This is an A/B
compatibility test and may reduce UI reconstruction quality.

## 2026-07-30: DLSS Off/Balanced color-path isolation

**Symptom:** With RenoDX enabled, DLSS Off looked generally flatter while DLSS
Balanced looked more saturated. Without the addon, the two DLSS modes were much
closer. The goal was to separate a normal DLSS reconstruction difference from a
RenoDX resource-path mutation.

**Evidence:** The original `slDLSSSetOptions` call was captured without
modification: Balanced used `colorBuffersHDR=1`, `useAutoExposure=0`,
`preExposure=1`, and `exposureScale=1`. This ruled out a DLSS HDR/exposure
option mismatch. The custom 0x3E/0x268/0xAD shader replacements and final proxy
gamut compression were also disabled during isolation.

The following A/B results identified the resource rule:

- `8bf0c15`: all three scene FP16 upgrades disabled; Off and Balanced became
  broadly consistent, though this build intentionally sacrificed highlight
  preservation.
- `4f52c4b`: restoring only `R8G8B8A8_TYPELESS + BACK_BUFFER` reproduced the
  systematic Off/Balanced color divergence.
- `bc6f891`: disabling that typeless rule and restoring only
  `R8G8B8A8_UNORM + ANY` kept the two modes consistent in the user's scene.

**Conclusion:** The typeless back-buffer upgrade incorrectly matches a native
DLSS-Off color path and changes its interpretation relative to the DLSS SR
path. It is omitted from the final configuration. The general UNORM upgrade is
retained as the current range-preservation candidate; the sRGB upgrade remains
omitted because it was not required for this fix and can alter the SDR
composite. This establishes relative color-path consistency, but absolute
matching against vanilla should still be checked after the final cleanup build.

**Absolute-color follow-up:** Main-menu comparison showed that the common
proxy output was still more saturated than vanilla even with all scene upgrades
disabled. A six-way grid compared Linear/sRGB/Gamma-2.2 decoding under BT.709
and BT.2020 source assumptions. Only Linear + BT.709 matched the vanilla
screenshot. `BuildConfig()` had inherited `swap_chain_decoding` from
`RENODX_INTERMEDIATE_ENCODING`, which defaults to sRGB for the current injected
gamma setting. The proxy therefore decoded an already-linear composite a second
time. The DL2 proxy now explicitly selects `ENCODING_NONE` and BT.709; the
existing HDR10-source override still selects PQ + BT.2020 when required.

**Range follow-up:** With Linear + BT.709 fixed, the measured peak was still
limited to 203 nits. The immediate cause was that color-path isolation commit
`4cd80d7` disabled the primary `0x3E36DA5B` HDR bridge and it had not yet been
restored. Native 0x3E applies its SDR curve and `saturate`, so wider FP16
resources alone cannot create values above 1.0. The proven single 0x3E bridge
is restored; 0x268/0xAD remain native to avoid double intermediate encoding.
The sRGB-to-FP16 rule is also retained because earlier `d5954eb` showed that
removing it clips the bridge output later in the chain. The typeless rule was
still omitted at this stage pending direct localization of the remaining cap.

**203-nit clamp localization:** With the bridge writing Linear BT.709, both the
raw and encoded 0x3E test ladders (`0.25, 1, 4, 16`) still measured no higher
than 203 nits. The targeted color-path audit then showed the exact loss:
0x3E writes format 27 (`R8G8B8A8_TYPELESS`) with `clone=0`; 0x268 reads and
writes the same format with `clone=0`; 0xAD reads it before finally writing an
RGB10 resource whose FP16 clone is active. HDR values are therefore clipped at
the first post-0x3E target, before reaching the valid final clone. The typeless
upgrade is restored to preserve this proven HDR chain. Its earlier color A/B
was confounded by the then-unfixed proxy sRGB double-decode, so it must be
revalidated with the corrected Linear BT.709 bridge and proxy.

Focused freeze is not a classic deadlock: tag serials, Presents, and symmetric
backbuffer barriers continue while the visible frame is stale. A separate
`DLSS FG Bypass All RenoDX Proxy` A/B now skips only the final proxy draw on
every Present after the first. Its output color is intentionally invalid; the
test asks only whether focused motion resumes. This distinguishes proxy/
Streamline backbuffer contention from a failure inside Streamline's HDR10
swapchain interception.

**Post-clone shader clamp:** A second targeted audit after restoring the
typeless upgrade showed the complete active chain using FP16 clones: 0x3E
writes `27=>10`, 0x268 reads/writes `27=>10`, and 0xAD reads that clone before
writing `24=>10`. The persistent 203-nit result is therefore not a resource
format clamp. The remaining native 0x268 pass is an SDR 3D-LUT/color-grade
pass: its input coordinates are built for the `[0,1]` SDR domain and its later
math explicitly uses `saturate`. It necessarily collapses the 0x3E raw ladder
values 4 and 16 back to SDR white. The existing 0x268 HDR LUT bridge is now
enabled, corrected to accept and emit the proven Linear BT.709 intermediate
directly (no legacy `InvertIntermediatePass`/`RenderIntermediatePass` sRGB
round-trip). The native 0xAD Gamma pass remains in place because its per-channel
power operation has no explicit upper-range clamp.

**Game Brightness unit correction:** DL2's final Linear BT.709 proxy uses the
fixed bridge unit `1.0 = 203 nits`. Generic `ToneMapPass` returns a value
relative to the selected Game Brightness and sets its curve ceiling to
`Peak / Game`. Feeding that result directly to the fixed-203 proxy made the
Game slider behave as an inverse curve control: lowering Game increased the
available ratio and brightened the mid/high range, while raising it compressed
and desaturated that range. HDR scene output is now multiplied by
`Game / 203` immediately after `ToneMapPass`. This preserves an absolute Peak
because `(Peak / Game) * (Game / 203) * 203 = Peak`, while making Game control
the physical diffuse-white level as labeled. Vanilla output and later UI
composition are intentionally not scaled.

**Popup/UI candidates:** Two post-Gamma captures separated the in-game exit
popup from the main-menu exit popup. The in-game popup exposed three full-size
pixel shaders (`0x54F3F767`, `0xF34DDC49`, and `0x43B22618`) whose dumped math
is characteristic of UI: texture times vertex color, texture/mask modulation,
and fixed RGB with texture-derived alpha. They write a full-size typeless R8
target through its FP16 clone. The main-menu capture was exhausted by sixteen
lower-resolution material/compute passes before its popup draw. The diagnostic
now filters that work, retains known UI and full-size pixel composites, logs
original/effective SRV and RTV *view* formats, and logs the known shaders'
pipeline blend factors. This distinguishes an sRGB-view semantic loss during
FP16 cloning from an incorrect UI-white scale before any rendering mutation.

**Popup/UI correction:** The enhanced capture confirmed the shared typeless R8
target is intentionally written through different views: scene/LUT passes use
an sRGB RTV (`view=29`), while six popup/button shaders use an UNORM RTV
(`view=28`). Both become a linear FP16 view (`=>10`) after cloning. The UI
pipelines use conventional source-alpha/one-minus-source-alpha blending and
their dumped math produces gamma-domain UI colors. Treating those stored gamma
values as linear explains the washed-out gray UI and button color banding.
The six confirmed UI shaders now decode their final RGB from sRGB to Linear
BT.709, multiply it by `UI Brightness / 203`, and preserve alpha verbatim.
The common proxy remains fixed at `1.0 = 203 nits`; therefore Game and UI white
are finally independent. Fullscreen copy/blur shaders remain native.

**DLSS color regression follow-up:** After the targeted `0x3E` typeless target
was promoted to FP16 to preserve HDR range, DLSS Off again appeared flatter
than Balanced. Both modes still traverse the same `0x3E -> 0x268 -> 0xAD`
chain with all three outputs cloned to FP16. The bounded color-path audit now
also reports original/effective SRV and RTV view formats so the next Off versus
Balanced capture can test whether UNORM/sRGB view semantics are being erased by
the FP16 clone. This is diagnostic-only and does not change rendering.

The view-format capture ruled that hypothesis out: both Off and Balanced use
the same `0x3E` input view (`26`) and the same sRGB intermediate views
(`29 => 10`). Their five curve constants are also identical. The source-writer
audit found many pixel writers for Off but no pixel/copy/resolve writer for
Balanced, identifying the latter as a compute/UAV-produced input. The bounded
writer audit now also tracks compute UAVs for both the `0x3E` scene source and
its 1x1 exposure texture. This remains metadata-only and is intended to locate
the first actual upstream divergence without changing the working HDR chain.

**Regression boundary and targeted-clone correction:** Historical binaries
located the visible Off/Balanced split at `bb5a1e1`: `787fe68` was consistent,
while both `bb5a1e1` and the later `5ebc218` were not. The split remains with
the current Tone Mapper set to Vanilla and also remains in the legacy
`RenderIntermediatePass` A/B, ruling out RenoDRT, the later HDR-aware LUT, and
the removed intermediate encode as primary causes. `bb5a1e1` restored the
always-enabled full-size typeless/sRGB clone rules at the same time it restored
the HDR bridge. Those rules are now hot-swap candidates: their FP16 clones are
activated only when the confirmed `0x3E` or `0x268` HDR shader actually writes
the bound RTV, followed by the framework's standard descriptor flush and RTV
rewrite. The general UNORM rule remains unchanged because the earlier isolated
A/B found it color-consistent. This preserves HDR headroom on the known chain
without changing unrelated same-format resources before their ownership is
known.
