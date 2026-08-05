# Dying Light 2 HDR/DLSS Debug Notes

## 2026-08-01 Off Capture

Source: `ReShade.log` from the live game directory.

### 0x3E input/curve

- `format=26`, `view=26`, `1920x1080`, no clone.
- Curve constants were stable across four draws: `2.27, 0.17, 1.69, 0.8, 0.14`.
- This does not indicate a source format or curve mismatch.

### Targeted color path

- `0x3E` source is native format 26 and is routed to the same native resource (`26 => 26`).
- `0x3E` entries reported `replacement_bound=0`.
- `0x268` and `0xAD` use FP16 clone-backed outputs (`27/29 => 10`) and their replacement paths are active.
- Therefore the remaining DLSS Off color difference is not explained by the 0x3E curve or by the FP16 clone format alone; the 0x3E replacement binding state must remain a separate suspect.

### Settings-save green flash

The settings exit path triggered repeated swapchain recreation:

- multiple `ResizeBuffers` calls at 1920x1080;
- swapchain generations advanced from 2 through 6;
- `DL2 DLSS FG: resize wait result=no_fence`;
- repeated `DL2 targeted t0 clone bind skipped: replacement table allocation failed`.

This is consistent with a transient clone/descriptor-table gap during resize, and is independent of the steady-state DLSS Off source path.

## Next diagnostic boundary

Do not change DLSS Balanced or the HDR clone format. First determine whether the Off color mismatch is caused by the unbound `0x3E` replacement or by the native source content. Separately, make resize-time descriptor allocation resilient before treating the green flash as a color issue.

## 2026-08-01 Follow-up

### Replacement audit limitation

The post-bind state mirror continued to report the native pipeline for `0x3E`, `0x268`, and `0xAD` even though their replacement pipelines existed and replacement debug modes visibly executed. Direct pipeline binding is not reflected back into this state mirror, so `replacement_bound=0` is not evidence that the replacement shader failed to run.

### Legacy sRGB A/B result

With DLSS SR Off, `0x3E Legacy sRGB Output (A/B)` made the image visibly washed out rather than matching the correct DLSS Balanced appearance. Restoring the old `RenderIntermediatePass` at `0x3E` is therefore rejected as a direct fix. The Off mismatch is not a simple missing legacy sRGB encode.

### Runtime DLSS mode switch crash

Changing DLSS mode while already in gameplay caused an immediate crash. Treat runtime DLSS mode switching as unsafe until resource lifetime handling is fixed. This likely shares the settings-save resize boundary: the upscaler output resource changes while FP16 clone views and replacement descriptor tables still refer to the previous generation.

## 2026-08-02 Resource Upgrade Isolation

The startup resource matrix established the following boundary:

- Native formats and `UNORM + sRGB only` preserve the expected DLSS Off color, but cap the sky near 203 nit.
- Enabling the full-size `R8G8B8A8_TYPELESS => R16G16B16A16_FLOAT` clone restores HDR headroom above 203 nit, but corrupts the DLSS Off color path.
- The UNORM and sRGB clone families are therefore neither the source of the color error nor sufficient to restore HDR headroom.

The next build narrows the typeless rule by creation index. `Typeless Resource Candidate` is a global startup setting with candidates 0 through 7; every candidate retains the UNORM and sRGB rules. A full game exit and restart is required after each change. The target result is native-format color with HDR headroom above 203 nit.

### Single-candidate result

Candidates 2, 3, 4, and 6 retain the expected color but remain capped near 203 nit. Candidate 5 changes the color darker and candidate 7 makes the image brighter, but both remain capped. This rules out a single required Typeless resource. The next matrix tests combinations `2+3`, `2+4`, `3+4`, `2+3+4`, `2+4+6`, and `2+3+4+6`, with UNORM+sRGB upgrades retained.

All tested combinations also remained capped without changing color. The indexed rules only cover one creation instance per selected index, whereas the broad rule continues matching later resources. The matrix is extended with ranges 8-63 and cumulative ranges 0-63. The `All typeless` mode is restored to the actual unbounded rule rather than a 0-7 mask.

The `8-15` range alone remained color-correct but capped at 203, while cumulative `0-15` exceeded 203 with incorrect color. This indicates an interaction between resources in `0-7` and `8-15`; `Cumulative 0-7` is added as the next boundary test.

The targeted color-path audit now reports `upgrade_index` for each captured input and output. This is derived from the actual `ResourceUpgradeInfo` attached to the resource clone, so it can identify which indexed rules are used by 0x3E, 0x268, and 0xAD without changing rendering behavior.

The first indexed capture identified the concrete late-color chain: 0x3E writes candidate 4, 0x268 reads 4 and writes candidate 5, and 0xAD reads candidate 7 after the intervening UI/composite writers. Pairwise tests and the exact `4+5+7` combination replace further broad range searching.

Runtime validation confirmed that the complete `4+5+7` set restores both correct color and HDR headroom. The pairwise subsets are incomplete: `4+5` renders too dark and `4+7` renders too light. Mode 0 is therefore changed to the production default exact chain, while the former unbounded Typeless rule remains available only as the final diagnostic option.

DLSS Balanced invalidated the fixed indices: all three captured late-color resources reported `clone=0` and `upgrade_index=-1`, leaving the path capped at 203 nit. FG then overexposed the final FP16 proxy independently. A DL2-local creation tracker now resets on swapchain initialization and assigns `creation_index` to every matching full-size Typeless render target, allowing unmatched Balanced and FG resources to be mapped without modifying the global resource-upgrade framework.

The tracker mapped Balanced without FG to candidates `0 -> 1`, and Balanced with FG to `2 -> 3`, while Off uses `4 -> 5 -> 7`. A fixed union would reintroduce unrelated upgrades. The next experiment creates inactive clones for all matching Typeless resources and calls the existing framework `ActivateCloneHotSwap` only from the known 0x3E, 0x268, UI-writer, BFFC, and 0xAD shader callbacks. This selects resources by rendering role rather than creation order.

The first semantic capture reported `clone=1` but retained resource/view formats `27=>27` and `29=>29`, so no FP16 view was actually bound. Existing working hot-swap games use a `R16G16B16A16_TYPELESS` clone resource for Typeless sources, allowing lazy view creation to map onto FP16. The semantic mode is updated to follow that established configuration; fixed-index modes retain their proven FLOAT clone.

The Typeless destination alone did not change the effective view. `ActivateCloneHotSwap` marked the resource enabled, but DL2's manual target rewrite continued to pass the original RTV. The callback now explicitly obtains the framework-managed lazy clone with `GetResourceViewClone` and rewrites the render target using that view; descriptor flushing remains responsible for the matching SRV views.

The explicit lazy-view rewrite caused a black screen and is rejected. Stable fixed-index mode `4+5+7` remains valid for Off. The captured Balanced mappings are exposed as separate startup modes `0+1` (FG Off) and `2+3` (FG On), avoiding runtime resource mutation. FG's PQ/linear overexposure remains a separate follow-up.

FG submission diagnostics now include the resource format and clone state for both `backbuffer` and `copy_source`. This distinguishes a linear FP16 source copied into an RGB10/PQ target from a correctly encoded PQ source without adding another mutation experiment.

Submission diagnostics now also print `final_color_mode` and `proxy_action`, so Direct versus Round-trip can be verified from the log rather than inferred from the UI setting.

## 2026-08-04 Stable Chains and FG Semantic Boundary

### Confirmed startup resource chains

- DLSS Off: exact candidates `4+5+7` preserve the expected color and restore HDR headroom above 203 nit.
- DLSS Balanced, FG Off: exact candidates `0+1` are the matching resource chain.
- DLSS Balanced with FG: exact candidates `2+3` upgrade the real-frame chain, but generated frames remain too dark/over-saturated and can report highlights above 10000 nit.
- These mappings are mode-specific. Do not combine them into a broad union and do not switch DLSS modes at runtime while diagnosing resource lifetime behavior.

### Rejected semantic hot-swap

The semantic hot-swap path first activated a clone without changing the effective resource/view formats. Explicitly rewriting the RTV to the lazy clone then produced a black screen. This path is rejected because the DL2 D3D12 descriptor, view, and barrier handoff is not synchronized as one atomic operation. Keep the proven fixed-index startup chains and do not use mode 31.

### FG submission result

The FG Direct and Round-trip paths are confirmed to execute. Submission logs also confirm that Compatibility changes `proxy_action` from `skip_generated_proxy` to `force_proxy_source`. Neither branch corrected the generated-frame appearance, so the remaining failure is not explained by a branch that failed to run, nor by the backbuffer/copy-source storage format alone.

### Current diagnostic hypothesis

The next boundary is Streamline color-tag metadata and transfer semantics. Earlier handoff logging observed two color tags with an unknown/zero native format. This is suspicious but not yet proof of the root cause: a pre-PQ linear resource, an already PQ-encoded final color, or a RenoDX proxy result may be registered without enough metadata for DLSS-G to interpret it consistently. The next diagnostic must enumerate every tag type, native resource and `nativeFormat`, dimensions, tracked clone, and which tags are counted as color inputs. Do not change the Off/Balanced tone mapping, PQ encoding, or fixed resource chains until that capture identifies the actual FG color input.

## 2026-08-05 Off/Balanced HDR Regression Checkpoint

### Step 1: startup configuration audit

The live `ReShade.ini` was inspected before making any rendering-code change. The global startup-only setting is currently:

```ini
[renodx]
ResourceUpgradeTest=33
```

`ResourceUpgradeTest` is read from the global `renodx` section when the addon initializes. A same-named value under a preset section is not used for resource creation. The code maps the active value as follows:

- `0`: exact DLSS Off chain `4+5+7`.
- `32`: exact DLSS Balanced, FG Off chain `0+1`.
- `33`: exact DLSS Balanced + FG chain `2+3`.

This means the last run used the FG-specific resource chain while testing DLSS Off and/or Balanced with FG disabled. That mismatch is sufficient to explain a return to native-format behavior and the approximately 203-nit ceiling; it is not yet evidence of a shader or tone-mapping regression.

### Step 2: controlled baseline procedure

Do not change DLSS mode while the game is running. For each test, select the resource-chain mode, exit the game completely, start it again, and confirm the startup log line `DL2 typeless candidate test` reports the expected mode and mask.

1. DLSS Off: select `Exact HDR chain 4 + 5 + 7` (mode `0`), restart, then measure a known bright scene.
2. DLSS Balanced with FG disabled: select `Exact Balanced chain 0 + 1` (mode `32`), restart, then measure the same scene.

Expected result for each matching chain is correct mode-specific color and highlights above 203 nit. A confirmed result will be recorded as the stable Off/Balanced HDR-chain repair summary before returning to the separate FG generated-frame issue.

### Step 3: controlled baseline result (confirmed)

Both restart-only tests passed with their matching chains. DLSS Off works with mode `0` (`4+5+7`), and DLSS Balanced with FG disabled works with mode `32` (`0+1`). Their color and HDR headroom are correct in the user-verified test scene.

**Repair summary:** the observed Off/Balanced 203-nit regression was a startup configuration mismatch, not a new shader, tone-mapping, or proxy regression. Mode `33` must be reserved for Balanced with FG. Select the chain that matches the intended DLSS/FG state before starting the game; do not change the rendering mode in a live session.

## 2026-08-05 Resource-Chain Hot-Switch Feasibility

### Step 4: lifetime and mutation audit

The exact-chain choice is consumed during addon initialization to build `resource_upgrade_infos`. The resource utility then applies a matching creation-index rule when the game creates each full-size `R8G8B8A8_TYPELESS` render target, creates an FP16 clone, and creates tracked resource views. Changing the UI setting later changes only its saved value; it does not retroactively create clones or rewrite the game, ReShade, and Streamline descriptor references for resources that already exist.

The earlier semantic hot-swap experiment is direct negative evidence: it enabled a lazy clone at a shader callback, but the game's manual render-target rewrite initially retained the original view. Rewriting the RTV to the clone explicitly then produced a black screen. DL2 therefore does not provide an atomic safe point where resource selection, all SRV/RTV descriptor tables, command-list state, and barriers can be changed together.

### Decision

Hot-switching is theoretically possible only as a full transactional renderer reset, not as a normal setting callback. A safe implementation would need to stop presentation, wait for the game queue and the DLSS-G input-completion fence, retire every affected resource/view/descriptor binding, force the game to recreate its temporal-upscaler targets, rebuild the matching clone map, and resume only after the new resources have been tagged. DL2 does not currently expose a proven callback that guarantees this boundary; changing DLSS in gameplay has already caused resource-lifetime crashes.

Keep the current restart-required chains. A future quality-of-life improvement may auto-select the correct chain at startup after detecting the initial DLSS/FG configuration, but it must still require a restart when the game's mode changes. This preserves the confirmed HDR repair instead of reintroducing the black-screen and FG synchronization failures.

## 2026-08-05 Historical Off/Balanced Result Reconciliation

### Step 5: separate color parity from HDR headroom

The historical A/B record confirms that build `787fe68` made DLSS Off and Balanced visually color-consistent. Builds `bb5a1e1` and `5ebc218` reintroduced the visible color split. However, that comparison established color parity only; it did not establish that both modes simultaneously preserved values above the approximately 203-nit boundary.

The later raw-ladder and resource-lineage work established the missing brightness condition separately. Native-format or incomplete clone paths could match color while remaining capped at 203 nit. Broad Typeless upgrades could restore headroom while corrupting color. Exact `4+5+7` was the first recorded Off chain to satisfy both conditions, after which Balanced was observed with `clone=0`/`upgrade_index=-1` on that startup mapping and required its separate `0+1` chain.

Therefore the user's memory of simultaneous Off/Balanced color parity is correct, but the current records do not support a single historical build/configuration that was verified to provide both correct color and HDR headroom for both modes without restart. Testing `787fe68` against the current fixed scene and nit measurement would be the definitive way to fill that historical evidence gap; it is not required for the current stable mode-specific repair.

### Step 6: `787fe68` versus `5ebc218` code boundary

Conversation review confirms `787fe68` was capped near 203 nit. Its code change only corrected the final proxy interpretation to Linear BT.709 (`ENCODING_NONE`), removing a second sRGB decode; it did not restore the missing FP16 intermediate range.

`5ebc218` is a credible HDR-headroom boundary. Its ancestry restored broad full-size Typeless and sRGB FP16 clone rules, changed the 0x3E bridge to emit Linear BT.709 directly, registered the 0x268 LUT replacement, and reconstructed HDR magnitude after the native SDR-domain grade. Those changes are specifically designed to carry values above 1.0 through the LUT instead of returning to the 203-nit reference-white ceiling.

The same code also explains why this was not yet the final shared solution: its Typeless rule matched every back-buffer-sized target rather than a rendering role or a mode-specific resource chain. Historical A/B already records `5ebc218` as Off/Balanced color-inconsistent. The expected old-build result is therefore both modes with HDR headroom, but a visible color difference between them. Runtime measurement of Off peak, Balanced peak, and color parity will confirm or reject that expectation before any new implementation is considered.

### Step 7: `5ebc218` runtime result

Old-build testing confirmed that `5ebc218` is normal only in DLSS Balanced; DLSS Off is not normal under the same broad resource-upgrade implementation. This rejects `5ebc218` as evidence of a previously working shared Off/Balanced solution. Its wide Typeless/sRGB clone rules happened to match the Balanced resource topology while upgrading the wrong or additional Off resources.

No `787fe68` retest is required. The conversation already records its approximately 203-nit ceiling, and its source change only corrected final proxy decoding without adding the FP16 intermediate and HDR LUT preservation introduced later. The remaining design problem is therefore resource-role identification across mode-specific creation topologies, not recovering a lost known-good universal rule.

### Step 8: Watch Dogs reference audit

The repository contains `src/games/watchdogs`, but its metadata identifies the original 2014 `Watch Dogs` (Steam app `243470`), not `Watch Dogs 2` or `Watch Dogs: Legion`. It has no DLSS or Streamline integration.

Its useful pattern is limited to DX11-style shader-scoped resource activation: it pre-creates `R16G16B16A16_TYPELESS` view clones for `B8G8R8A8_TYPELESS`, then known tone-map/AA shaders call `ActivateCloneHotSwap`, flush descriptors, and rewrite the active RTV. This is evidence that semantic activation can work when one graphics draw owns a simple immediate render-target transition.

It is not a safe template for DL2 mode switching. DL2 uses D3D12 descriptor tables, multiple typeless intermediates, compute/upscaler producers, temporal resource recreation, and Streamline references. The same semantic approach was already tested in DL2: activation without an effective view did nothing, while explicit lazy-clone RTV rewriting produced a black screen. Death Stranding Director's Cut is a closer DLSS-era reference, but it also installs static format-based upgrade rules during `init_device`; it does not dynamically change resource topology with the DLSS mode.

### Step 9: Luma Watch Dogs 2 DLSS reference audit

The Luma Framework Watch Dogs 2 implementation was shallow/sparse-cloned at upstream commit `9e5c8e7` for inspection. Unlike DL2, Watch Dogs 2 has no native DLSS integration here: Luma injects its own D3D11 NGX super-resolution pass, splits the game's deferred command list so NGX runs on the immediate context, captures the source color/motion/depth resources, and owns the resolve target and auxiliary resources.

This implementation can change DLSS quality without restarting because its color topology is invariant. The source is explicitly viewed as `R16G16B16A16_FLOAT`, the game pipeline is documented as linear, and only the NGX feature instance changes. `DLSS::UpdateSettings` compares resolution/settings, releases the old NGX feature and parameters, then creates a replacement feature before drawing. Per-frame source references are released after the injected pass, while owned resources are recreated when dimensions change.

This is useful evidence for the correct form of a safe runtime transition: the upscaler owner must control the feature lifetime, input/output resources, command execution boundary, and history reset together. It does not provide a directly portable fix for DL2, where the game and Streamline own those objects and changing DLSS also changes which native full-size Typeless resources are created. Luma therefore supports the earlier conclusion that a reliable DL2 hot switch requires a coordinated renderer/upscaler reset, not merely changing RenoDX's clone mask.
