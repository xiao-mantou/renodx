/*
 * Copyright (C) 2026 xiao-mantou
 * SPDX-License-Identifier: MIT
 */

#include <deps/imgui/imgui.h>

#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../utils/settings.hpp"

namespace {

// Preset indices into the embedded variants:
// peak: 0 = 400 nits, 1 = 450 nits, 2 = 1000 nits
float current_peak_preset = 1.f;
float applied_peak_preset = 1.f;

// paper white: 0 = 100 nits, 1 = 150 nits, 2 = 203 nits (default BT.2408)
float current_paper_white_preset = 2.f;
float applied_paper_white_preset = 2.f;

std::span<const uint8_t> SelectedVariant() {
  int pw = static_cast<int>(current_paper_white_preset);
  int peak = static_cast<int>(current_peak_preset);

  if (pw == 0) {  // 100 nits
    switch (peak) {
      case 2:  return __0x33333100;  // 1000 nits
      case 1:  return __0x22222100;  // 450 nits
      default: return __0x11111100;  // 400 nits
    }
  } else if (pw == 1) {  // 150 nits
    switch (peak) {
      case 2:  return __0x33333150;  // 1000 nits
      case 1:  return __0x22222150;  // 450 nits
      default: return __0x11111150;  // 400 nits
    }
  } else {  // 203 nits (default)
    switch (peak) {
      case 2:  return __0x33333333;  // 1000 nits
      case 1:  return __0x22222222;  // 450 nits
      default: return __0x11111111;  // 400 nits
    }
  }
}

// Runtime replacements apply on the next present, so the setting takes effect immediately without a restart.
void OnPresent(
    reshade::api::command_queue*,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect*,
    const reshade::api::rect*,
    uint32_t,
    const reshade::api::rect*) {
  if (applied_peak_preset == current_peak_preset && applied_paper_white_preset == current_paper_white_preset) return;

  auto* device = swapchain->get_device();
  if (device == nullptr) return;

  renodx::utils::shader::AddRuntimeReplacement(device, 0xB2F67FED, SelectedVariant());
  applied_peak_preset = current_peak_preset;
  applied_paper_white_preset = current_paper_white_preset;
  reshade::log::message(reshade::log::level::info, "f122: applied runtime tonemap variant (peak + paper white)");
}

renodx::mods::shader::CustomShaders custom_shaders = {};

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .key = "ToneMapPeakNits",
        .binding = &current_peak_preset,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Peak Brightness",
        .section = "Tone Mapping",
        .tooltip = "Tone map peak in nits. Applies immediately.",
        .labels = {"400", "450", "1000"},
        .on_change = []() { applied_peak_preset = -1.f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapGameNits",
        .binding = &current_paper_white_preset,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 2.f,
        .label = "Game Brightness (Paper White)",
        .section = "Tone Mapping",
        .tooltip = "Paper/diffuse white in nits. 203 is standard HDR reference white. Applies immediately.",
        .labels = {"100", "150", "203"},
        .on_change = []() { applied_paper_white_preset = -1.f; },
    },
};

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for F1 22";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      renodx::utils::shader::use_replace_async = true;
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings);
  if (fdw_reason == DLL_PROCESS_ATTACH) {
    custom_shaders.clear();
    custom_shaders.emplace(0xB2F67FED, renodx::mods::shader::CreateCustomShader(0xB2F67FED, SelectedVariant()));
    custom_shaders.emplace(0x2EA7EE8A, renodx::mods::shader::CreateCustomShader(0x2EA7EE8A, __0x2EA7EE8A));
    applied_peak_preset = current_peak_preset;
    applied_paper_white_preset = current_paper_white_preset;
  }
  renodx::mods::shader::Use(fdw_reason, custom_shaders);

  return TRUE;
}
