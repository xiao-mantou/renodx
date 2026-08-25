# DL2 performance A/B handoff

- Baseline: no addon ~68 FPS; addon ~63 FPS, with worse motion pacing.
- CPU observers disabled: movement became more stable; keep
  `kEnableDl2CpuObservers=false`.
- Proxy disabled: clearly smoother, but output color is invalid; not a fix.
- Generic `state::Use=false`: no clear extra gain; keep state tracking on.
- scRGB gamut-compression bypass: no meaningful gain; reverted.
- Proxy copy-only (commit `5545ed9`): still ~63 FPS; full-size clone/handoff
  cost remains. Copy-only branch was disabled and normal Proxy restored.
- Barrier fast path (`e667344`): semantics unchanged; latest retained
  performance optimization. A half-FPS difference is within measurement noise
  unless repeated consistently.

Next performance boundary, only if needed: isolate clone/resource synchronization
without changing HDR math. Otherwise leave performance here and continue HDR
brightness/calibration.
