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

The focused-FG final proxy source is an RGB10/UNORM handoff. The source-range probe reached the red/high bands while the raw-source quadrant changed between focused and unfocused states. This points to a focused-FG handoff representation/semantic mismatch or an RGB10 headroom bottleneck, not a simple exposure or saturation setting.

Do not repeat the four modes above as blind A/B tests. Future tests should change one handoff property at a time: source representation/encoding, pre-RGB10 scaling, or final-proxy resource format.
