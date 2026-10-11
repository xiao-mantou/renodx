# Experimental Four-LUT Candidates

- 64 cache shaders matched four `ColorGradingLUT` samples, `ImageAdjustments2`, and `SceneColorTexture`.
- Ten already had addon sources and were left untouched; 54 new experimental replacements were added as `four_lut_0xHASH.ps_3_0.hlsl`.
- The new files retain each decompiled shader's pre/post-LUT effects, add the shared curve/LUT bridge, and restore after LUT grading before any post-LUT vignette when present.
- `LifeIsStrange_FourLutCandidateSource` switches only the 54 new entries between RenoDX and native bytecode. It defaults to RenoDX replacement; existing hand-corrected shaders and their per-shader controls are unchanged.
- These are cache-derived candidates, not confirmed active HDR-chain members. Existing F8 captures did not contain the 54 new hashes; producer/consumer resource matching remains outstanding.
- The generated baselines are based on HlslDecompiler output and are experimental, not individually validated native-equivalent translations.
