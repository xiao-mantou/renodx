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
