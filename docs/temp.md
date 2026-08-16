# DL2 HDR 工作临时笔记 (temp)

## 当前架构 (方案 B, max-channel 版)

- 分支: codex/dl2-hdr-next
- 未提交改动: 0x268 LUT 输入从 NeutralSDR 改为 max-channel 保色度压缩

### 完整数据流 (0x268 HDR 路径)

```
input_hdr (untonemapped, 0x3E 输出, HDR >1)
  → sdr_scale = ComputeMaxChannelScale(input_hdr)  // = Neutwo(max)/max, max==0 safe
  → neutral_sdr = input_hdr * sdr_scale            // 0..1, 保 R:G:B (色度)
  → EncodeSafe(neutral_sdr) → 游戏 LUT 采样 → DecodeSafe
  → 原生调色链: 色温 → 亮度平衡 → smoothstep 对比度 → 饱和度 → native_lut_grade (0..1)
  → upgraded_grade = native_lut_grade / max(sdr_scale, 0.001)   // Reconstruct 恢复 HDR
  → ToneMapPass(input_hdr, upgraded_grade, neutral_sdr)
```

### 关键决策 (已确认)

- **必须保持** `neutral_sdr → LUT → graded_sdr` 同参考空间前后关系
- **禁止** 用 `input_hdr/max_channel` 直接当 LUT 输入 (会打破 neutral/graded 关系)
- **禁止** 亮度阈值混合 (0.5-1.5 拍脑袋)
- **先不做** Adaptive D65 / Gamma Gamut / CUSTOM_SDR_GAMUT_COMPRESSION
- Reconstruct 必须接在原生调色链之后 (÷sdr_scale), 不是只换 LUT 输入

### 待验证 (构建后)

1. DebugMode 35: TL/TR/BL 是否保留色度 (地面/天空褪色是否消除)
2. HDR 高光仍正常 (~1800 太阳)
3. 观察: 原生亮度平衡 (line 130-138) 在压缩后 neutral_sdr 下行为是否正常

### 35 实测 (74e2d98)

- 天空: TL(0..1 native) 灰暗, BR(ToneMapPass) 明亮层次强 -> ToneMapPass 用 input_hdr 重建正确
- 地面: 四象限区别不大 -> 地面 max<=1, sdr_scale~1, 压缩无副作用 (保色度验证)
- 待确认: BR 相对原版 SDR 天空是否仍褪色 (决定褪色在 LUT 输入还是原生调色链/TM 之后)

## 关键发现 (2026-08-16): 0.6 是游戏原生常数

dump 目录 `E:\...\renodx-dev\dump\0x3E36DA5B.ps_5_0.hlsl` 是游戏原生反汇编:

```hlsl
r0.x = t1.Sample(0,0).x                       // exposure
r1.xyz = t0.Sample(v1).xyz                    // source
r0.xyz = r1.xyz * r0.xxx                       // source * exposure
r0.xyz = float3(0.6,0.6,0.6) * r0.xyz          // <-- 0.6 游戏原生!
... ApplyDL2SDRCurve ...
```

**0.6 是 DL2 原生 0x3E 的常数,不是 RenoDX 假设。** 移除它导致 scene 放大 1.67x -> 过曝/雾感/不生动(用户实测: 原版 SDR 生动, 我们抬白雾感).

**必须恢复**: `scene_linear = source.rgb * 0.6` (0x3E), 使 vanilla 和 untonemapped 都恢复校准.




### 已验证事实 (勿改)

- 203 nit = proxy 硬编码假设, 未校准 (Game=203 时 ToneMapPass 输出相对=203单位)
- RenoDRT 中灰锚点: mid_gray 0.18 → 10 nit, 硬编码
- Neutwo(x) 低值≈identity, scale≈1 → neutral_sdr≈原色 (保色度)
- 0xA7F77A42 是参考: LUT 输入用 neutral_sdr, 三参数 ToneMapPass
- 资源分辨率必须匹配 swapchain (1440 vs 1600 导致 FP16 升级失败 → 203 clamp)

### 命令

- 构建: push 触发 GitHub Actions
- 测试: ResourceUpgradeTest=0 (DLSS Off), SwapChainFormat=1 (scRGB)
