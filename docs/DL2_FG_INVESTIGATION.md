# Dying Light 2 Frame Generation Investigation

Short working log for the DLSS Frame Generation path. Keep HDR/UI/DLSS SR history in `DL2_HDR_TEMP_RESULTS.md`.

## Current Chain

`0xAD source -> RenoDX prepared HDR tray -> Streamline DLSS-G -> fake swapchain buffers -> RGB10 real backbuffers -> RenoDX final proxy`

The real swapchain resources are RGB10 (`DXGI_FORMAT_R10G10B10A2_UNORM`, 24), while the DLSS-G-facing swapchain reports RGBA8 (`DXGI_FORMAT_R8G8B8A8_UNORM`, 28). Streamline consequently allocates three `sl.dlssg.fake-swapchain-buffer` resources as RGBA8.

## Proven Results

- Present timing is complete: each 0xAD submission reaches the immediately following DLSS-G `Present1`; `skip=1` and `hr=0` are stable.
- 2x FG cadence is visible as two ReShade Presents per source frame.
- Global GPU waits, final-copy waits, exposure compensation, and saturation compensation did not solve the flash and must not be repeated.
- `71fdeea`: creation detour reported installed, but no `DL2 DLSS FG creation format` callback appeared. The fake buffers remained RGBA8 and focused FG still flashed; drag/previous-frame behavior changed only because the existing bridge remained active.

## Current ABI Fix

The Streamline internal before-hook type includes `bool& Skip`, but the plugin-exported `slHookCreateSwapChainForHwnd` function has seven parameters and no `Skip` reference. The addon previously detoured the export with the internal eight-parameter signature. The next build corrects only this ABI and keeps Off/Balanced, bridge, color, and waits unchanged.

Success evidence after a full game restart:

1. RenoDX logs `DL2 DLSS FG creation format: ... requested=28 forwarded=24` before FG resources are allocated.
2. `sl.log` allocates the three `sl.dlssg.fake-swapchain-buffer` resources as RGB10 rather than RGBA8.
3. Focused FG no longer alternates/black-flashes and does not retain previous-frame content.

## Local Formatting

Use `C:\Users\xiaom\Documents\renodx\clang_format\clang_format\data\bin\clang-format.exe` with the repository `.clang-format` file. The launcher under `clang_format\bin` currently fails because its Python package is not on `PYTHONPATH`.
