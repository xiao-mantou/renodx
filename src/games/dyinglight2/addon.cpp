/*
 * Copyright (C) 2025 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#include <array>
#include <mutex>
#include <sstream>

#include <embed/shaders.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

// Disabled by default. When armed from the Advanced diagnostic setting, this
// records graphics and compute shader hashes after the known late Gamma pass,
// ending at the very next Present.
// It does not inspect resources, descriptor tables, or command history.
float downstream_draw_capture = 0.f;

struct DownstreamDrawCaptureState {
  reshade::api::command_list* command_list = nullptr;
  std::array<uint32_t, 16> hashes = {};
  std::array<bool, 16> is_compute = {};
  uint32_t count = 0u;
  bool active = false;
  bool consumed = false;
};

DownstreamDrawCaptureState downstream_draw_capture_state = {};
std::mutex downstream_draw_capture_mutex;

inline constexpr auto OnDownstreamDrawCapture = []<typename Context>(Context& context)
    -> renodx::utils::command_action::CallbackResult<Context> {
  auto& capture = downstream_draw_capture_state;
  std::scoped_lock lock(downstream_draw_capture_mutex);
  if (downstream_draw_capture < 0.5f) {
    capture = {};
    return {};
  }
  if (capture.consumed) return {};

  auto* shader_state = renodx::utils::command_action::GetShaderState(&context);
  if (shader_state == nullptr) return {};
  const bool is_compute = context.IsDispatch();
  const uint32_t shader_hash = renodx::utils::shader::GetCurrentShaderHash(
      shader_state, is_compute ? renodx::utils::shader::COMPUTE_INDEX
                               : renodx::utils::shader::PIXEL_INDEX);

  if (!capture.active) {
    if (!is_compute && shader_hash == 0xAD085E81u) {
      capture.command_list = context.cmd_list;
      capture.count = 0u;
      capture.active = true;
    }
    return {};
  }

  // Command lists may be recorded on several threads. Keep the capture on
  // the exact list where the Gamma draw occurred, so unrelated UI/worker
  // draws cannot pollute the order.
  if (capture.command_list != context.cmd_list) return {};
  if (shader_hash == 0u) return {};

  if (capture.count < capture.hashes.size()) {
    capture.hashes[capture.count] = shader_hash;
    capture.is_compute[capture.count] = is_compute;
    ++capture.count;
  }
  return {};
};

void OnDownstreamDrawCapturePresent(
    reshade::api::command_queue*,
    reshade::api::swapchain*,
    const reshade::api::rect*,
    const reshade::api::rect*,
    uint32_t,
    const reshade::api::rect*) {
  std::scoped_lock lock(downstream_draw_capture_mutex);
  auto& capture = downstream_draw_capture_state;
  if (!capture.active) return;

  std::stringstream stream;
  stream << "DL2 same-frame draw order after 0xAD085E81 (" << capture.count << "):";
  for (uint32_t index = 0u; index < capture.count; ++index) {
    stream << " " << (capture.is_compute[index] ? "CS" : "PS") << ":0x"
           << std::hex << std::uppercase << capture.hashes[index];
  }
  reshade::log::message(reshade::log::level::info, stream.str().c_str());

  // One capture per manual arm. This leaves no per-frame work afterwards and
  // prevents draw recording from leaking into the next frame's G-buffer.
  downstream_draw_capture = 0.f;
  renodx::utils::settings::UpdateSetting("CaptureDownstreamDraws", 0.f);
  capture.active = false;
  capture.consumed = true;
}

renodx::mods::shader::CustomShaders custom_shaders = {
    CustomDirectXShaders(0x3E36DA5B),
    // A later LUT/color-grade composite. Its normal path below remains the
    // original game code; it is registered only to expose a late-output probe.
    CustomDirectXShaders(0x268BAB6D),
    // A visible gamma/power pass after the LUT composite. Its debug branch
    // isolates whether it remaps HDR highlights after the scene bridge.
    CustomDirectXShaders(0xAD085E81),
    // Disabled: guessed hashes caused crashes because the copied tonemapper
    // template shader has mismatched inputs/outputs.
    // CustomDirectXShaders(0x4d2b3f4d),
    // CustomDirectXShaders(0x8a1c8855),
    // CustomDirectXShaders(0x79b3c079),
    // CustomDirectXShaders(0xa766966e),
};

ShaderInjectData shader_injection;

float current_settings_mode = 0;

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .key = "SettingsMode",
        .binding = &current_settings_mode,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = false,
        .label = "Settings Mode",
        .labels = {"Simple", "Intermediate", "Advanced"},
        .is_global = true,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapType",
        .binding = &shader_injection.tone_map_type,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 3.f,
        .can_reset = true,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Sets the tone mapper type",
        .labels = {"Vanilla", "None", "ACES", "RenoDRT"},
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapPeakNits",
        .binding = &shader_injection.peak_white_nits,
        .default_value = 1000.f,
        .can_reset = false,
        .label = "Peak Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of peak white in nits",
        .min = 48.f,
        .max = 4000.f,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapGameNits",
        .binding = &shader_injection.diffuse_white_nits,
        .default_value = 203.f,
        .label = "Game Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of 100% white in nits",
        .min = 48.f,
        .max = 500.f,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapWhiteClip",
        .binding = &shader_injection.tone_map_white_clip,
        .default_value = 10.f,
        .label = "White Clip",
        .section = "Tone Mapping",
        .tooltip = "Sets the scene value that approaches Peak Brightness.",
        .min = 1.f,
        .max = 100.f,
        .is_visible = []() { return current_settings_mode >= 2 && shader_injection.tone_map_type == 3.f; },
    },
    new renodx::utils::settings::Setting{
        .key = "GammaCorrection",
        .binding = &shader_injection.gamma_correction,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Gamma Correction",
        .section = "Tone Mapping",
        .tooltip = "Emulates a display EOTF.",
        .labels = {"Off", "2.2", "BT.1886"},
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueProcessor",
        .binding = &shader_injection.tone_map_hue_processor,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Hue Processor",
        .section = "Tone Mapping",
        .tooltip = "Selects hue processor",
        .labels = {"OKLab", "ICtCp", "darkTable UCS"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueCorrection",
        .binding = &shader_injection.tone_map_hue_correction,
        .default_value = 100.f,
        .label = "Hue Correction",
        .section = "Tone Mapping",
        .tooltip = "Hue retention strength.",
        .min = 0.f,
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .max = 2.f,
        .format = "%.2f",
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlights",
        .binding = &shader_injection.tone_map_highlights,
        .default_value = 50.f,
        .label = "Highlights",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeShadows",
        .binding = &shader_injection.tone_map_shadows,
        .default_value = 50.f,
        .label = "Shadows",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeContrast",
        .binding = &shader_injection.tone_map_contrast,
        .default_value = 50.f,
        .label = "Contrast",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlightSaturation",
        .binding = &shader_injection.tone_map_highlight_saturation,
        .default_value = 50.f,
        .label = "Highlight Saturation",
        .section = "Color Grading",
        .tooltip = "Adds or removes highlight color.",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value * 0.02f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeBlowout",
        .binding = &shader_injection.tone_map_blowout,
        .default_value = 0.f,
        .label = "Blowout",
        .section = "Color Grading",
        .tooltip = "Controls highlight desaturation due to overexposure.",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeFlare",
        .binding = &shader_injection.tone_map_flare,
        .default_value = 0.f,
        .label = "Flare",
        .section = "Color Grading",
        .tooltip = "Flare/Glare Compensation",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type == 3; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeStrength",
        .binding = &shader_injection.color_grade_strength,
        .default_value = 100.f,
        .label = "LUT Strength",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeLUTScaling",
        .binding = &shader_injection.custom_lut_scaling,
        .default_value = 100.f,
        .label = "LUT Scaling",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "RenoDRTCurve",
        .binding = &shader_injection.renodrt_tone_map_method,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "RenoDRT Curve",
        .section = "Tone Mapping",
        .tooltip = "Selects the HDR highlight rolloff curve. Daniele is the recommended default.",
        .labels = {"Daniele", "Reinhard", "Hermite Spline", "Neutwo"},
        .is_visible = []() { return current_settings_mode >= 1 && shader_injection.tone_map_type == 3.f; },
    },
    new renodx::utils::settings::Setting{
        .key = "RenoDRTScaling",
        .binding = &shader_injection.tone_map_per_channel,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "RenoDRT Scaling",
        .section = "Tone Mapping",
        .tooltip = "Luminance preserves hue. Per Channel and Max Channel can make bright colored lights appear more intense.",
        .labels = {"Luminance", "Per Channel", "Max Channel"},
        .is_visible = []() { return current_settings_mode >= 2 && shader_injection.tone_map_type == 3.f; },
    },
    new renodx::utils::settings::Setting{
        .key = "RenoExposureStrength",
        .binding = &shader_injection.custom_auto_exposure,
        .default_value = 75.f,
        .label = "Highlight Exposure Retention",
        .section = "Tone Mapping",
        .tooltip = "Linearly blends HDR highlights from unexposed (0) to DL2's game exposure (100). Midtones always use the full game exposure.",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "RenoExposureMin",
        .binding = &shader_injection.auto_exposure_min,
        .default_value = 0.5f,
        .label = "Highlight Exposure Minimum",
        .section = "Tone Mapping",
        .tooltip = "Lower bound for HDR-highlight exposure after protection is applied.",
        .min = 0.1f,
        .max = 1.f,
        .format = "%.2f",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "RenoExposureMax",
        .binding = &shader_injection.auto_exposure_max,
        .default_value = 2.f,
        .label = "Highlight Exposure Maximum",
        .section = "Tone Mapping",
        .tooltip = "Upper bound for HDR-highlight exposure after protection is applied.",
        .min = 1.f,
        .max = 4.f,
        .format = "%.2f",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "RenoHDRProtectionStart",
        .binding = &shader_injection.hdr_exposure_protection_start,
        .default_value = 0.75f,
        .label = "HDR Protection Start",
        .section = "Tone Mapping",
        .tooltip = "Raw scene luminance where HDR highlight exposure protection begins.",
        .min = 0.05f,
        .max = 8.f,
        .format = "%.2f",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "RenoHDRProtectionEnd",
        .binding = &shader_injection.hdr_exposure_protection_end,
        .default_value = 4.f,
        .label = "HDR Protection End",
        .section = "Tone Mapping",
        .tooltip = "Raw scene luminance where full HDR highlight exposure protection is reached.",
        .min = 0.1f,
        .max = 16.f,
        .format = "%.2f",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxLensFlare",
        .binding = &shader_injection.custom_lens_flare,
        .default_value = 100.f,
        .label = "Lens Flare",
        .section = "Effects",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "DebugMode",
        .binding = &shader_injection.debug_mode,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = false,
        .label = "Debug Mode",
        .section = "Debug",
        .tooltip = "False-color visualization and output probes. Luminance Ladder places four known scene values in the lower-right corner.",
        .labels = {"Off", "HDR Input Range", "Neutral SDR", "Graded SDR", "RenoDRT Output", "Output Probe (500-nit red)", "Scene Probe (Peak white)", "Output Luminance Ladder", "Raw Output Ladder", "Late LUT Output Ladder", "Source t0 Range", "Auto Exposure t1", "Bypass Late Gamma (Test)", "LUT Output Constant (500-nit white)", "Gamma Output Constant (500-nit white)", "Final Proxy Constant (500-nit white)"},
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "CaptureDownstreamDraws",
        .binding = &downstream_draw_capture,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .can_reset = false,
        .label = "Capture Post-Gamma Command Order",
        .section = "Debug",
        .tooltip = "One-shot, records up to 16 graphics or compute shader hashes after 0xAD085E81 until the next Present, then turns itself off. No resource tracing or dumping.",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
};

void OnPresetOff() {
  renodx::utils::settings::UpdateSettings({
      {"ToneMapType", 0.f},
      {"ToneMapPeakNits", 203.f},
      {"ToneMapGameNits", 203.f},
      {"ToneMapWhiteClip", 10.f},
      {"RenoDRTCurve", 0.f},
      {"RenoDRTScaling", 0.f},
      {"RenoExposureStrength", 0.f},
      {"RenoExposureMin", 0.5f},
      {"RenoExposureMax", 2.f},
      {"RenoHDRProtectionStart", 0.75f},
      {"RenoHDRProtectionEnd", 4.f},
      {"GammaCorrection", 0.f},
      {"ToneMapHueProcessor", 0.f},
      {"ToneMapHueCorrection", 0.f},
      {"ColorGradeExposure", 1.f},
      {"ColorGradeHighlights", 50.f},
      {"ColorGradeShadows", 50.f},
      {"ColorGradeContrast", 50.f},
      {"ColorGradeSaturation", 50.f},
      {"ColorGradeHighlightSaturation", 50.f},
      {"ColorGradeBlowout", 0.f},
      {"ColorGradeStrength", 100.f},
      {"ColorGradeLUTScaling", 100.f},
      {"FxLensFlare", 100.f},
      {"SwapChainEncoding", 4.f},
      {"DebugMode", 0.f},
      {"CaptureDownstreamDraws", 0.f},
  });
}

// TEST L (DL1-style without SetUseHDR10):
// test-J 日志分析显示：
//   1. SetUseHDR10(true) 把 swapchain 格式改成 R10G10B10A2 (HDR10 PQ)
//   2. 但 upgrade_targets 是 R8G8B8A8 -> R16G16B16A16_FLOAT，不匹配 R10G10B10A2
//   3. back buffer 的 has_upgrade_target: false，只有 clone_target
//   4. 游戏把 SDR 内容 [0,1] 渲染到 R10G10B10A2 (HDR10 PQ) back buffer
//   5. SDR 数值被错误解释为 PQ 编码 → 颜色炸裂
//   6. 进入游戏后某些渲染路径导致完全黑屏
//
// 正确做法（参考 DL1）：
//   - 不用 SetUseHDR10，target_format 保持默认 R16G16B16A16_FLOAT (scRGB)
//   - swapchain 升级为 R16G16B16A16_FLOAT，游戏渲染 [0,1] SDR 内容到 float16
//   - swapchain proxy shader 把 R16G16B16A16_FLOAT 转换为 HDR 输出
//   - 用 OnInitDevice 动态切换 DX11/DX12 shader（保留 test-J 的做法）
//   - 不用 swapchain_proxy_compatibility_mode=false（用默认 true）
//   - upgrade_targets 去掉 copy_dest（参考 DL1）
//
// test-J 日志证明 D3D12 swapchain 可以被修改（没有循环创建/销毁），
// 之前的"循环"结论是基于 shared state bug 修复前的测试，不可靠。
void OnInitDevice(reshade::api::device* device) {
  if (device->get_api() == reshade::api::device_api::d3d11) {
    renodx::mods::shader::expected_constant_buffer_space = 0;
    renodx::mods::swapchain::expected_constant_buffer_space = 0;
    reshade::log::message(reshade::log::level::info, "Activating DX11 swap chain proxy...");
    renodx::mods::swapchain::swap_chain_proxy_vertex_shader = __swap_chain_proxy_vertex_shader_dx11;
    renodx::mods::swapchain::swap_chain_proxy_pixel_shader = __swap_chain_proxy_pixel_shader_dx11;
  } else if (device->get_api() == reshade::api::device_api::d3d12) {
    reshade::log::message(reshade::log::level::info, "Activating DX12 swap chain proxy...");
    renodx::mods::shader::expected_constant_buffer_space = 50;
    renodx::mods::swapchain::expected_constant_buffer_space = 50;
    renodx::mods::swapchain::swap_chain_proxy_vertex_shader = __swap_chain_proxy_vertex_shader_dx12;
    renodx::mods::swapchain::swap_chain_proxy_pixel_shader = __swap_chain_proxy_pixel_shader_dx12;
  }
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Dying Light 2";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      renodx::utils::command_action::Register(
          OnDownstreamDrawCapture,
          {.shader_hash = 0u,
           .command_types = renodx::utils::command_action::COMMAND_TYPE_DIRECT_DRAW
                            | renodx::utils::command_action::COMMAND_TYPE_DIRECT_DISPATCH
                            | renodx::utils::command_action::COMMAND_TYPE_INDIRECT});
      reshade::register_event<reshade::addon_event::present>(OnDownstreamDrawCapturePresent);

      // Shader hook config (applies to both D3D11 and D3D12 custom shaders)
      // DL2 shared.h uses register(b13, space50) for SM5.1+ (D3D12) and
      // register(b13) for SM5.0 (D3D11). mods::shader::OnInitDevice reads
      // expected_constant_buffer_space only for d3d12/vulkan, so setting 50
      // here is correct: D3D12 gets 50, D3D11 keeps default 0.
      renodx::mods::shader::expected_constant_buffer_index = 13;
      renodx::mods::shader::expected_constant_buffer_space = 50;
      renodx::mods::shader::allow_multiple_push_constants = true;
      renodx::mods::shader::force_pipeline_cloning = true;

      // TEST M (test-L + compatibility_mode=false):
      // test-L 出现白色亮块+残影症状（打开 ReShade 面板关闭后留下白色方块，
      // 鼠标留下残影），像是 compatibility mode 下 D3D12 swapchain 渲染异常。
      // 原代码设置了 compatibility_mode=false，这里加上试试。
      // 不调用 SetUseHDR10 → target_format 保持默认 R16G16B16A16_FLOAT (scRGB)
      // 不用 ignored_device_apis → D3D12 swapchain 也被修改为 R16G16B16A16_FLOAT
      // OnInitDevice 动态切换 DX11/DX12 shader 和 cbuffer space
      renodx::mods::swapchain::swapchain_proxy_compatibility_mode = false;
      renodx::mods::swapchain::force_borderless = false;
      renodx::mods::swapchain::use_resource_cloning = true;

      // 初始用 DX11 shader，OnInitDevice 会根据 device API 动态切换
      renodx::mods::swapchain::swap_chain_proxy_vertex_shader = __swap_chain_proxy_vertex_shader_dx11;
      renodx::mods::swapchain::swap_chain_proxy_pixel_shader = __swap_chain_proxy_pixel_shader_dx11;
      renodx::mods::swapchain::expected_constant_buffer_index = 13;
      renodx::mods::swapchain::expected_constant_buffer_space = 50;

      // Upgrade targets: R8G8B8A8 -> R16G16B16A16_FLOAT (exclude copy_dest
      // per project convention to avoid matching D3D12 frame generation textures)
      renodx::mods::swapchain::resource_upgrade_infos.push_back({
          .old_format = reshade::api::format::r8g8b8a8_typeless,
          .new_format = reshade::api::format::r16g16b16a16_float,
          .ignore_size = false,
          .use_resource_view_cloning = true,
          .aspect_ratio = renodx::mods::swapchain::SwapChainUpgradeTarget::BACK_BUFFER,
          .usage_include = reshade::api::resource_usage::render_target,
      });

      renodx::mods::swapchain::resource_upgrade_infos.push_back({
          .old_format = reshade::api::format::r8g8b8a8_unorm,
          .new_format = reshade::api::format::r16g16b16a16_float,
          .ignore_size = false,
          .use_resource_view_cloning = true,
          .aspect_ratio = renodx::mods::swapchain::SwapChainUpgradeTarget::ANY,
          .usage_include = reshade::api::resource_usage::render_target,
      });

      // DL2's SDR scene composite may use the sRGB view format. Promote it
      // as well, otherwise HDR values emitted by the scene bridge are clipped
      // to 1.0 before the late UI composite and swapchain proxy can see them.
      renodx::mods::swapchain::resource_upgrade_infos.push_back({
          .old_format = reshade::api::format::r8g8b8a8_unorm_srgb,
          .new_format = reshade::api::format::r16g16b16a16_float,
          .ignore_size = false,
          .use_resource_view_cloning = true,
          .aspect_ratio = renodx::mods::swapchain::SwapChainUpgradeTarget::ANY,
          .usage_include = reshade::api::resource_usage::render_target,
      });

      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);

      break;
    case DLL_PROCESS_DETACH:
      renodx::utils::command_action::Unregister(OnDownstreamDrawCapture);
      reshade::unregister_event<reshade::addon_event::present>(OnDownstreamDrawCapturePresent);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
