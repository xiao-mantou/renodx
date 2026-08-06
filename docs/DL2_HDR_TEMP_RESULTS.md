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

## Exact FG Producer Boundary

Build `89f9b8d` closed the writer question:

- Streamline fills its two alternating RGB10 Final Color resources with `CopyResource`.
- Each source is the original 0xAD target, while RenoDX's completed HDR image is in that target's effective FP16 clone.
- The copy cadence is stable and lossless in the 16-Present capture.

Do not repeat source-semantic or final-nit multiplier grids against the normalized RGB10 output. `cea9a1b` did not execute its bridge because it matched the 0xAD original as RGB10 instead of its observed RGBA8 UNORM format. The revised A/B is `AD FP16 -> Linear RGB10 Bridge`: normalize the FP16 source by `Peak / 203` before Streamline, then restore Peak in the final proxy. This is a paired representation change rather than an unpaired brightness or saturation compensation.

Safety requirements for that A/B are now explicit: final-proxy semantic 11 is gated by a successful bridge render for the exact RGB10 tray and current swapchain generation; failures and unmatched trays retain Direct PQ. A proposed pre-draw barrier mirror was not retained because it could not prove the clone's first physical state.

`ea4ad80` reached `rendered=1` but logged `source_state=2147483648` (`general`), followed by focused-FG black/stuck output. For the next build, the state is taken from the known 0xAD producer role (`render_target`) rather than the clone creation-state metadata.

`bf10d3c` then logged `source_state=4` but still black/stuck under focus. The follow-up build is probe-only: it reports the native command-list type and deliberately records no bridge render/copy, so Direct-PQ behavior can be isolated without another GPU stall.
