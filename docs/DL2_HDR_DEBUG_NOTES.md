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
