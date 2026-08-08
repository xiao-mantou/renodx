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

