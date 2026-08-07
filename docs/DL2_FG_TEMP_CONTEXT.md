# DL2 FG Temporary Context

This is a compact handoff note for the DLSS Frame Generation investigation. The stable Off/Balanced HDR work is separate and should not reuse the FG bridge experiments.

## Known chain

`0xAD game target -> RenoDX FP16 clone/HDR preparation -> Streamline DLSS-G input -> DLSS-G generated output -> RGB10 final output -> RenoDX proxy`

The real DL2 backbuffers are `DXGI_FORMAT_R10G10B10A2_UNORM` (24). The Streamline-facing swapchain reports `DXGI_FORMAT_R8G8B8A8_UNORM` (28). Streamline consequently creates three `sl.dlssg.fake-swapchain-buffer` resources as `eFormatRGBA8UN`.

## Proven facts

- DLSS-G Present timing is not the root cause: the 0xAD submission is consumed by the next `Present1`, with successful returns and stable 2x cadence.
- Auxiliary `UIColorAndAlpha`/`HUDLessColor` tags do not explain FG color, flash, 203-nit caps, or drag artifacts.
- Global waits, final-copy waits, exposure compensation, and saturation compensation did not solve FG and must not be repeated.
- The mode-11 bridge (`AD FP16 -> PQ RGB10 Handoff`) replaces Streamline's generated image with a prepared 0xAD frame. It causes dark/overdeep color, flashes, and repeated-frame drag. Keep it disabled for production.
- Direct PQ no longer flashed as badly, but focused FG remained dark because Streamline still received the mismatched RGBA8 contract.

## Current code/test status

- `cc52bf9` hooks `slGetNativeInterface` and wraps a matching swapchain so `GetDesc/GetDesc1` report RGB10.
- The wrapper now owns its COM reference count and releases the underlying swapchain correctly.
- Runtime logs show `slGetNativeInterface` was installed, but no `native swapchain contract: wrapped=1` line appeared. Therefore the callback did not receive the relevant swapchain before Streamline allocation.
- `sl.log` still shows `sl.dlssg.fake-swapchain-buffer` as `eFormatRGBA8UN`; RGB10 allocations with other names are not proof of success.
- The native-contract direction remains a code/timing issue, not an Off/Balanced color fix.

## Required FG baseline

- `DLSS Balanced`, FG On
- `DLSS FG Final Color = Direct PQ` (mode 0)
- `DLSS FG RGB10 Native Contract = On`, restart required
- `DLSS FG Declared Final Color Format = Game default (0)`
- Auxiliary tags, tagged clone, pre-PQ suppression, skip-generated-proxy, bypass-all-proxy, and mode 11 handoff disabled

## Important correction

`ResourceUpgradeTest=33` is **Exact Balanced + FG chain 2 + 3**, logged as `mask=0xc`; it is not the `4 + 5 + 7` chain. The current correct Off/Balanced test is the UI entry `Typeless 4 + 5 + 7 + UNORM/sRGB`, internal value 29, logged as `mask=0xb0`.

Historical stable baselines are also recorded in `DL2_LIVE_SHADER_LOG.md`: Off uses startup mode `0` (exact `4+5+7`) and Balanced with FG disabled uses startup mode `32` (exact `0+1`). These are restart-time resource presets, not live per-mode switches. Mode 29 currently produces the same `mask=0xb0` as mode 0, but mode 0 remains the canonical Off baseline.

## Next work boundary

First restore and validate Off/Balanced HDR brightness and UI using `ResourceUpgradeTest=29`, without changing FG bridge code. Only after that should the Streamline native-interface timing be revisited.
