# Experimental Four-LUT Candidates

- 64 cache shaders matched four `ColorGradingLUT` samples, `ImageAdjustments2`, and `SceneColorTexture`.
- Their `SceneColorTexture` sampling split is 32 shaders with one sample and 32 with five samples. All ten previously known ImageAdjustments/LUT shaders have the five-sample signature, so the experimental group is narrowed to the remaining 22 five-sample cache candidates.
- These 22 also match four `ColorGradingLUT` samples and contain `ImageAdjustments2`; this is a strong core-structure match, not proof of identical full shader behavior. Their other samplers and pre/post-LUT effects vary.
- The other 32 one-sample candidates are excluded from the replacement group. The ten existing addon sources remain untouched.
- Six of the 22 selected variants had an explicit final RGB `saturate`; their HDR branch now bypasses only that final output clamp, while SDR retains the native clamp. The other 16 variants already had no explicit final RGB saturate after the bridge.
- The new files retain each decompiled shader's pre/post-LUT effects, add the shared curve/LUT bridge, and restore after LUT grading before any post-LUT vignette when present.
- `HDRCurveMethod=3` now bypasses the LUT grade with the shared RenoDX `ToneMapPass` at that same point; later vignette/grain operations remain active. Methods 0-2 retain the existing LUT bridge.
- `LifeIsStrange_FourLutCandidateSource` switches only the 22 selected entries between RenoDX and native bytecode. It defaults to RenoDX replacement; existing hand-corrected shaders and their per-shader controls are unchanged.
- These are cache-derived candidates, not confirmed active HDR-chain members. Existing F8 captures did not contain the 22 selected hashes; producer/consumer resource matching remains outstanding.
- The generated baselines are based on HlslDecompiler output and are experimental, not individually validated native-equivalent translations.
