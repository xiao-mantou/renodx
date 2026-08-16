# DL2 HDR 交接笔记 (2026-08-16 更新, 精简可指挥后续)

## 当前架构 (codex/dl2-hdr-next, HEAD=48fe4d2)

```
0x3E36DA5B (替换 tonemapper):
  scene_linear = source.rgb * 0.6          <-- 0.6 是游戏原生常数(已从 dump 确认)
  untonemapped = scene_linear * exposure   <-- HDR 路径输出 (含 0.6)
  vanilla = ApplyDL2SDRCurve(game_exposed) <-- SDR 参考 (含 0.6)
  Off(Vanilla=0) 输出 vanilla, 否则输出 untonemapped
  ↓
0x268BAB6D (世界 LUT):
  neutral_sdr = NeutralSDR(input_hdr)      <-- 参考做法(0xA7F77A42)
  LUT 输入 = neutral_sdr (编码进 sRGB 域采样)
  原生调色链: 色温→亮度平衡→smoothstep→饱和度 → native_lut_grade (0..1)
  ToneMapPass(input_hdr, native_lut_grade, neutral_sdr)   <-- 三参数
  ↓
0xAD085E81 (gamma, power) → proxy (×203)
```

## 已验证事实

- **0.6 是游戏原生常数**: dump 0x3E36DA5B.ps_5_0.hlsl 反汇编 `source*exposure*0.6`。移除导致 scene 放大 1.67x → 过曝/雾感。已恢复。
- **原生 dump 位置**: `E:\...\renodx-dev\dump\` (0x3E/0x268/0xAD 都是纯净原生,无 RenoDX 注入)
- **原生 0x268 调色链**: 和我们替换版逐位一致 (色温/LUT/亮度平衡/smoothstep/饱和度/遮罩)
- **RenoDRT 曲线区分**: None(1)/ACES(2)/RenoDRT(3) 在 ToneMapPass 内部区分 (type 分支)。None 无 rolloff 不裁切, RenoDRT 丹尼尔曲线 clamp 到 peak。Mode 48 曲线看起来一样是低中段差异小, 但 Peak 裁切行为不同(正常)。
- **0.6 同时进 vanilla 和 untonemapped**(都从 scene_linear 派生)✓
- **ToneMapPass 用 Peak/Game 相对比值, 0.6 被曲线归一化**: 所以 vanilla 明显受 0.6 影响, RenoDRT 变化不大(0.6 效果被 ToneMapPass 消化)

## 当前问题 (核心)

**RenoDRT vs 原版 SDR 差距不小** (用户反馈):
- 建筑物部分: 颜色扁平偏淡
- 深色"阴影"层次少, 像蒙了 fog 滤镜
- 高光部分暂不看(那是 HDR 扩展, 正常)
- 非褪色问题 (用户澄清)

**已排除**:
- 不是 0.6 没进 renodrt (0.6 已进, 被 ToneMapPass 归一化)
- 不是 NeutralSDR 去饱和 (0.6 修复后 vanilla 好了, NeutralSDR 是参考做法)
- max-channel + Reconstruct 已回退 (48fe4d2, 矫枉过正)

**可能原因**:
- RenoDRT 曲线的阴影/对比处理与原版 SDR 曲线本质不同 (HDR 重建特性)
- 用户试调: 降 Shadows 可以但颜色不对; 加 Contrast 但阴影太低出白色缺块; 加 Saturation 可行
- 用户质疑: "真的应该手动加饱和度调回去吗?" (SKILL 原则: 不因 HDR 加饱和, 应默认接近原版)

## 下一步方向 (待讨论)

1. **确认 RenoDRT 阴影/深色差距的根源**: 是 RenoDRT 曲线固有, 还是 0x268 桥/中间格式问题
2. **参考 0xA7F77A42 的 shadows/mid_gray 配置**: 对比它怎么处理低亮度
3. **考虑**: 是否 0x268 的 ToneMapPass 应该传不同 shadows/contrast 配置 (而非默认 1.0)
4. 参考 SKILL line 152: "Keep vanilla SDR appearance as default reference" —— 如果 RenoDRT 让深色偏离原版, 可能需要对齐

## 关键发现 (48fe4d2, 检查中)

**0x268 LUT 输入差异**:
- Vanilla 路径 (line 97): LUT 输入 = r1 = **input_hdr (原始)**
- HDR 路径 (line 115): LUT 输入 = **neutral_sdr = NeutralSDR(input_hdr)**

两个路径 LUT 采样输入不同 -> HDR 的 LUT 调色和 vanilla/原生不同 -> 色调偏离.

**需确认**: NeutralSDR(BT709 曲线, peak=100, mid_gray 0.18->10 nit)是否改变色调(不只压缩值域)? 若改变, HDR 的 LUT 输入应改用和 vanilla 相同的原始 input_hdr, 还是保持 neutral_sdr (参考 0xA7F77A42)?

### 2026-08-16 LUT 采样对比 (关键)

- **0xA7F77A42 参考**: 用 `renodx::lut::Sample` (处理 >1, tetrahedral, sign)
- **我们 + 原生 0x268**: 手动 `EncodeSafe -> SampleLevel -> DecodeSafe` (sRGB 编码采样)
- **确认**: 原生 0x268 dump 也是手动 sRGB 编码 + Sample, 我们忠实复现. 采样方式不是问题.
- **0xA7F77A42 差异**: 它的 LUT 输入有条件 `if (CUSTOM_AUTO_EXPOSURE != 1) r2 = neutral_sdr`, 即 AUTO_EXPOSURE=1 时用原始场景. 我们无条件用 neutral_sdr.
- **NeutralSDR BT709 曲线**: saturation=1, dechroma=0, Daniele 用亮度比缩放 (保色度) -> 理论上不改变色调.

**待查**: NeutralSDR 的 mid_gray 锚点 (0.18->10nit) 对 DL2 值域是否合适. 若 DL2 中灰不是 0.18, NeutralSDR 会压平阴影/改变色调.

**线索**: 用户观察到 "色调本身就不对" (深色扁平, 类似 fog). 这可能 = NeutralSDR 对 DL2 阴影的处理和原版 SDR 曲线不同.

### 2026-08-16 NeutralSDR 来源决策 (关键)

- `ApplyDL2SDRCurve` 含 `saturate` (line 267) -> 输出 clamp 到 0..1, 会 clip HDR 高光
- **不能用 vanilla 输出当 neutral_sdr** (会 clip, 违反 SKILL)
- **SKILL line 115/120 允许 "analytic vanilla tonemap"**: 即 DL2 曲线 `(a*x+b)*x/((c*x+d)*x+e)` **去掉 saturate 的 analytic 形式**
- 这是非裁切形式, 保留 >1, 符合 SKILL

**下一步 (AI 建议)**: 用 DL2 vanilla 曲线的 analytic 形式 (无 saturate) 构造 neutral_sdr, 替代 NeutralSDR(BT709). 这更符合 DL2 原生色调.

**不动**: max-channel / Reconstruct (已回退), 只改 neutral_sdr 来源.

### 2026-08-16 稳定基线 (5cb6a27)

- **已回退到 48fe4d2**: NeutralSDR + 0.6 (用户确认 addon 手动下载正常)
- **ApplyDL2CurveNoClip 实验失败**: 输出 >1 破坏 LUT bridge (log2/sRGB/LUT), 太阳中心黑 + 203 限制. 已回退.
- **教训**: 换 neutral_sdr 实现前, 必须确认和 LUT bridge 值域契合 (>1 会破坏)
- **当前稳定**: 0x3E=0.6, 0x268=NeutralSDR + 三参数 ToneMapPass, 无 max-channel/Reconstruct

### 用户需求 (关键, 当前目标)

- **HDR 低亮度/中调必须接近 vanilla**, 不能有重 fog 感
- 高光部分可不同 (HDR 扩展正常)
- 现在 HDR vs vanilla 低亮度差异大 (fog 感重), 不可接受
- 目标: "至少 SDR 亮度部分和 vanilla 差不多"

**待查**: 
- NeutralSDR 对低亮度(0..1)的行为: 是否在低亮度也偏离 input_hdr, 导致 LUT 调色和 vanilla 不同
- HDR 路径 native_lut_grade 是否应 = vanilla 的低亮度 (只扩展高光)

### 2026-08-16 NeutralSDR 低亮度行为 (决定性)

**模拟 NeutralSDR(Daniele n=100, mid_gray 0.18->18) 输出**:
```
input    NeutralSDR   比值
0.18     0.180       1.00   <- 中灰对齐
0.5      0.379       0.76   压暗
1.0      0.551       0.55   压暗近半
2.0      0.712       0.36
8.0      0.912       0.11
```

**根因**: NeutralSDR 的 Daniele 曲线把 0.18 以上压缩. 
- Vanilla LUT 输入 = input_hdr (1.0 时是 1.0)
- HDR LUT 输入 = NeutralSDR (1.0 时只有 0.55)
- 同像素 LUT 采样位置不同 -> 调色不同 -> 中调/低亮度偏移 -> fog

**fog 感 = NeutralSDR 压缩了 DL2 的中高调** (它的 mid_gray 锚点 0.18 和 DL2 值域不匹配)

**解法方向**: 需要 "低亮度≈identity(和 vanilla 一致)、高亮度才 roll-off" 的 neutral proxy (SKILL 允许 smooth clamp / proven vanilla bounded signal). ApplyDL2CurveNoClip 失败(>1 破坏 LUT), 需设计低亮度保原值、高亮度压缩的曲线.

### 临时实验 (uncommitted): neutral_sdr = min(input_hdr, 1.0)

**目的**: 一次只验证一个假设 - fog 是否单纯来自 NeutralSDR 对 <=1 区域的压缩.
- <=1: identity (和 vanilla LUT 输入一致) -> 若 fog 消失, 证明根因
- >1: clamp 到 1.0 (故意粗暴, 只隔离 <=1 假设)

**注意**: 这是临时实验, 不是最终方案. 
- 若 fog 明显消失 -> 设计真正的 >1 smooth roll-off (低亮度 identity + 高亮度压缩)
- 若 fog 仍在 -> NeutralSDR 不是唯一原因, 需查别处

**1.0 是否是 DL2 Paper White 未证明**: 不能因它是 0-1 边界就认定. 本实验只验证 <=1 identity 是否消除 fog.







## 构建/测试命令

- 构建: push 触发 Actions
- 测试: ResourceUpgradeTest=0 (DLSS Off), SwapChainFormat=1 (scRGB), Game=203, Peak=4000
- DebugMode 35: 0x268 LUT grid 四象限 (TL native/TR upgraded/BL stable/BR current)
- Mode 48: ToneMapPass 响应梯 (0.18-32 in, 单参数)

## 文档

- 详细 handoff: docs/dyinglight2_hdr_handoff.md
- 本 temp: 精简可指挥 (优先读这个恢复上下文)
