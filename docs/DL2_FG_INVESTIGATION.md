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

## Runtime `0adad73`

The seven-parameter ABI correction still produced no `DL2 DLSS FG creation format` callback. Streamline continued allocating all three `sl.dlssg.fake-swapchain-buffer` resources as RGBA8, so the exported-function detour does not control the cached/active creation path and must not be extended further.

## Creation Contract Audit (pending runtime)

Prior logs prove only that the FG-facing proxy query reports RGBA8 and DLSS-G allocates RGBA8 fake buffers. They do **not** prove the engine's original creation request, nor the actual native swapchain/backbuffer format at Streamline creation time.

The dedicated diagnostic interposer now records, without mutation:

1. `ENGINE_REQUEST`: the engine `DXGI_SWAP_CHAIN_DESC1` at the Streamline entry.
2. `POST_PLUGIN_HOOKS`: the local descriptor after all Streamline plugin before-hooks.
3. `NATIVE_AFTER_DXGI_CREATE`: native `GetDesc1`, `GetColorSpace1`, and every `GetBuffer()->ID3D12Resource::GetDesc().Format`.
4. `SET_COLOR_SPACE`: later native color-space changes.

Interpret only the native backbuffer resource formats as actual containers. If they are RGB10 with HDR10/BT.2100 while fake buffers remain RGBA8, the remaining RGBA8 choice is inside the closed DLSS-G plugin. If native resources start RGBA8, a creation/resize boundary remains the only theoretical HDR-FG repair point.

Mode 11 is now rejected as a production fix. The log proves it intercepts Streamline's `RGBA8 -> RGB10 m_pDLFGOutputs` copy but substitutes a separately prepared 0xAD source frame instead of converting Streamline's generated frame. This explains all three visible failures together:

- flash: 11 initial submissions used `bridge_ready=0`, then switched to the replacement path;
- color: the replacement assumes `linear BT.709 -> PQ BT.2020`, independent of Streamline's generated result;
- drag/judder: generated frames are discarded and replaced by repeated/misaligned source frames.

The next valid direction is pre-generation semantics: intercept `slGetNativeInterface` for the swapchain and return a DL2-local wrapper whose `GetDesc`/`GetDesc1` report the actual RGB10 HDR10 backbuffer format. Test with `DLSS FG Final Color = Direct PQ`; mode 11 must remain disabled. Success means Streamline itself allocates/generates RGB10 frames, preserving its motion output rather than replacing it afterward.

The wrapper owns one COM reference to the underlying swapchain and now has its own reference count. Its final `Release` deletes the wrapper and the virtual destructor releases the underlying object; repeated `slGetNativeInterface` calls therefore do not leak wrapper instances.

## Local Formatting

Use `C:\Users\xiaom\Documents\renodx\clang_format\clang_format\data\bin\clang-format.exe` with the repository `.clang-format` file. The launcher under `clang_format\bin` currently fails because its Python package is not on `PYTHONPATH`.
