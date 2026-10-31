# Clive Barker's Hellraiser: Revival

HDR black point and shadow chroma correction for the scene tonemapper
(`0x057BDCB8`, `ps_6_6`).

## What this does

The game runs one `FColorGradingSettings` for both SDR and HDR and only swaps
the output transform. Measured on the shipping build at a 450 nit peak:

| Observation | Value |
| --- | --- |
| `LUT(0)` output | **0.2533 nits** (neutral, `R = G = B`) |
| Shadow chroma on the neutral axis | `R/G = B/G = 0.79` at 1 nit |
| Tonemap toe slope | 0.47 nits/nits (0.106 normalized, matches ACES) |
| Peak | 449.27 nits |

The 0.2533 nits floor is an absolute value, so it reads as a grey veil on an
OLED. The green cast is the game's split-tone grading, which the LUT applies
below ~10 nits.

The addon works on the **LUT output**, before the PQ encode. It does not
replace the tonemapper, so the game's curve and the creator's grading are
preserved.

| Setting | Effect |
| --- | --- |
| Black Point Removal | Subtracts `LUT(0)`. Exactly zero at the floor, curve shape untouched. |
| Shadow Desaturation | Blends the shadow band toward neutral **at constant luminance**. Only acts below *Desaturation Range*, so midtones and highlights are bit-exact. |
| Desaturation Range | Nits at which the desaturation has faded out (default 10). |
| Black Crush | Optional extra nits. Above ~0.05 this starts clipping real shadow detail. |

Film grain is left untouched: it is added in PQ code space and is worth about
one 10-bit PQ code (measured: on/off moves the frame mean by 0.08%), i.e.
invisible, so it does not warrant a slider.

HDR only. The SDR path (`cb0_008w <= 2`) is untouched.

## Verification

1. Build the target `hellraiser` (`renodx-hellraiser.addon64`).
2. Load the addon, enable HDR output in the game (`HDRDisplayOutputNits` in
   `GameUserSettings.ini`), then open the ReShade overlay.
3. Set **Black Point Removal = 0** and read the `min` value from the ReShade
   HDR Analysis overlay on a dark frame; it should be ~0.25 nits above zero.
4. Set **Black Point Removal = 1**; the same frame should read ~0.
5. Sweep **Shadow Desaturation** 0 → 1 and confirm `max` never changes.
