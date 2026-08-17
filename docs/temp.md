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

## 当前问题 (核心)**RenoDRT vs 原版 SDR 差距不小** (用户反馈):
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

### 2026-08-17 根本发现: 0x268 input_hdr 域不一致 (方案 B 架构问题)

**min 实验失败**: neutral_sdr=min(input_hdr,1.0) 没消除 fog. 这否定了 "NeutralSDR 压缩 <=1" 是唯一原因.

**真正根因**: 0x3E 输出两种模式不同:
- vanilla 模式: 输出 `vanilla` (SDR 曲线结果, 0..1)
- HDR 模式: 输出 `untonemapped` (线性 HDR, 含 0.6+曝光, >1)

所以 0x268 的 input_hdr:
- vanilla: = vanilla (SDR 曲线结果)
- HDR: = untonemapped (线性曝光值)

**即使 min(input_hdr,1.0), HDR 的 LUT 输入仍是线性曝光值, vanilla 是 SDR 曲线结果 -> 域不同 -> LUT 调色不一致 -> fog**

**0xA7F77A42 无此问题**: 它自己采样 t0 并自己算 vanilla 曲线 (line 59-75), 不依赖 0x3E 输出.

**方向**:
- A. 0x268 自己算 vanilla 曲线 (像 0xA7F77A42), LUT 用 vanilla, HDR 用 untonemapped
- B. 0x3E 同时输出 vanilla 和 untonemapped (0x268 都用)

### 2026-08-17 决定性发现: 0x268 的 neutral_sdr 曲线系数用错了 cb0

**问题**: 0x268 写 `ApplyDL2SDRCurve(input_hdr, cb0[0], cb0[1])`, 但 0x268 的 cb0 布局和 0x3E **完全不同**:
- 0x3E 的 cb0 = SDR 曲线系数 (a,b,c,d,e) = `[2.27, 0.17, 1.69, 0.8, 0.14]` (audit 抓取, 全天候稳定)
- 0x268 的 cb0 = 渐晕阈值/门控 (cb0[0].xyz) + smoothstep 对比度权重 (cb0[0].w) + 饱和度 (cb0[1].x) + 色温 (cb0[1].y)

所以 neutral_sdr 把渐晕阈值当 a/b、门控当 c、对比度当 d、饱和度当 e → **不是 vanilla** → HDR LUT 采样位置偏离原版 → fog。这是 6997664 (sRGB 编码匹配) 之后仍然残留的 fog 根源。

**修复 (已改)**: 0x268 硬编码 audit 常量:
```hlsl
const float4 sdr_curve0 = float4(2.27, 0.17, 1.69, 0.8);
const float4 sdr_curve1 = float4(0.14, 0.0, 0.0, 0.0);
const float3 neutral_sdr = ApplyDL2SDRCurve(input_hdr, sdr_curve0, sdr_curve1);
```
验证: x=1.275 → ≈1.0 (纸白), x=1.0 → 0.928, 与交接文档已知行为一致。

**配合 6997664 后 HDR LUT 输入应完全 = vanilla**: neutral_sdr=vanilla (输入值一致) + 手写 sRGB 编码 (坐标一致) → LUT 采样一致 → native_lut_grade 一致 → 低/中调 fog 应消失。

**待验证**: 用户测试 DebugMode 35 的 TL (HDR 正常路径) vs vanilla 模式应一致。

### 2026-08-17 DebugMode 59 探针修复 (BL 模式不变性缺陷)

**原探针问题**: mode 59 的 BL 象限标注为 "neutral_sdr (mode-invariant reference)", 但实际**不是模式不变的**:
- 两种模式下 input_hdr 含义不同: Off 时 0x3E 输出 vanilla(SDR 曲线结果), HDR 时输出 untonemapped(线性)
- 所以 Off 下 neutral_sdr = curve(vanilla) 二次压曲线(偏暗), HDR 下 neutral_sdr = curve(untonemapped) = vanilla(单次)
- 亮天空: BL(Off)≈curve(1.0)=0.928 vs BL(HDR)≈1.0 → 用户看到 "切换只有亮度区别 + 全屏白"

**修复 (待提交)**:
1. 0x3E mode 59: 两种 ToneMapType 都输出 untonemapped (绕过 vanilla 分支)
2. 0x268 mode 59: 强制 r1=neutral_sdr (即使 Off), 让 LUT 采样/调色链从同一 neutral_sdr 出 native_lut_grade
3. 新四象限 (TL/TR/BL 模式不变, BR 才是模式相关):
   - TL = input_hdr (0x268 实际收到的原始 untonemapped, 接线检查)
   - TR = neutral_sdr (= vanilla, HDR 的 LUT 输入)
   - BL = native_lut_grade (vanilla 参考的 LUT 调色结果)
   - BR = 当前正常输出 (HDR=ToneMapPass, Off=LUT grade)
4. addon.cpp 59 标签更新: "TL raw input, TR vanilla, BL LUT grade, BR current"

**测试法**: 不动相机, 切 HDR/vanilla, TL/TR/BL 必须逐像素一致 (变了=曝光漂移或 0x268 收到的不是 untonemapped)。TL vs TR 看曲线是否作用在收到的输入上; TR 对照真实 vanilla 画面确认 neutral_sdr==vanilla。

## 构建/测试命令

- 构建: push 触发 Actions
- 测试: ResourceUpgradeTest=0 (DLSS Off), SwapChainFormat=1 (scRGB), Game=203, Peak=4000
- DebugMode 35: 0x268 LUT grid 四象限 (TL native/TR upgraded/BL stable/BR current)
- Mode 48: ToneMapPass 响应梯 (0.18-32 in, 单参数)

## 文档

- 详细 handoff: docs/dyinglight2_hdr_handoff.md
- 本 temp: 精简可指挥 (优先读这个恢复上下文)
