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
| Black Level Fix (On/Off) | Subtracts `LUT(0)`, measured from the LUT itself. Exactly zero at the floor, curve shape untouched. |
| Extra Black Level | Manual black level in nits, applied after the fix. Above ~0.05 this starts clipping real shadow detail. |

Both are the same affine operator, `out = (in - k) * W / (W - k)`: a constant
subtraction followed by a rescale that keeps the white point bit-exact. The
only difference is where `k` comes from - the LUT for the fix, the slider for
the extra level.

There is deliberately no chroma control. The shadow cast is a level-dependent
split-tone (`R/G = B/G = 0.79` at 1 nit, `B/G = 1.33` at 92 nits, neutral above
250 nits), i.e. it *is* the grading rather than an artifact, and this game's
frame mean is only ~2 nits, so any luminance threshold that covers the cast
also covers most of the frame. Neutralising it is a look change, not a fix.

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
