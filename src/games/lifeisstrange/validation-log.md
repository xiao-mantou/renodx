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
- The former startup-only `LifeIsStrange_AB_Disable06A2Replacement` key has been replaced by the always-visible segmented `LifeIsStrange_06A2ShaderSource` selector in the RenoDX UI. `0=RenoDX replacement` (default), `1=Native game shader`; it changes only the 06A2 replacement decision.
- `LifeIsStrange_AB_DisableIntermediateUpgrade=1` removes the dedicated FP16 intermediate rules and automatically disables the proxy for a valid vanilla-path comparison.
- `LifeIsStrange_IntermediateBindingDiagnostic=1` enables the 512/FC2A binding diagnostics and SRV clone rebinding. It defaults to `0` so normal runs do not rewrite descriptor bindings.
- `LifeIsStrange_ForceProxyWhite`, `LifeIsStrange_Force06A2White`, `LifeIsStrange_ForceFC2AWhite`, and `LifeIsStrange_Force512White` default to `0`; they are probes only and should not be used for normal gameplay.

## Main-menu sun capture (2026-09-28, DevKit only)

- Snapshot: D3D9, 1920x1080 B8G8R8A8_UNORM swapchain; no RenoDX replacement or resource upgrade was active.
- Draw 235 (`0x06A2A81D`) binds the 1920x1080 FP16 scene view `0x370D7060` at sampler s0. The original PS3 disassembly samples `SceneColorTexture` at s0. DevKit's `usedByActiveShader=false` metadata for this binding conflicts with the disassembly; do not use that flag alone to infer D3D9 sampler usage.
- Live readback of that FP16 view measured max RGB R=26.609, G=17.984, B=14.703, max luminance=19.581, and 31,147 pixels with an RGB channel above 1.0. This is a live-resource read, not a frozen snapshot copy. EXR: `E:\SteamLibrary\steamapps\common\Life Is Strange\Binaries\Win32\renodx-dev\dump\20260928-mainmenu-sun-scene-fp16.exr`.
- The original `0x06A2A81D` disassembly ends with `mad_sat oC0.xyz`; draw 235 writes to B8 resource `0x370D9A00`. This is the first confirmed HDR-to-SDR clamp in the captured native chain.
- Draw 236 (`0x51229A9B`) samples `0x370D9A00` and writes B8 resource `0x370D9320`. Draws 237/238 then write overlays back to `0x370D9A00`; draw 239 runs `0x51229A9B` again, samples that updated resource, and rewrites `0x370D9320`. Draw 240 (`0xFC2A0632`) samples `0x370D9320` and writes the B8 swapchain.
- Live readback after these draws showed `0x370D9A00` max luminance=0.998584 and `0x370D9320` max luminance=0.998584; neither contained RGB values above 1.0. These readbacks are current resource contents, not frozen per-draw copies.

## Stage 1: force-white chain readback (2026-09-28, main menu)

- RenoDX build `lifeisstrange-2c3d0e20d0cc17e61f1221080cd40b88bdd382e9`; DevKit and RenoDX loaded together, DX11 proxy off. Runtime settings from `ReShade.log`: `Force06A2White=1`, `Bypass06A2LUT=1`, `SceneExposure=1`, both A/B disables off.
- D3D9 snapshot: 1920x1080, B8G8R8A8_UNORM swapchain, 252 draws. Draw 239 is `0x06A2A81D` (shader source `Add-on`), draw 240 is `0x51229A9B`, and draw 244 is `0xFC2A0632`.
- Same-snapshot bindings prove the chain: scene FP16 SRV `0x36E9B7E0` -> 06A2 B8 resource `0x36E9BA40` / FP16 clone `0x34C8BAA0` -> 512 FP16 output clone `0x34C8BBE0` -> FC2A SRV `0x34C8BBE0` -> B8 swapchain.
- Live readback of the scene FP16 input (not an exact frozen snapshot) measured channel maxima R=30.0625, G=20.265625, B=16.5625; 26,152 pixels had an RGB channel >1, 11,736 >2, and 2,739 >4.
- With the force-white probe active, the 06A2 FP16 clone readback confirmed `usedClone=true`: every pixel had an RGB channel >1 and >2; maxima were R=4.07421875, G=4.07421875, B=4.078125. The 512 FP16 clone also read back with `usedClone=true`, all pixels >1 and >2, and matching RGB maxima/means. This rules out clipping in the 06A2-to-512 FP16 path for this probe; it does not establish the unforced scene output or final HDR presentation.
- Ignore the scene input analyzer's `luminance.max=222.081075` for this capture: it contradicts the RGB maxima, while the checked analyzer formula is a weighted RGB sum. Channel statistics and threshold counts are the usable results.
- In that build, `LifeIsStrange_SceneExposure` had no HLSL consumer, so `SceneExposure=1` had no rendering effect.

## Stage 1B: unforced 06A2 with LUT bypassed (2026-09-28, main menu)

- RenoDX build `lifeisstrange-2c3d0e20d0cc17e61f1221080cd40b88bdd382e9`; runtime settings: `Force06A2White=0`, `Bypass06A2LUT=1`, proxy off, RenoDX readback callback disabled. 1920x1080 D3D9 snapshot: 255 draws.
- Draw 242 (`0x06A2A81D`, shader source `Add-on`) reads scene FP16 `0x32EA3780` and writes the 06A2 FP16 clone `0x36D7B820`; draw 243 (`0x51229A9B`) consumes `0x36D7B820` and writes FP16 clone `0x36D7C040`; draw 247 (`0xFC2A0632`) consumes `0x36D7C040` and writes the B8 swapchain.
- One-shot live readbacks (not exact frozen snapshot copies), all with confirmed `usedClone` where applicable: scene input maxima R=31.28125/G=21.109375/B=17.21875, with 26,435 pixels >1, 12,455 >2, and 3,382 >4. 06A2 output clone maxima R=1.16015625/G=1.1064453125/B=1.0947265625, with 4,285 pixels >1 and none >2. 512 output clone had the same RGB maxima, 4,231 pixels >1, and none >2.
- The large range reduction is already present at the 06A2 output while LUT bypass is enabled; the 512 FP16 pass retains nearly the same range. The 06A2 shader's pre-LUT processing is now the next target to isolate. HDR10 presentation remains untested with proxy disabled.
- EXRs: `20260928-mainmenu-stage1b-scene-input-fp16.exr`, `20260928-mainmenu-stage1b-06a2-clone-fp16.exr`, and `20260928-mainmenu-stage1b-512-clone-fp16.exr` under the game `renodx-dev/dump` folder.

## 06A2 max-channel LUT bridge (2026-09-29)

- Historical implementation note: this pre-curve scale/three-argument `ToneMapPass` experiment is superseded by the 2026-09-30 bridge below; do not treat it as the current shader path.
- The Stage 1B `1.16` peak came from the LUT-bypass value after `ImageAdjustments2`; it is not the scene-input peak. The captured pre-shaper FP16 scene reaches RGB maxima above `31`.
- The tested `939a28d3` source did not match the previous note: it computed N2 after `ImageAdjustments2`, restored only the already-compressed LUT result, and never called `ToneMapPass`. `addon.cpp` also forced `ToneMapType=0`, so changing the UI/INI type could not affect rendering.
- That revision captured the scene after bloom/compositing and before `ImageAdjustments2`, compressed it for the native curve/LUT, then used the pre-LUT value as `neutral_sdr` and decoded LUT output as `graded_sdr` in three-argument `ToneMapPass`. Vanilla/0 retained the original SDR LUT path and final saturation.
- Runtime HDR luminance and visual matching still require a new build and in-game test.

## ImageAdjustments2 audit (2026-09-29)

- `ReShade.log` confirms build `2b24f5f08dc01f5b02c33380fb754c80a8e05c07`, `ToneMapType=3` (RenoDRT), HDR10 output, and DX11 proxy enabled. LUT bypass, white probes, and RenoDX readback are disabled. The reported ~580-nit peak and overexposed appearance are user-observed, not a DevKit/readback measurement.
- In `postprocess_0x06A2A81D.ps_3_0.hlsl`, `ImageAdjustments2` is c8. The shader computes `r1.xyz = r0.zwy * c8.y + c8.x`, takes reciprocals, multiplies them into the matching color registers, then immediately `saturate`s before packed `ColorGradingLUT` sampling. The per-channel curve is `f(C) = C / (c8.x + c8.y * C)`; `saturate` is the following hard `[0,1]` LUT-domain clip.
- The retained DevKit files contain shader CSOs and scene/output EXRs, but no draw constant snapshot. Stage 1B's `1.16` peak is evidence that the 06A2 pre-LUT path strongly reduced the scene range: that build's LUT-bypass branch forwarded the value captured immediately after `ImageAdjustments2` and before LUT saturation. It was still a final resource readback with later vignette/grain, not an isolated same-pixel curve sample, so it cannot recover `c8.x/c8.y` or the exact curve knee/asymptote.
- The superseded pre-curve branch captured scene color before `ImageAdjustments2` and fed it to three-argument `ToneMapPass`; the native curve remained only in the LUT reference path. The user-reported 580-nit/overexposed result does not prove that `ImageAdjustments2` alone caused it.
- The previous HDR iteration mixed an undecoded pre-LUT reference with an sRGB-decoded LUT result; that branch is superseded. The current transport-only bridge and its provisional LUT-domain assumption are recorded below.
- At audit time, DevKit MCP was unavailable and no live draw-constant capture had been made; the uncertainty below is superseded by the confirmed trace.

## Live ImageAdjustments2 snapshot (2026-09-29 18:04 +08:00)

- Captured one D3D9 main-menu frame with DevKit only: 1920x1080, B8G8R8A8_UNORM swapchain, 243 draws. No RenoDX addon, proxy, clone, or resource readback was used.
- Draw 230 (`0x06A2A81D`) reads scene FP16 view/resource `0x348772A0` at s0 and LUT `0x34878F80` (256x64 B8G8R8A8_UNORM) at s4; it writes `0x34878C00`, a 1920x1080 B8G8R8A8_UNORM render target. The shader declares `ImageAdjustments2` at c8, but the snapshot reports `constantCount=0` and no constant buffers, so c8 values were not captured.
- Draw 234 (`0x51229A9B`) samples `0x34878C00` and writes B8 resource `0x348798C0`. Draw 235 (`0xFC2A0632`) samples `0x348798C0` and writes the B8 swapchain. This same-frame binding evidence confirms `scene FP16 -> 06A2/B8 -> 512/B8 -> FC2A -> swap` for this menu frame.
- `Trace With Snapshot` was off for this 18:04 capture, so it produced no push-constant trace. At that time the formatter also omitted `first`, preventing exact c-register attribution; see the confirmed trace below.
- This vanilla DevKit-only snapshot confirms the native B8/saturate bottleneck but does not measure the current RenoDX HDR branch or explain its reported ~580-nit output.

## Confirmed ImageAdjustments2 constants (2026-09-29 21:30 +08:00)

- One D3D9 snapshot with DevKit build `873b59af` captured 248 draws. Target pixel shader `0x06A2A81D` is draw #235; no resource or pixel readback was performed.
- The adjacent pixel-stage `push_constants` entry is `first: 32, count: 4`, raw `0x3e5b4027, 0x3f7925fe, 0x3e82c9f9, 0x3f800000`. `first` advances in 32-bit scalar slots, so 32 maps to float4 register c8; the shader disassembly names `ImageAdjustments2` at c8. Attribution is confirmed.
- Captured c8 is `(0.21411191, 0.97323596, 0.25544718, 1.0)`. The shader uses x/y for `f(C)=C/(c8.x+c8.y*C)`, then applies `saturate` before LUT sampling. For this frame the curve asymptote is about 1.0275 and reaches the LUT clip at input C about 8.0. Parameters may vary by frame/scene.
- The 18:35 trace omitted `first`; its previously guessed values are unassigned and must not be treated as c8. This trace supersedes that uncertainty but does not explain the current RenoDX HDR luminance by itself.
- `SnapshotTraceWithSnapshot` was enabled for this single capture; turn it back off (`=0`).

## ImageAdjustments2 constant repeatability check (2026-10-09)

- The initial evidence contained one valid c8 sample: `first=32, count=4`, pixel-stage push constants for draw #235 (`0x06A2A81D`), raw `0x3e5b4027, 0x3f7925fe, 0x3e82c9f9, 0x3f800000`; therefore `a=c8.x=0.21411191` and `b=c8.y=0.97323596` were confirmed for that captured frame. Three additional same-scene captures are now recorded below. The earlier 18:35 trace has no `first` and is not a comparable sample.
- Same-scene repeatability is now confirmed across the original plus three new snapshots: all four raw c8 DWORDs match bit-for-bit. Cross-scene stability remains unverified. For any further sample, confirm the pipeline is pixel shader `0x06A2A81D`, match the nearby `push_constants` event to the draw, and record `first`, `count`, all four raw DWORDs, and decoded c8 values.
- Minimum runtime setup: ReShade host plus the DevKit addon only. Disable the Life Is Strange RenoDX addon and DX11 proxy. No resource clone, pixel readback, or `Trace Constant Buffers` capture is needed; the target values are emitted by the one-frame push-constant trace.
- Enable `Trace With Snapshot` (`SnapshotTraceWithSnapshot=1`) and queue via the DevKit panel's `Snapshot` menu or MCP `devkit_queue_snapshot`. These normal snapshot paths schedule a verbose whole-frame API trace containing `push_constants` events. F8 uses the auto-export path and explicitly skips trace scheduling, so F8 alone is insufficient for this test.
- Turn `Trace With Snapshot` back to `0` after each capture. The trace runs for one frame and can add log volume and frame overhead; capture one frame at a time and pause between samples. `Trace Constant Buffers` remains off.
- Results table:

| Sample | Scene / draw | `first/count` | Raw c8 DWORDs | `a` / `b` | Status |
| --- | --- | --- | --- | --- | --- |
| Existing | Main menu, draw #235 | `32/4` | `3e5b4027, 3f7925fe, 3e82c9f9, 3f800000` | `0.21411191 / 0.97323596` | Confirmed single frame |
| Repeat 1 | Current user-selected scene, 2026-10-09 19:26:27.752 | `32/4`, pixel stage, layout `0x0d34adb8[5]` | `3e5b4027, 3f7925fe, 3e82c9f9, 3f800000` | `0.21411191 / 0.97323596` | Bit-identical to existing sample |
| Repeat 2 | Same running scene, 2026-10-09 19:28:08.603 | `32/4`, pixel stage, layout `0x0d34adb8[5]` | `3e5b4027, 3f7925fe, 3e82c9f9, 3f800000` | `0.21411191 / 0.97323596` | Bit-identical |
| Repeat 3 | Same running scene, 2026-10-09 19:30:21.159 | `32/4`, pixel stage, layout `0x0d34adb8[5]` | `3e5b4027, 3f7925fe, 3e82c9f9, 3f800000` | `0.21411191 / 0.97323596` | Bit-identical |
| Other scene | Pending | Pending | Pending | Pending | Optional |
- Each repeat matched a `bind_pipeline` event for pixel shader `0x06A2A81D`; the immediately following push-constant event was `first=32/count=4`, so attribution to c8 is direct. The entire c8 vector, not just `a/b`, remained bit-identical across these three captures.
- This establishes stability across three captures over about four minutes in the same running scene only. It does not establish that the game never changes c8 across levels, cutscenes, exposure/settings changes, or other rendering variants.
- Trace was queued for exactly these three repeat captures. `SnapshotTraceWithSnapshot` is still enabled in the live game configuration; turn it off in the DevKit overlay now to avoid tracing later unrelated snapshots.

## HDR bridge baseline and clip audit (2026-09-30)

- The clean semantic baseline is commit `b3b91149` (`fix(lifeisstrange): align 06a2 sm3 semantics`), checked against the retained original `0x06A2A81D.ps_3_0.msasm`. The nine SM3 swizzle, write-mask, interpolation, and constant corrections stay intact; the current HDR experiment is an injection on top of that baseline, not a replacement with raw decompiler output.
- HDR-only bridge: apply the temporary tangent extension at pivot `0.18` to the pre-curve scene signal; provisionally decode the curve output as sRGB-shaped, apply max-channel N2 scaling for the LUT proxy, preserve the original packed 2D LUT sampling, decode the LUT result, divide by that scale exactly once, then sRGB-encode the intermediate for following game passes. The LUT transfer assumption and pivot remain experimental.
- This transport test deliberately does not call `ToneMapPass`; adding RenoDX scene tone mapping is a separate step after confirming unbounded transport. Do not combine manual N2 restoration with the three-argument `ToneMapPass(untonemapped, graded_sdr, neutral_sdr)` grade/reference path.
- Vanilla/SDR keeps the baseline `mul_sat` LUT-domain behavior and final `mad_sat` output clamp. HDR uses `saturate` only on the bounded LUT proxy; the final scene output is not saturated.
- Before the 2026-09-30 source correction, an HDR swapchain preset activated the unclipped 06A2 bridge, but `isolate_06a2_shader=true` removed FC2A from the normal replacement set. The intended FC2A `RenderIntermediatePass` therefore did not run while swapchain decoding was forced to `None`. This is a code-confirmed encoding mismatch candidate, not proof of the reported overexposure cause.
- Clip audit: the `0x51229A9B` MSASM has no saturate; `0xFC2A0632` and `RenderIntermediatePass` have no `[0,1]` color clamp. HDR10 `SwapChainPass` PQ-encodes and applies configured peak scaling, not an SDR `[0,1]` hard clip. The intermediate FP16 upgrade/proxy path was independently confirmed by the earlier force-white tests; this shader change still needs build and runtime verification.

## Proxy and intermediate controls (2026-09-30, build and runtime verified)

- `LifeIsStrange_EnableHDRPipeline` controls the 06A2 LUT bridge and, by default, the FC2A intermediate conversion; `LifeIsStrange_EnableFC2AReplacement` can override FC2A replacement. It defaults to `1`. `LifeIsStrange_EnableDX11Proxy` controls only the final proxy/swapchain output in normal mode.
- The SM3 intermediate scale now derives from Game White / UI White instead of reading an unassigned injection slot. With the DX11 proxy enabled, Auto selects the matched sRGB intermediate-encode/swapchain-decode pair; with proxy off and HDR pipeline on, Auto retains the previous `None` selection.
- Build `lifeisstrange-be97881ceabb573d58739382f328dfe5173b529f` loads and its diagnostic overrides are active. The earlier overall overexposure cause remains open; the UI leakage comparison below narrows one affected path to FC2A replacement/intermediate handling.

## UI image whitening isolation (2026-09-30, runtime diagnostics)

- User test: UI images were washed out with `LifeIsStrange_EnableHDRPipeline=1` and returned to normal with it set to `0`. With `LifeIsStrange_AB_Disable06A2Replacement=1`, the intro/logo and save-select translucency improved, but UI images remained washed out; text was unaffected.
- `EnableHDRPipeline=0` is not a single-variable test: it disables the 06A2 HDR bridge, removes the FC2A replacement by default, and changes both intermediate and swapchain decoding from `None` to sRGB. The UI-image cause is therefore not isolated yet.
- Added startup-only diagnostic overrides: `LifeIsStrange_EnableFC2AReplacement=-1` (`-1=Auto`, `0=off`, `1=on`), `LifeIsStrange_IntermediateDecoding=0`, and `LifeIsStrange_SwapChainDecoding=0`. Selection values are `0=Auto`, `1=None`, `2=SRGB`, `3=2.2`, `4=2.4`; Auto selects sRGB for both when the DX11 proxy is enabled, and otherwise preserves the previous pipeline-dependent selection. Missing keys retain these defaults.
- Important naming detail: `LifeIsStrange_IntermediateDecoding` maps to the existing setting whose UI label is "Intermediate Encoding". Value `2` selects sRGB encoding in `RenderIntermediatePass`; it does not decode the source. `LifeIsStrange_SwapChainDecoding=2` selects sRGB decoding in `SwapChainPass`.
- User-reported unmatched tests: FC2A replacement off with Auto/None swapchain handling becomes broadly white; intermediate sRGB encoding with swapchain still Auto/None looks the same; swapchain sRGB decoding with the intermediate still linear makes the whole image too dark. These settings deliberately mismatch the source and destination transfer functions, so the visual result is not a valid quality comparison.
- The three outcomes are consistent with a linear FP16 intermediate on the default HDR path: native FC2A or an sRGB-encoded intermediate must not be fed to a proxy that assumes linear, while sRGB-decoding the default linear intermediate double-decodes it and darkens the image. These unmatched combinations do not identify the UI-whitening root cause.
- Valid matched test: `EnableHDRPipeline=1`, `AB_Disable06A2Replacement=0`, `EnableFC2AReplacement=0`, `IntermediateDecoding=0`, `SwapChainDecoding=2`, proxy HDR10, all force-white probes off. The latest `ReShade.log1` confirms build `lifeisstrange-be97881ceabb573d58739382f328dfe5173b529f`, these exact settings, and the user reports normal UI images with only very slight leakage.
- Second matched test: `EnableHDRPipeline=1`, `AB_Disable06A2Replacement=0`, `EnableFC2AReplacement=1`, `IntermediateDecoding=2`, `SwapChainDecoding=2`, proxy HDR10, all force-white probes off. The latest `ReShade.log1` confirms build `lifeisstrange-be97881ceabb573d58739382f328dfe5173b529f` and these settings; the user reports the same normal UI and very slight leakage, with HDR highlight brightness retained.
- Both successful cases feed sRGB-shaped values to an sRGB-decoding proxy, with FC2A replacement respectively off and on. This narrows the visual problem to the default linear/`None` handoff or an interaction specific to it; it does not prove FC2A shader math is defective. The matched sRGB pair is promoted to the DX11-proxy Auto default based on visual and highlight validation.
- Earlier comparisons used the startup-only `LifeIsStrange_AB_Disable06A2Replacement` key; it is obsolete. The runtime selector now permits same-session comparison while keeping intermediate upgrades, later passes, and proxy settings unchanged. The Native option still runs the game's original 06A2 shader, including its native LUT clamp.

## HDR curve pivot and shared-scale A/B (2026-10-01)

- `HDRCurvePivot` is now limited to `0.18–2.00`; the user reports `0.7` looks acceptable. It affects only the 06A2 HDR tangent-extension pivot; vanilla/SDR, LUT compression/reconstruction, output nits, and ToneMapPass are unchanged.
- Added `HDRCurveMethod` for an in-game A/B: `0` preserves the existing per-channel tangent extension; `1` calculates the brightest input channel's tangent-vs-vanilla curve ratio and applies that single gain to the original curve RGB. This tests whether independent channel branches cause the tint; it is not yet the selected final algorithm.
- The candidate method changes only the pre-LUT curve result. Existing sRGB-shaped LUT handling and max-channel N2 compression/reconstruction remain unchanged. Compare methods with pivot `0.7` and all other settings fixed.
- The method reuses c59.y, previously named `SceneExposure`; that value had no HLSL consumer. The obsolete `LifeIsStrange_SceneExposure` diagnostic key is no longer read.

## Runtime 06A2 shader A/B (2026-10-02)

- Added the always-visible segmented RenoDX UI control `06A2 Shader Source`: click `RenoDX replacement` or `Native game shader`. It is evaluated by the 06A2 draw callback, so switching does not require restarting the game; RenoDX creates the replacement pipeline through its existing path when selected.
- This isolates the shader replacement choice only. FP16 resource upgrades, 512/FC2A processing, HDR settings, and the DX11 proxy remain as configured; Native mode is not a full vanilla-rendering comparison.
- Startup and setting-change logs report the active 06A2 source. Runtime switching still requires an in-game visual check for D3D9 pipeline state restoration.

## Alternate gameplay angle: 0x6B7D5C22 clip (2026-10-03)

- DevKit capture with DX11 proxy off: D3D9, 1920x1080 B8 swapchain, 813 draws, DevKit build `lifeisstrange-devkit-873b59af8a2b70358036045501caef424f690217`. This snapshot contains no `0x06A2A81D`; gameplay draw #785 uses pixel shader `0x6B7D5C22` instead.
- Draw #724/657's `0x51229A9B` pass reads FP16 `0x36D8E280` and writes FP16 `0x36D8E160`. Live readback of both views was effectively identical: max RGB `(6.6445, 5.5898, 5.125)`, max luminance `5.7805`, and 3,894 pixels with an RGB channel above 1.0. This 512 FP16 pass preserves the measured highlight in this capture.
- Draw #785 (`0x6B7D5C22`) samples that FP16 `0x36D8E160` at s0 and writes resource `0x36D8C100`, whose active FP16 clone is `0x372F5520`. The clone's current contents, consumed by draw #786 (`0x51229A9B`), max at RGB `(0.9824, 0.9800, 0.9756)`; no RGB value is above 1.0. Thus the HDR-range loss is before the later 512 pass, at or within #785.
- Original PS3 disassembly for `0x6B7D5C22` was available; `cmd_Decompiler.exe` failed. Instructions 96–100 apply `ImageAdjustments2` c8's per-channel rational curve, then `mul_sat r0` clamps before s5 `ColorGradingLUT` sampling. Instruction 131 ends with `mad_sat oC0.xyz`, another output clamp. This is a confirmed destructive SDR/LUT path, not evidence that a later overlay is the first clip.
- The shipped editable `postprocess_0x06A2A81D.ps_3_0.hlsl` targets the separate `0x06A2A81D` hash; there is no `0x6B7D5C22` replacement in the current LiS source. The prior 06A2 replacement therefore does not cover this captured gameplay variant.
- All resource statistics above are DevKit live-resource readbacks (`snapshotExact=false`), not frozen per-draw copies. The matching draw bindings and shader math localize the likely clip, but an exact temporal before/after measurement at #785 was not captured.

## 0x6B7D5C22 native-equivalent baseline (2026-10-03)

- Dumped the original `ps_3_0` bytecode with DevKit to `Life Is Strange/Binaries/Win32/renodx-dev/dump/0x6B7D5C22.ps_3_0.cso` (4,416 bytes; SHA-256 `93F61B3B1DA1AD7625292D9C764DBDCDFB2625C4288AF17993DEE7A46FCF79D3`). `cmd_Decompiler.exe` still fails; downloaded `HlslDecompiler.exe` produced `.asm` and `.fx`, both retained beside the CSO in `renodx-dev/dump`.
- Manual audit found the generated `.fx` lost the `dp3` weight vector: it emitted `dot(r0.zwy, 0.3)` instead of `dot(r0.zwy, c29.xyz)` (`0.3, 0.59, 0.11`). The editable baseline restores that vector, explicitly binds the original constants/samplers, and retains the source `mul_sat` before LUT and final `mad_sat`.
- Added `.baseline/0x6B7D5C22.ps_3_0.hlsl` as a native-only baseline. DevKit compiled and activated it from an isolated live directory; `devkit_get_shader` reports `source=File`, hash `0x6B7D5C22`, profile `ps_3_0`. The user visually confirmed the replacement matches the original closely; this is not a pixelwise comparison. No HDR bridge or curve change is included in this fallback file.

## 0x6B7D5C22 HDR LUT bridge candidate (2026-10-03)

- Added `postprocess_0x6B7D5C22.ps_3_0.hlsl` separately from the native-equivalent fallback. It gates the HDR branch on `LIFEISSTRANGE_HDR_PIPELINE`; HDR-off retains the original curve, LUT-domain `mul_sat`, final `mad_sat`, and half-precision output.
- The HDR branch captures the post-composite/bloom/light-shaft signal before `ImageAdjustments2`, uses the existing per-channel tangent pivot and max-channel LUT compression controls, preserves the packed 2D LUT addressing, reconstructs the scale once after LUT sampling, and keeps post-LUT vignette/grain. Only the two RGB clamps are bypassed in HDR; mask, UV, bloom, and light-shaft saturates remain.
- sRGB decode/encode for the LUT bridge is provisional, copied from the existing 06A2 transport path. The 6B draw's runtime c8 values and LUT transfer domain have not been captured independently; verify before treating this as final.
- The shader map uses `__ALL_CUSTOM_SHADERS`, so the hash-named HLSL is picked up by the Life Is Strange addon build without a hand-written map entry. Compile and runtime checks for this candidate are pending.

## 0x6B7D5C22 addon integration (2026-10-03)

- The verified native-equivalent baseline is retained at `.baseline/0x6B7D5C22.ps_3_0.hlsl`; the leading-dot folder is intentionally excluded by the repository shader build. This avoids two HLSL files generating the same `0x6B7D5C22.cso` embed.
- The active `postprocess_0x6B7D5C22.ps_3_0.hlsl` captures the same `r0.zwy` RGB swizzle used by the native `ImageAdjustments2` curve before extending it.
- Added an independent `6B7D5C22 Shader Source` segmented setting for runtime native/replacement A/B. `HDRCurvePivot`, `HDRCurveMethod` (per-channel vs max-channel shared RGB scale), and `HDRPipeline` remain shared with 06A2; resource upgrades and proxy behavior are unchanged.
- Build/runtime validation remains pending. The capture established the 6B draw's immediate FP16 input/output neighborhood, but did not prove that the same frame continues through FC2A to swap.

## 0x006B1C38 native-equivalent candidate (2026-10-04)

- DevKit dump `0x006B1C38.ps_3_0.cso` is 3,872 bytes (SHA-256 `296D3A26441909CA1C4D96D0724BF42664615D8F3F2DE7411CCE41E0B3463AAA`). HlslDecompiler `.asm` is the semantic reference; its `.fx` is not yet a safe replacement.
- Manual audit found two lost `dp3` vectors in `.fx`: luminance weights `c24.yzw=(0.3,0.59,0.11)` became scalar `0.3`, and depth-state weights `c25.yzw=(1,2,3)` became scalar `3`. The candidate restores both vectors and explicitly binds the omitted `c0` constant and `s0` scene sampler.
- Like 06A2, this PS3 shader applies the `ImageAdjustments2` rational channel curve, saturates before four packed-2D `ColorGradingLUT` samples, then applies vignette/grain and a final RGB saturate. It is a post-process variant with `DNEHexDofTexture1` at s4 and the LUT at s5; its runtime c9 values and LUT transfer domain are not captured.
- No retained draw snapshot identifies `0x006B1C38` as animation-only. The 2026-10-03 gameplay capture used the distinct hash `0x6B7D5C22`; shader dump presence alone does not establish draw usage.
- Added an isolated segmented Native/RenoDX shader-source selector. The live native-equivalent candidate compiled and activated through DevKit; the user confirmed its picture looks normal. The HDR bridge and addon build were pending at that point.

## 0x006B1C38 shared HDR curve/LUT bridge (2026-10-04)

- Added the HDR branch to the verified live-equivalent HLSL. It uses the shared `HDRCurvePivot`, `HDRCurveMethod`, and `EnableHDRPipeline` controls already used by 06A2/6B7D5C22; no new curve setting or changes to those shaders.
- HDR branches around the native rational curve plus LUT-input `mul_sat`, substitutes the pivot-tangent curve and a bounded max-channel-compressed LUT proxy, then reconstructs once after sampling. It removes only the final color `mad_sat`; spatial/depth/mask saturates and LUT-proxy bounds remain.
- SDR/HDR-pipeline-off retains the native curve, LUT input clamp, and final output clamp. Runtime LUT transfer-domain validation and addon build are pending.

## Animation/post-process shader expansion (2026-10-06)

- F8 snapshot identified five additional PS3 post-process candidates: `0x0A3988C0`, `0x8FC18C2F`, `0x9563BC16`, `0xD478BD88`, and `0xE52684C3`. `0x51229A9B` remains unchanged because its role is a later composite/transport pass, not a proven ImageAdjustments2/LUT pass.
- HlslDecompiler output for all five candidates was manually corrected against the retained `.asm`: the missing luminance `dp3` vector is `(0.3, 0.59, 0.11)`, the missing depth-state `dp3` vector is `(1, 2, 3)`, and omitted `c0`/`s0` bindings were restored. The native spatial, bloom, vignette, grain, packed-LUT addressing, swizzles, and write masks remain intact.
- Each active `postprocess_0xHASH.ps_3_0.hlsl` keeps an HDR-off/native-equivalent branch and adds the same HDR LUT bridge used by the previously validated candidates: ImageAdjustments2 tangent extension, max-channel LUT transport compression, one reconstruction after LUT sampling, and removal of only the final HDR color clamp. Intermediate spatial/mask saturates remain unchanged.
- The five hashes are registered through `__ALL_CUSTOM_SHADERS` and each has an independent Native/RenoDX source selector. `HDRCurvePivot`, `HDRCurveMethod`, and `EnableHDRPipeline` are shared; no replacement was added for `0x51229A9B`.
- Local `fxc` validation with `ps_3_0`, `/Gec`, `/D __SHADER_TARGET_MAJOR=3`, and `/D __SHADER_TARGET_MINOR=0` compiled all five shaders successfully. Dumped bindings retain `shader_injection c50`, `SceneColorTexture s0`, and the original per-shader LUT sampler (`s5`, `s7`, `s6`, `s6`, `s6`). This is shader/compiler evidence only; the x86 addon build and runtime A/B remain pending.

## 0x4FA5AD22 semantic baseline (2026-10-09)

- F8 captures place `0x4FA5AD22` among the same late post-process family as `0x06A2A81D`/`0x006B1C38`, but it is a distinct pass, not a hash alias: it samples `LowResPostProcessBuffer` at s5 for a depth composite before `ImageAdjustments2`, uses `ColorGradingLUT` at s4, then applies vignette/grain and final RGB saturation. Its own draw-level c8 constants have not been independently captured.
- Added `0x4FA5AD22.ps_3_0.hlsl` as a native-semantic replacement only. It preserves the rational `ImageAdjustments2` operation, the saturating LUT-domain write, the packed LUT sampling path, and final `mad_sat`; it does not use `HDRCurvePivot`, `HDRCurveMethod`, or the HDR LUT bridge.
- Corrected two scalarized HlslDecompiler expressions against the retained ASM: luminance `dp3` uses `c16.yzw=(0.3,0.59,0.11)`, and the depth-state `dp3` uses `-c17.xzw=(3,1,2)`. Register bindings are explicit for dynamic c0/c2/c7-c14 and samplers s0-s5.
- Added an independent `LifeIsStrange_4FA5AD22ShaderSource` selector. `RenoDX replacement` selects the semantic HLSL baseline; `Native game shader` leaves the original bytecode active. Resource upgrades, other shader selectors, shared HDR controls, and the DX11 proxy are unchanged.
- System D3DCompiler compiled the replacement as `ps_3_0` in backward-compatibility mode (`/Gec` equivalent): 2,772-byte bytecode; one unrelated `aces.hlsl` loop-variable shadow warning. This is shader compilation and ASM audit only. The x86 addon build and in-game Native/RenoDX A/B remain unverified.

## 0x4FA5AD22 HDR LUT bridge test (2026-10-10)

- Added the shared `ImageAdjustments2` tangent extension and bounded LUT transport only when `LifeIsStrange_EnableHDRPipeline=1`; the original-semantic SDR branch, packed LUT sampling, depth composite, vignette, grain, alpha, and source selector remain intact. HDR reconstructs the LUT result once and bypasses only the final RGB saturate.
- The HDR input swizzle is `r0.zwy` (RGB); LUT transport is explicitly repacked as `B,B,R,G`, matching this shader's packed-address math. No resource-upgrade, proxy, readback, or other shader behavior changed.
- F8 captures establish that this hash is used in the tested scene family, but do not establish that its draw input exceeds 1 or that its output/intermediates are fully upgraded to the final proxy. Treat the first runtime result as a targeted shader A/B, not proof of the complete HDR chain.
- System D3DCompiler compiled the updated shader as `ps_3_0` in backward-compatibility mode: 4,576-byte bytecode, with only the existing `aces.hlsl` loop-variable shadow warning. Full addon/action build and runtime A/B remain pending. Startup/source-change logs now distinguish `RenoDX HDR LUT bridge` from the HDR-off semantic baseline.
- Runtime test: keep `LifeIsStrange_4FA5AD22ShaderSource=0`, `LifeIsStrange_EnableHDRPipeline=1`, `HDRCurvePivot=0.70`, and `HDRCurveMethod=0` (per-channel tangent); compare the same toilet-item prompt border/angle against Native (`ShaderSource=1`) with all other settings unchanged. Confirm the startup log says `4FA5AD22_shader_source=RenoDX_HDR_LUT_bridge`.

## 0x5DA670EE variant and incremental shader extraction (2026-10-11)

- The latest three F8 captures (`20261011-003130-856`, `20261011-003133-223`, `20261011-003135-239`) all contain the same post-process variant. `0x36B78BE0` is a 1920x1080 `R16G16B16A16_FLOAT` resource sampled by `0x5DA670EE`; because it is non-sRGB FP16 and is consumed as `SceneColorTexture`, it is the proven linear scene-color working signal for this pass.
- `0x5DA670EE` writes a 1920x1080 `B8G8R8A8_UNORM` target, samples `ColorGradingLUT` at s5, uses ImageAdjustments2 from c10 on RGB `r0.zwy`, packs the LUT address as `B,B,R,G`, and has a LUT-domain `mul_sat` plus final `mad_sat`. It is a distinct ImageAdjustments2/LUT post-process variant, not an alias of 06A2.
- Added `postprocess_0x5DA670EE.ps_3_0.hlsl` from the retained `.asm`/HlslDecompiler output. HDR-off preserves the native curve, LUT clamp, vignette/grain, alpha, and final clamp. HDR-on uses the shared tangent/LUT bridge and removes only the LUT-domain and final RGB clamps; no spatial/depth/mask saturates were changed.
- Added `LifeIsStrange_5DA670EEShaderSource` for isolated Native/RenoDX A/B. The shared `HDRCurvePivot`, `HDRCurveMethod`, and `LifeIsStrange_EnableHDRPipeline` controls are reused; resource upgrades, readback, and proxy logic are unchanged.
- DevKit captures can extract every shader encountered by the current runtime path as `.original.cso`; repeated F8 batches are deduplicated by hash. They cannot guarantee every shader in the game in one run because D3D9 creates many shaders lazily by scene/material. The reliable workflow is multi-scene F8 capture, hash merge, pixel-shader filtering, then FP16/LUT/full-frame ranking before decompilation.

## Experimental 4-LUT candidates: no-LUT ToneMapPass (2026-10-18)

- Added the existing `HDRCurveMethod=3` direct `ToneMapPass` A/B to all 22 cache-derived candidates. It replaces the LUT-grade contribution at the same output point, preserving each shader's later vignette/grain; methods 0-2 still use the prior LUT bridge. The 22 remain experimental and are not confirmed runtime chain members.
- With captured curve constants `a=0.21411191`, `b=0.97323596`, `f(0.18)=0.4624`, close to sRGB encoding of linear 0.18. The audited shader has no explicit gamma/pow conversion between this rational curve and LUT coordinate generation. This supports a gamma-shaped LUT domain, but does not prove the game's scene input uses calibrated 18% middle gray.
- `ToneMapPass` method 3 consumes the pre-curve scene signal and ignores `HDRCurvePivot`; RenoDX's ToneMapPass/RenoDRT config already defaults to scene mid-gray 0.18. For methods 0-2, 0.18 is only a conventional A/B pivot, not a confirmed LiS pivot; user previously reported 0.7 as visually acceptable.

## Dual-limit LUT bridge (2026-10-18)

- `HDRCurveMethod=2` uses two ImageAdjustments2 tangent-to-hyperbola curves: the LUT/chroma limit remains fixed at `1.25`, while the HDR reconstruction-reference limit is derived from `ToneMapPeakNits / ToneMapGameNits`. It is encoded for the upstream `GammaCorrection` because FC2A sRGB-decodes the bridge result before `RenderIntermediatePass` applies that correction.
- FC2A scales the scene by `GameWhite / GraphicsWhite`, then the proxy scales by `GraphicsWhite`; the matched `IntermediateEncoding`/`SwapChainDecoding` pair cancels. The derived upstream limit is independent of the DX11 proxy toggle, so DevKit captures use the same curve. The final `SwapChainPass` peak clamp is unchanged.
- The bridge still applies N2 max-channel compression before the original packed LUT and restores the HDR reference after grading. The derived curve limit is a reconstruction target, not a replacement for the final `SwapChainPass` peak clamp. Method 2 does not call `ToneMapPass`; method 3 remains the separate direct ToneMapPass A/B.
- If the configured peak is below the curve value at `HDRCurvePivot`, the derived limit is floored just above the pivot value to keep the hyperbola well-formed; the final SwapChainPass remains responsible for enforcing the lower nit ceiling. Runtime image/color/peak validation is required.
- `1.25` is Method 2's custom LUT/chroma-curve asymptote, not the vanilla curve limit: the captured `b=0.97323596` gives an analytic vanilla asymptote `1/b ~= 1.0275`, while the original shader's `saturate` clips the actual LUT input to `1`. After sRGB decode and N2 max-channel compression, the `1.25` curve ceiling maps to an encoded LUT input of about `0.9345`.
- Range check across 81 peak/game-white/gamma/pivot combinations covered by UI settings: un-floored peak round-trip error was below `1.1e-14`; the maximum derived HDR curve-domain limit was `7.47` (4000-nit peak, 48-nit game white, gamma 2.2), below FP16's 65504 maximum. This is math-only validation; FXC/DXC/Slang are unavailable locally, so the branch Action must verify compilation.
- Scalar round-trip checks map the asymptotic reference back to the configured peak for 1000/4000 nit peaks, 203/500 nit Game White, and sRGB/2.2/2.4 gamma paths. This is math-only validation; local FXC compilation and in-game A/B remain pending.
