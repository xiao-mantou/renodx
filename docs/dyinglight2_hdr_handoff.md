# DL2 HDR Handoff

Repo: `C:\Users\xiaom\Documents\renodx\trae\renodx`
Branch: `codex/dl2-hdr-next`
Runtime: `E:\SteamLibrary\steamapps\common\Dying Light 2\ph\work\bin\x64`

## Current chain

`0x3E36DA5B` scene tonemapper (real HDR scene)
-> `0x268BAB6D` world LUT / grading (Game anchor; UI not merged)
-> independent UI writers
-> `0xAD085E81` gamma/power pass (UI merged)
-> BFFC/final proxy/swapchain

DLSS/FG resource upgrade and timing code is fragile. Do not modify it while tuning Game/Peak unless explicitly required. Off/Balanced non-FG color path was previously made acceptable; FG still has separate HDR handoff/color/decode and temporal issues.

## User target

- Game Brightness changes paper-white and low/mid range only.
- Peak Brightness controls highlight shoulder/output and preserves cloud detail until actual display/peak clipping.
- Game=200 must not flatten clouds merely because midrange is raised.
- Keep native/off/balanced/FG resource semantics intact.

## Relevant commits

- `62cabd2` HDR Expansion Grid
- `9cec209` Game/Peak Anchor Grid
- `d5af804` four-quadrant residual fix
- `dd4c51c` dual-anchor normal `0x3E`
- `395dbdd` Game Separation Grid
- `f7c51be` post-LUT Game paper-white diagnostic (`0x268`)
- `4ac07b7` bounded Hermite highlight candidates

## Code locations

- `src/games/dyinglight2/tonemapper_0x3E36DA5B.ps_5_x.hlsl`
  - Helpers: `ToneMapDL2Anchored`, `ToneMapDL2Separated`, `ToneMapDL2FixedScene`, `ToneMapDL2FixedHermite`.
  - Normal path currently still calls `ToneMapDL2Anchored(..., RENODX_RENO_DRT_WHITE_CLIP)`.
  - Hermite candidate uses `HermiteSplineLuminanceRolloff`, clamps mapped result to `peak`.
- `src/games/dyinglight2/tonemapper_0x268BAB6D.ps_5_x.hlsl`
  - Debug modes 40..45 only: post-LUT Game tests; 40=`exit@2`, 41=`exit@4`, 42 uniform, 43/44/45 Hermite source white 4/8/16.
- `src/games/dyinglight2/addon.cpp`
  - UI currently exposes `ToneMapWhiteClip` (`min=1`, `max=100`, default 10), and Game/Peak controls.

## Latest test facts

- `DebugMode=41`, `ToneMapGameNits=100`, `ToneMapPeakNits=408`, `ToneMapWhiteClip=2`, `ToneMapType=3` was unstable: clip 2 can overshoot ~1500 nit; clip 3 can collapse near ~300 nit. This is a mathematical Neutwo boundary, not a useful final parameter.
- User reports Hermite Source White 8 (`DebugMode=44`) looks best among candidates.
- Current request: expose Source White 8 as a UI-adjustable control and then test Game=100 vs 200 at same Peak/SourceWhite.
- Recommended initial test: `Game=100`, `Peak=400`, `SourceWhite=8`; then only change Game to 200.
- Do not keep diagnostic grid modes as the final user-facing solution unless needed for another A/B.

## Next implementation

1. Reuse `ToneMapWhiteClip` as the Source White control; rename label to `HDR Source White`, default `8`, tooltip explaining it is the fixed scene input approaching Peak and independent of Game Brightness.
2. Clamp Source White in shader to remain numerically valid relative to `Peak/203` (epsilon required); avoid the old clip 2/3 boundary.
3. Connect normal `0x3E` path to `ToneMapDL2FixedHermite(..., RENODX_RENO_DRT_WHITE_CLIP)` only after preserving a diagnostic fallback if practical.
4. Connect `0x268` normal path to the validated `exit@2` low/mid Game scaling, before UI composition.
5. Build once, then test Game 100 and 200 with identical Peak/SourceWhite; inspect cloud peak/detail and UI separately.

## 2026-08-08 Implementation (items 1-4 done, uncommitted)
Working tree on `codex/dl2-hdr-next` now contains:

- `addon.cpp`: `ToneMapWhiteClip` label renamed to `HDR Source White`, default 8, tooltip documents scene units (1.0 = 203 nits) and independence from Game Brightness. `OnPresetOff` reset updated to 8. Key/binding unchanged.
- `tonemapper_0x3E36DA5B.ps_5_x.hlsl`: the normal RenoDRT path (`ToneMapType=3`) now calls `ToneMapDL2FixedHermite(untonemapped, vanilla, neutral_sdr, RENODX_RENO_DRT_WHITE_CLIP)`. Diagnostic fallbacks preserved: grid modes 38/39 still exercise Anchored/Separated, modes 40-42 FixedScene, modes 43-45 fixed Hermite source white 4/8/16. ACES/None keep their previous `ToneMapDL2Anchored` path.
- `tonemapper_0x268BAB6D.ps_5_x.hlsl`: the post-LUT Game weight block now runs unconditionally. Debug modes 40/41/42 keep exit@2/exit@4/uniform weights; the normal path uses the validated exit@2 weight (`1 - smoothstep(1.0, 2.0, luminance)`), so Game changes paper-white/low-mid without moving the Peak-owned highlight shoulder.

Source White clamping to `Peak/203` (epsilon) is handled inside `ToneMapDL2FixedHermite` via `max(source_white, peak + 0.001)`; the old Neutwo clip 2/3 boundary is no longer reachable on the normal path.

## Next (step 5): build and A/B test

1. Push branch `codex/dl2-hdr-next` (or `git diff --check` + commit + GitHub Actions).
2. Full restart with `ToneMapType=3` (RenoDRT), `DebugMode=0` (Off), DLSS/FG state chosen to match a resource-chain mode (`0` Off / `32` Balanced FG Off / `33` Balanced + FG).
3. Start at `Game=100`, `Peak=400`, `SourceWhite=8`; verify paper-white and low/mid respond to Game while cloud peaks track Peak.
4. Change only `Game` to `200`; confirm clouds are not flattened and UI stays separate.
5. If the Hermite Source White default of 8 is not the desired shoulder, adjust `HDR Source White` and compare at the same Peak/Game.

## Build/format

GitHub Actions is the build path; local FXC is unavailable/unreliable. Use `git diff --check`. Clang format executable:
`C:\Users\xiaom\Documents\renodx\clang_format\clang_format\data\bin\clang-format.exe`

## Streamline note

`sl.log` was mostly startup/exit resource logging (~117 KB/~762 lines); no dedicated `dl2_streamline_trace.arm` was observed. Do not treat it as the primary cause of HDR brightness behavior. Runtime JSON log path was set to the DL2 binary directory.

## 2026-08-09 Night highlight diagnosis and Night Scene Gain

### Night probe results (DebugMode 46, live)

- Mode 10 (raw `source.rgb`, no 0.6, no exposure): all deep blue -> every pixel < 0.25 in raw t0 units.
- Mode 11 (exposure scalar `t1`): yellow-green -> exposure ~1.5-2.0 at night.
- Mode 46 (`source * 0.6 * exposure`, linear segmented palette): mostly gray/cyan-blue, scattered purple, occasional green/yellow/red. So exposed scene is mostly 0.05-0.5 with local spikes to 1-8. The scene has real HDR structure (lights register up to ~8 scene units = >1500 nits raw); the majority is just physically dark.

### Two structural findings

1. **The Hermite knee is near scene 1.06.** `ToneMapDL2FixedHermite` runs `HermiteSplineLuminanceRolloff(max_channel, peak, source_white)` in log2 space; the spline knee sits at `1.5*log2(peak)/log2(source_white) - 0.5`, which for peak=422 nit (2.08 scene) and source_white=8 is at luminance ~1.06. Below that, the curve is identity -> the entire night scene (< 1.06) passes through unshaped. This is why night reads flat/gray: the curve only shapes the lights, not the dark bulk. It was validated for daytime cloud preservation, not night contrast.
2. **Exposure-protection controls barely move at night as configured.** At `RenoExposureStrength=75` and exposure ~2, `protected_exposure = lerp(1, 2, 0.75) = 1.75`, so the protection blend range is 2.0 vs 1.75 - nearly flat. Changing `RenoHDRProtectionStart` (0.75 -> 0.05) moves `adaptive_exposure` by ~0.02. Additionally, DebugMode 46 bypasses protection entirely (it uses raw `exposure`), so tuning protection sliders while Mode 46 is active produces no visible change by design.

### Fix: Night Scene Gain

- New UI slider `Night Scene Gain` (`night_scene_gain`, default 1.0, range 0.1-10, section Tone Mapping, visible at SettingsMode >= 1).
- Applied as `scene_linear = source.rgb * 0.6 * max(gain, 0.01)` at the top of `0x3E36DA5B`. All downstream paths (`vanilla`, `untonemapped`, `neutral_sdr`, protection) derive from `scene_linear`, so they scale together; wall-vs-light ratios are preserved while the curve shapes the now-in-range values. Mode 46 also uses `scene_linear`, so it doubles as the gain-tuning tool.
- `ShaderInjectData` gained a trailing `float night_scene_gain`; CB upload size grows in lockstep via `sizeof`. No default behavior change (gain=1.0 is identity).
- Default 1.0 keeps daytime unchanged; raise to ~2-4 at night until Mode 46 shows walls in green and lights yellow/orange/red, then set DebugMode to Off.

### Recommended night test

1. Full restart with DebugMode=46, Game=203, Peak=422, SourceWhite=8.
2. Raise Night Scene Gain to ~3; confirm walls lift from deep blue to green/cyan and lights reach yellow/orange/red.
3. Set DebugMode=0 (normal path). Verify night is visible with lights clearly above the surrounding walls, then tune Game/Peak for the final look.
4. If walls become too bright (daytime-feel), lower gain rather than Game, since gain is scene calibration and Game is the paper-white reference.

## 2026-08-10 Debug probe upgrades: Dark Scene + exposure readout, Quick Debug Probe

- Legacy DebugMode 47 = `Dark Scene Absolute Luminance + Exposure Readout`. Continuous palette: values 0-2 map strictly linearly (deep blue -> cyan -> green -> yellow -> orange), values >2 are log2-compressed (orange -> red -> white) so lights do not dominate the palette. The displayed value is the auto-exposed scene `max(scene_linear * exposure)`. A white 5x7 glyph chip in the top-left corner renders the live auto-exposure scalar from t1 (e.g. `E1.5`).
- New Debug-section setting `Quick Debug Probe` (`QuickDebugMode`, index only, own binding so it cannot fight `DebugMode` during preset load). It maps to the useful probes: 1=47 Dark Scene+Exposure, 2=46 Exposed Linear Segments, 3=11 Auto Exposure, 4=10 Source t0 Range, 5=19 Source t0 Chroma, 6=44 Hermite SW8, 7=45 Hermite SW16. Switching it writes `shader_injection.debug_mode` on change.
- Both probes render through `RenderIntermediatePass` so they survive the game's later composite passes.
- Diagnostic use: with Dark Scene + Exposure, night bulk should read dark blue/cyan/green with lights at yellow/orange/red; the corner chip confirms exposure ~0.4 day vs ~1.5-2 night for the eventual auto-exposure-driven day/night adaptive gain.

## 2026-08-10 t1 baseline readback and exposure-magnitude correction

### The glyph chip is not trustworthy for magnitude

The on-screen glyph renders one integer + one decimal digit and clamps the
integer to 0-9. A real value >= 10 therefore prints as `9.x`, and the `E`
label next to the first digit can be misread as a digit: the user read the
night `E8.0`/`E8.1` glyphs as "80"/"81", and day sky also read as `E8.0`.
The value does change with the scene (consistent with auto-exposure), but its
magnitude cannot be read off the glyph.

### Corrected 0x3E exposure understanding

- `t1` is a `1x1` `R32_FLOAT` texture; the shader reads `t1.SampleLevel((0,0)).x` as a single global baseline. Same frame, every pixel gets the same value, so "sky 80 vs ground 81" was a frame-to-frame change, not per-pixel.
- `cb0` (values `[2.27, 0.17, 1.69, 0.8, 0.14]`) is the fixed SDR tonemap curve constants (`ApplyDL2SDRCurve`), identical day/night. It is not the per-pixel auto-exposure.
- The `0x3E Inputs and Curve` audit confirms the same t1 resource and same cb0 values across captures; only the t1 content (invisible to the audit) varies.

### New: t1[0] readback in the 0x3E audit

`Capture 0x3E Inputs and Curve (4 Draws)` now performs a one-shot deferred
readback of the first captured t1 texture: copies to a `gpu_to_cpu` staging
texture via the immediate command list, `flush_immediate_command_list` +
`wait_idle`, maps, and logs `format`, `size`, `raw_bits`, and the decoded
`float`. Guarded by `t1_baseline_readback_logged` (reset on each arming),
runs only when `audit.count >= 1`, and never in the hot path. The glyph chip
remains but is documented as a rough visual; the readback line
`DL2 t1 baseline readback: ... float=...` is authoritative.

### Next: capture day and night readbacks

Click the audit button once in a bright day scene and once at night; compare
the `DL2 t1 baseline readback` `float=` values. That determines whether t1[0]
separates day/night (and by how much) before building the auto-exposure-driven
adaptive gain. Do not tune Night Scene Gain or the protection curve from the
glyph readout.

## 2026-08-15 Architecture decision: return to standard RenoDX path (A1/Y)

### Decision

After testing the DL2-specific Hermite curves and the t1[0]-driven Night Scene
Gain, the user decided to **return to the standard RenoDX tone-mapping path**
and re-tune color from there. DL2's auto-exposure baseline `t1[0]` was measured
(day outdoor `0.1077`, dark interior `1.8`), but boosting the scene with a
uniform gain washed out sunlit highlights, and the RenoDRT Curve / Scaling
controls had no effect because the DL2 normal path bypassed `ToneMapPass`.

### What changed (branch `codex/dl2-hdr-next`, uncommitted at write time)

- `tonemapper_0x3E36DA5B.ps_5_x.hlsl`
  - Removed the `0.6` scene calibration and Night Scene Gain from
    `scene_linear`; it is now `source.rgb`.
  - Normal path now uses `ScaleToneMappedScene(renodx::draw::ToneMapPass(untonemapped, vanilla, neutral_sdr))`, so RenoDRT Curve / Scaling / Game / Peak controls take effect again. Off still outputs `vanilla`.
  - The DL2 Hermite/Anchored/Separated/FixedScene helpers remain **only as
    diagnostic grids** (DebugMode 40-45); they are no longer the normal path.
  - The exposure-protection chain (`adaptive_exposure`, `protected_exposure`,
    `protection_start/end`) is still computed only because diagnostic modes
    reference it; the normal path uses `untonemapped = scene_linear * exposure`.
- `tonemapper_0x268BAB6D.ps_5_x.hlsl`
  - Highlight handling changed from `UpgradeToneMap`/`Chrominance`/`Hue`
    reconstruction (which rewrote highlight hue and caused the washed-out
    look) to a luminance-preserving restore: `o0.rgb = native_lut_grade *
    (y_hdr / y_lut)`. Chroma/hue come straight from the game LUT; only
    luminance is stretched back to the HDR scene value. DebugMode 35 grid
    still exposes native/upgraded/stable for A/B.
- `addon.cpp` / `shared.h`
  - UI sliders that are no longer effective in the normal path are labeled
    with `(X)` and their tooltips state the reason:
    - `Night Scene Gain (X)` (0x3E no longer reads it)
    - `HDR Source White (X)` (only diagnostic Hermite grids use it)
    - `Highlight Exposure Retention/Minimum/Maximum (X)`
    - `HDR Protection Start/End (X)`
  - These are kept, not deleted, pending the paper-white calibration step.

### Key architectural findings (from DL1 comparison)

- DL1 does no scene tonemapper hook; its LUTs do not hard-clamp, so RenoDX TM
  can run at the very end (gamma). DL2's 0x3E is itself the game tonemapper and
  hard-clamps, so DL2 cannot fully copy DL1: the TM must stay at 0x3E, and the
  LUT clamp must be made reversible (record HDR luminance, grade in 0..1, then
  restore by ratio).
- There are **two saturates**, and they are not the same:
  - 0x3E `ApplyDL2SDRCurve` saturate = the true game paper-white (scene 1.0).
  - 0x268 LUT saturate = only a LUT-coordinate requirement (3D LUT lookup needs
    0..1); it kills HDR highlights as a side effect, not by defining paper-white.
- `1.0 = 203 nits` is an unproven assumption (same category as the removed `0.6`).
  The true paper-white needs a calibration step: measure what scene value the
  game actually clamps to, then anchor it against a known-brightness reference.

### Next steps

1. Finish UI cleanup (task #3) and build once (task #4).
2. Paper-white calibration: add a diagnostic showing `scene_linear / 1.0`
   ratio to find where the game paper-white really lands; anchor 203 against a
   known-luminance reference.
3. Decide 0x268 Game exit@2 scaling fate (currently still present; it may be a
   duplicate of the 0x3E standard Game handling).

## 2026-08-15 Later: paper-white anchor investigation (key conclusions)

Long discussion resolved what anchors the HDR paper white. Recorded so the
thread is not lost.

### Data-flow vocabulary (keep precise)

```
raw (scene_linear, unexposed)
  x exposure (auto)  ->  untonemapped (uTM) = raw * exposure
  uTM -> ApplyDL2SDRCurve (vanilla, 0..1)   [SDR reference, clamps at 1.0]
  uTM -> ToneMapPass (HDR, 0..peak)         [RenoDRT]
```

- **Two different "1.0"s**: vanilla's 1.0 (SDR display white, output of
  `ApplyDL2SDRCurve`) vs ToneMapPass output 1.0 (HDR paper white). Do not
  conflate.
- `uTM = 1.275` is the input at which `ApplyDL2SDRCurve` outputs 1.0 (solved
  from cb0 = [2.27, 0.17, 1.69, 0.8, 0.14]: `(a x + b)x / ((c x + d)x + e) = 1`
  => `x = (0.63 + sqrt(0.7217)) / 1.16 ~= 1.275`). It is the SDR display ceiling
  (white wall), NOT the HDR paper white.
- The 0x268 LUT clamps at coordinate 1.0 (white corner). uTM 1.275 reaches
  that corner; anything above is saturate-clamped to white (SDR loses highlight
  steps there). This is the correct "highlight boundary" in uTM.

### Correct anchor model (RenoDRT, code-verified)

```
src/shaders/tonemap/reno_drt.hlsl:
  reference_white = 100.f        // reference white nits
  mid_gray_value  = 0.18f        // scene 18% gray
  mid_gray_nits   = 10.f         // output gray nits
  peak = nits_peak / reference_white
```

- RenoDRT's paper white / peak are derived from the **mid-gray anchor**
  (0.18 scene -> 10 nits; reference white = 100 nits). It is **independent of
  any SDR reference point**. "SDR reference white as paper white" was
  investigated and rejected; the framework does not use SDR values.
- DL2's `diffuse_white_nits` (Game slider, default 203) is the *user paper
  white*; RenoDRT's internal reference white (100) is separate.
- `ScaleToneMappedScene` converts ToneMapPass output (relative to Game) back to
  DL2's fixed 203 unit: `color * (Game / 203)`.

### Open question for DL2

- Whether RenoDRT's mid-gray anchor (0.18 scene) lands correctly in DL2 depends
  on the uTM (raw * exposure) value domain, which is not calibrated yet. The
  real test: measure what uTM value is DL2's scene mid-gray, and what nits it
  produces. This is the paper-white calibration.
- Previous "0.6 calibration" and "203 nits" are both unproven assumptions
  (same category). Do not treat either as authoritative without measurement.

### Next concrete step (measurement)

Add a diagnostic that shows, for a scene, how `uTM` maps to RenoDRT's anchors:
display false-color of `uTM` relative to `mid_gray` (0.18) and relative to the
paper-white boundary (1.275). Then measure a known-mid-gray scene to anchor the
value domain.

## 2026-08-15 Plan B verified: sun highlight now correct

### Verified facts (user-measured, Game=203, Peak=4000, DLSS Off chain)

- Mode 48 (ToneMapPass response ladder, single-arg, opaque bars) with
  Daniele curve: inputs 0.18/0.5/1/2/4/8/16/32 map to outputs
  38/105/200/360/614/958/1335/1666 nit. Monotonic, Daniele's m2 asymptote
  (~8.2x) is the high-input ceiling, not Peak.
- Neutral default curve (Daniele): sun center ~1800 nit. Curves Reinhard/
  Hermite/Neutwo reached nearer Peak at high inputs (Neutwo 4.0 input -> 820
  vs Daniele 614), so high-rolloff differs by curve.
- Resource resolution must match the swapchain: the 1440 render vs 1600
  swapchain caused `upgrade_index=-1` (no FP16 clone) and the 203-nit clamp.
  Fixing the render resolution to match re-enabled the FP16 upgrade.

### Why the sun changed from ~800 to ~1800 (probable: Peak/Game ratio, not plan B)

The sun brightness depends on `Peak/Game`: with Game=500,
`Peak/Game = 4000/500 = 8` -> sun ~800; with Game=203, `Peak/Game =
4000/203 ~= 19.7` -> sun ~1800. Raising Game lowers `Peak/Game` and thus
darkens highlights; this is standard RenoDRT behavior, not a regression.

The 800 measurement was taken on a build that already contained plan B
(`2b76415`), so `2b76415` itself cannot be the cause of the 800->1800
change. The difference is almost certainly the Game setting (500 vs 203),
not a code commit.

**Deferred verification (do not assume):** re-test build `2b76415` with
`Game=500`, `Peak=4000` and confirm the sun reproduces ~800. If it does,
the Game/PK ratio is confirmed as the cause. If it does not, the cause is
still unexplained and needs investigation.


### Paper white and mid-gray both remain to be calibrated

- `203 nits` is still an unproven assumption (proxy hardcodes
  `RENODX_GRAPHICS_WHITE_NITS = 203`). It should be aligned to DL2's real
  paper white via a known-luminance measurement.
- RenoDRT's mid-gray anchor (`0.18 scene -> 10 nits`) is hardcoded. It assumes
  `uTM = raw * exposure` is a standard scene domain, but the exposure multiply
  may shift the value domain so that `uTM 0.18` is not DL2's true mid-gray.
  This must be calibrated: measure what `uTM` value is DL2's scene mid-gray.
- These are precision refinements, not bug fixes; the current HDR is working
  (sun ~1800, highlights preserved, curve/Peak controls effective).

## 2026-08-16 Washed-out LUT grade: NeutralSDR input -> max-channel

### Symptom

DebugMode 35 (0x268 LUT grid): TL/TR/BL (all variants of `native_lut_grade`)
look washed out (desaturated) compared to the original SDR, more so than BR
(the current ToneMapPass output). The whole frame is slightly desaturated, not
just highlights, though mild. The user also reported that Off/Vanilla mode has a
slight "filterless" feel vs the true original SDR.

### Root cause

The HDR-path LUT input used `NeutralSDR(input_hdr)`. RenoDRT's `NeutralSDR` is
deliberately neutral/desaturating, so sampling the game LUT with a
desaturated input produced a washed-out grade. TL/TR/BL being identical
confirmed the loss happens at the LUT sample input, before the ToneMapPass.

### Fix (applied, uncommitted at write time)

`tonemapper_0x268BAB6D.ps_5_x.hlsl` HDR-path LUT sample now divides by
max-channel before encoding into the LUT domain:

```hlsl
const float max_channel = max(max(input_hdr.r, max(input_hdr.g, input_hdr.b)), 1.f);
float3 lut_input = input_hdr / max_channel;
float3 lut_gamma = srgb::EncodeSafe(lut_input);
```

This preserves chroma through the LUT (max-channel division keeps RGB ratios)
while keeping values bounded in 0..1 so the vanilla smoothstep/contrast/
saturation chain stays safe. `neutral_sdr` is still computed and passed to the
three-argument `ToneMapPass` as the neutral reference. Matches the
Silksong/Wobbly max-channel bridge in the SKILL.

### Verify

Rebuild and check DebugMode 35: TL/TR/BL should now keep more saturation and
match the original SDR grade more closely. Confirm the whole frame (not just
highlights) no longer looks washed out, and that HDR highlights still reach
~1800 sun / Peak.

### Correction: max-channel input was wrong - reverted to neutral_sdr

The max-channel change was reverted. It broke the required data relationship
for the three-argument ToneMapPass: `neutral_sdr -> LUT -> graded_sdr` must be
the same reference-space before/after relationship (a pixel's color change
between neutral SDR and the LUT-graded SDR). Feeding `input_hdr / max_channel`
as the LUT input made `neutral_sdr` and `graded_sdr` live in different
reference spaces, which explains the ground/sky inconsistency.

`0xA7F77A42` (the reference) samples the LUT with `neutral_sdr`:

```hlsl
neutral_sdr = NeutralSDR(r2)
if (AUTO_EXPOSURE != 1) r2 = neutral_sdr   // LUT input = neutral_sdr
r1 = lut::Sample(..., r2)                  // graded_sdr
ToneMapPass(untonemapped, r1, neutral_sdr)
```

The washed-out look is therefore likely rooted in `NeutralSDR` itself or in
the `neutral_sdr -> LUT -> graded_sdr` construction, NOT in a max-channel
choice. Next step: inspect `NeutralSDR()` (saturation=1, dechroma=0 should
preserve chroma) and the LUT-domain reconstruction to find where chroma is
lost, then build a proper chroma-preserving SDR baseline (Wobbly Life style
gamut compression) while keeping `neutral_sdr -> LUT -> graded_sdr` intact.

### Do not

- Do not swap LUT input to `input_hdr / max_channel` (breaks neutral/graded
  reference-space relationship).
- Do not blend inputs by luminance thresholds (0.5-1.5 was arbitrary).
- Prefer fixing the `neutral_sdr`/`graded_sdr` construction over band-aid
  input swaps.

## 2026-08-18: isolate the confirmed AD clone-bind crash

- The crash boundary is `0xAD085E81` targeted `t0` descriptor-table clone
  binding, not the `ToneMapPass` formula and not the disabled 0x268 center
  readback.
- The diagnostic build keeps 0x268 `OnTargetDraw` (HDR t0 clone) but changes
  0xAD from `OnTargetDraw` to `OnTargetOutputDraw`. The subsequent runtime log
  still reported `rewrite=1`, proving that AD's output-RTV rewrite remained
  active. That run also used HDR10 and emitted repeated FG present identities,
  so it was not a valid pure-Off crash test.
- The next isolation removes the AD command-action registration entirely:
  `0xAD085E81` remains audit-only, with both t0 and output-RTV rewriting
  disabled. A stable `DebugMode=0`, `ToneMapType=3` launch under the explicit
  pure-Off configuration is then the first valid crash result. Its AD input
  falls back to the native resource and may return to a 203-nit ceiling; do not
  interpret that as a RenoDRT math verdict.
- The `Capture 0x268 Center Probe` button remains readback-disabled because
  its staging/readback path is unsafe. The button currently logs only.

## 2026-08-19: unsafe center-probe readback confirmed

Build `c059dbc` armed the center probe and immediately logged all-zero I/N/L/B,
followed by repeated `DXGI_ERROR_DEVICE_REMOVED` with removal reason
`DXGI_ERROR_DEVICE_HUNG`. The cause was a full-frame copy of the active
2560x1600 FP16 RTV from the Present callback without an explicit resource
state transition.

The diagnostic path was changed to copy only the four DebugMode 60 pixels into
a deferred `4x1` FP16 `gpu_to_cpu` staging resource. It requires the source to
advertise `render_target` usage, transitions
`render_target -> copy_source -> render_target`, signals a D3D12 fence, and
maps only after that fence completes. It leaves all tone mapping,
LUT, RenoDRT, Game, and Peak code untouched. A source without an explicit
render-target usage is skipped rather than guessed. This is diagnostic-only;
do not interpret a missing readback as a rendering result.

## 2026-08-20: restore shader replacements for tone-map A/B

Build `ba9a7b7` intentionally had `kEnableDl2ShaderReplacements=false` for
crash isolation, so Vanilla/RenoDRT changes were inert. The next diagnostic
build restores shader replacements only; FG/Streamline, targeted descriptor
rewrites, and shader-layout hooks remain disabled.

The first replacement-enabled build (`bb5bc58`) crashed during startup after
registering the copied `0xAD085E81` replacement. The next isolation removes
only that replacement; `0xAD` remains audit-only while `0x3E` and `0x268` stay
enabled for Vanilla/RenoDRT testing.

`c9867a8` still crashed after `0xAD` was removed, with the later startup log
reaching the popup UI pipeline. The next isolation keeps only `0x3E` and
`0x268` replacements; UI and BFFC replacements are temporarily disabled so
tone-map testing has no unrelated replacement pipeline in the startup path.

`5742925` still crashed with only `0x3E` and `0x268` registered. The next
split keeps only `0x3E` to determine whether the crash is in the LUT replacement
or already in the upstream tonemapper replacement.
