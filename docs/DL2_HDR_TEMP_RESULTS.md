# Dying Light 2 HDR Temporary Results

This file records confirmed runtime A/B results that should not be repeated unless the final-proxy handoff code changes.

## Focused FG Final Color

Test context:

- DLSS Balanced + FG
- HDR10/PQ swapchain
- Focused game window
- Stable FG compatibility settings preserved

Confirmed results:

| Mode | Result | Interpretation |
| --- | --- | --- |
| Direct PQ | Image becomes visibly too dark/deep in color | Not a valid final mapping; PQ interpretation does not match the observed focused-FG source as currently handed off |
| PQ / BT.2020 | Same class of mismatch as Direct PQ | Do not repeat as a repair candidate without changing the source handoff |
| Linear / BT.709 (203 nit) | Color is closest/acceptable, but peak remains limited around 203 nit | Current best color semantic, insufficient absolute scale/headroom |
| Linear / BT.709 (Peak setting) | Breaks above 203 nit, but color becomes incorrect | Scaling alone is not a valid fix; it changes the interpretation of the source |

## Current Diagnostic Conclusion

### Compute bridge candidate (not runtime-tested yet)

The prior probe showed the graphics bridge was being attempted on the wrong command-list class. The next candidate uses the existing Streamline Compute list and writes a temporary RGB10 UAV before the native tray copy. No brightness/saturation compensation is used. The result is valid only if startup reports `RGB10 compute bridge: supported=1` and runtime reports `AD bridge ... command_list_type=2` followed by `rendered=1`.

The first Compute candidate met those two conditions but still hung because it transitioned the FP16 clone from `render_target` inside the Compute list. The next candidate prepares a separate FP16 staging resource on the Direct 0xAD post-draw callback, then restricts the Compute list to `shader_resource -> UAV` work.

The staging candidate ran without the hang but showed black flashes and ghosting. The current candidate completes the conversion on the Direct producer list and makes the Streamline Compute callback copy-only, removing the asynchronous FP16 staging read.

The Direct-produced RGB10 candidate still flashed whole frames. Since both trays were marked bridged, the remaining likely fault is missing cross-queue ordering for the custom prepared resource. The next build adds a GPU-only Direct-to-Compute fence wait.

The focused-FG final proxy source is an RGB10/UNORM handoff. The source-range probe reached the red/high bands while the raw-source quadrant changed between focused and unfocused states. This points to a focused-FG handoff representation/semantic mismatch or an RGB10 headroom bottleneck, not a simple exposure or saturation setting.

Do not repeat the four modes above as blind A/B tests. Future tests should change one handoff property at a time: source representation/encoding, pre-RGB10 scaling, or final-proxy resource format.

## Streamline Auxiliary Color Tags

Confirmed in both the earlier `de5f7f7` A/B and the current `451b015` capture:

- `UIColorAndAlpha` and `HUDLessColor` point to the same original format-27 resource.
- The Streamline wrapper reports `nativeFormat=0`, dimensions `0x0`, and no FP16 clone.
- The observed post-arm window contains no matching transfers, compute/UAV writers, or graphics writers.
- Explicit-null A/B for UI only, HUD-less only, and both disabled produced no visual change.

Conclusion: auxiliary UI/HUD-less tags are excluded as the cause of the deep focused-FG color, 203-nit Linear/BT.709 ceiling, extreme Direct-PQ output, and motion artifacts. Do not repeat the handoff/tag/transfer/writer capture unless the tag-routing code changes.

The unresolved input is Streamline's automatically intercepted Final Color path, which is separate from these auxiliary tags.

The next diagnostic binds input snapshots by the `slSetTagForFrame` native command context that executes the Final Color copy. Do not interpret a correlation as valid unless `input_match=1`, `tag_source=command_context` (or `carried` for a generated Present), `copy_tag_conflict=0`, `unmatched_copies=0`, and `input_evictions=0`.

Result: the automatic Final Color copy uses Streamline's private, stable command list rather than any `slSetTagForFrame` command buffer. All 16 samples correctly downgraded to `unmatched_copy`; do not repeat direct command-context matching. The focused-FG brightness fault remains at the RGB10 Final Color representation/encode boundary, not at the Game Exposure, Peak Brightness, or downstream scalar controls.

Next diagnostic: arm the existing source-writer tracker against the two dynamically discovered final RGB10 resources. Record draw/RTV, dispatch/UAV, copy, and resolve producers plus their visible source and native command context. This replaces command-buffer identity matching; it does not change rendering or repeat the transfer-function A/B matrix.

### Fence experiment note (2026-08-06)

The broad producer/consumer fence caused an unresponsive game because it waited on every Compute queue submission. The next build narrows the wait to command lists that actually contain the DLSS-FG bridge copy. This is an ordering-only change; it should be judged first by responsiveness and absence of global flashes, then by focused-FG color/headroom.

Runtime result for `933e223`: the narrowed wait is reached (`wait queue=... value=7293`) immediately after `AD bridge ... copied=1`, but focused entry still produces a black frame. This rejects the hypothesis that broad wait scope alone caused the black frame. The bridge's cross-queue resource handoff remains unsafe or cyclic even when the consumer list is identified precisely; stop adding queue waits to this path.

The subsequent Direct-PQ capture established the safe output contract: all 16 final submissions were RGB10 sources copied to RGB10 backbuffers with `preserve_copy=1`, `output_hdr10=1`, and `proxy_action=skip_generated_proxy`. The two Streamline sources alternated deterministically, while their visible producers remained `CopyResource` operations from the rotating original 0xAD targets. The next mutation therefore encodes the FP16 clone to BT.2100 PQ/RGB10 before Streamline, keeps the final copy native, and replaces the global-latest fence with per-prepared-resource fence values.

## Exact FG Producer Boundary

Build `89f9b8d` closed the writer question:

- Streamline fills its two alternating RGB10 Final Color resources with `CopyResource`.
- Each source is the original 0xAD target, while RenoDX's completed HDR image is in that target's effective FP16 clone.
- The copy cadence is stable and lossless in the 16-Present capture.

Do not repeat source-semantic or final-nit multiplier grids against the normalized RGB10 output. `cea9a1b` did not execute its bridge because it matched the 0xAD original as RGB10 instead of its observed RGBA8 UNORM format. The revised A/B is `AD FP16 -> Linear RGB10 Bridge`: normalize the FP16 source by `Peak / 203` before Streamline, then restore Peak in the final proxy. This is a paired representation change rather than an unpaired brightness or saturation compensation.

Safety requirements for that A/B are now explicit: final-proxy semantic 11 is gated by a successful bridge render for the exact RGB10 tray and current swapchain generation; failures and unmatched trays retain Direct PQ. A proposed pre-draw barrier mirror was not retained because it could not prove the clone's first physical state.

`ea4ad80` reached `rendered=1` but logged `source_state=2147483648` (`general`), followed by focused-FG black/stuck output. For the next build, the state is taken from the known 0xAD producer role (`render_target`) rather than the clone creation-state metadata.

`bf10d3c` then logged `source_state=4` but still black/stuck under focus. The follow-up build is probe-only: it reports the native command-list type and deliberately records no bridge render/copy, so Direct-PQ behavior can be isolated without another GPU stall.

### Completion-tracked slot result (2026-08-07)

Mode 11's PQ handoff executed for both Streamline trays, but focused FG flashed whole black frames. `DLSS FG Auxiliary Color Tags = None (Final Color only)` applied live (`mode=3`, both tags null) without changing the flash, so this test must not be repeated.

The remaining concrete race is prepared-resource reuse: the existing fence orders the Direct producer before the Compute copy, but the singleton resource can be overwritten by a later Direct frame before that Compute copy finishes. The next build uses a bounded per-source slot pool plus a consumer completion fence signaled by whichever queue submits the exact Compute list. Success requires repeated `consumer fence: signal ... uses=1`, no persistent `pool_exhausted=1` or `signal failed`, stable focused FG, and unchanged Off/Balanced behavior. It adds no Direct queue wait and no color compensation.

Final review tightened this to per-slot producer and consumer fences; a single shared monotonic value is unsafe when more than one native queue can signal it. Reset now releases unsubmitted reservations, and mode/swapchain changes advance a handoff epoch so late old work cannot publish a current tray. The live Auxiliary-tag switch is already proven effective and is not part of this build's test matrix.

Source destruction now moves prepared slots to deferred retirement and releases them during bridge teardown, avoiding both unsafe immediate destruction and an ownership leak across resource recreation.

Runtime result: `6202457` improved the black flash but still alternated final submissions between `bridge_ready=0` and `bridge_ready=1`; the same log also showed `pool_exhausted=1`. The next focused change retains the last published tray during replacement and increases the bounded slot pool to 8 per source. No PQ or color compensation changes are included.

Follow-up result: `bridge_ready=1` became stable, but residual previous-frame content remained. `d1c1799` added a targeted consumer-fence wait at final copy submission; it produced a jelly-like queue delay and did not remove the flash. The wait is rolled back while the per-slot lifetime fix remains. The next diagnostic should correlate generated Present identity with the exact source/tray serial rather than add another global or final-copy wait.

### Streamline trace

The bounded v2.9 interposer trace now covers `Present1`, `Present`, resize, and tag recycle. It is read-only and exists only to align Streamline display submissions with RenoDX FG tray/serial logs.

Runtime `bda1b4c`: after adding `sl.interposer.json` with a verbose log path, the diagnostic interposer generated `sl.log` and emitted the expected `DL2 SL Present1` lines. The original 128-call window was exhausted about 5.5 seconds after startup, before the target FG scene, so those samples cannot diagnose the focused-FG handoff.

The follow-up trace is armed by creating `dl2_streamline_trace.arm` beside `sl.interposer.dll` after entering the target scene. The marker is consumed automatically, then the next 256 `Present1` calls record per-feature hook ids, `skip` transitions, base return values, and elapsed time. This remains read-only and adds no queue wait or color mutation.

The DL2 Debug setting `Arm Streamline Present1 Trace (256)` creates the same marker beside the addon DLL, which is the game `x64` directory shared with `sl.interposer.dll`. `sl.interposer.json` still controls the log sink and is not rewritten at runtime.

Overlapped runtime trace: each 0xAD submission was consumed by the immediately following DLSS-G `Present1`; all calls returned success with `skip=1`, and ReShade observed two output Presents per source frame. Timing fall-through is excluded. The creation contract is mismatched instead: the DLSS-G-facing swapchain reports format 28 (RGBA8 UNORM) while every actual backbuffer is format 24 (RGB10 HDR). Streamline startup confirms four RGB10 real buffers but three `sl.dlssg.fake-swapchain-buffer` allocations in RGBA8. The next A/B rewrites only the main D3D12 DLSS-G creation descriptor from RGBA8 to RGB10 before those fake buffers are allocated; no Present wait, exposure, or color compensation is added.

Follow-up review: the first runtime attempt installed the Present hooks only after the initial fake buffers were created, so no creation-format line appeared. Retry now also occurs at `init_swapchain`; missing creation export no longer disables Present tracing. Creation-hook logs label returned descriptors as deferred because this is a Streamline before-hook and the real swapchain is created after the callback.

## Non-FG Brightness Mapping (2026-08-07)

- `HDR Input Range` false color marks source max-channel 4 as red and 8 as white; it is not a nit readout.
- DL2 then applies `scene_linear = source * 0.6`, so a red source near 4 becomes about 2.4, or roughly 487 nits at the fixed 203-nit intermediate unit before tone mapping.
- Raising highlight exposure retention from 75 to 100 and its clamp from 2 to 4 produced no meaningful brightness change. Switching Daniele to Neutwo also produced no meaningful change.
- Next probe: `DL2 HDR Expansion Grid` repeats the image in four quadrants: current baseline, no 0.6 calibration, fixed highlight-only gain, and Peak-driven highlight-only gain. It does not alter the normal path.

## Clean HDR LUT Decode Baseline (ed97c21)

- Path: HDR/RenoDRT normal path; A2 extended-curve bridge reverted; `4c4bc99` single inverse-sRGB decode retained.
- Runtime comparison: low/mid-tone pixels in HDR/RenoDRT now pixel-level match Vanilla/Off in RGB and perceived luminance.
- The prior low/mid fog desaturation/darkening is no longer present in this comparison.
- Do not reintroduce A2 or change ToneMap/Proxy parameters based on the low/mid-tone issue; the effective fix is the duplicate inverse-sRGB decode removal.
