/*
 * Copyright (C) 2026 xiao-mantou
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../utils/settings.hpp"

namespace {

// Index into the embedded peak variants below. NFS Unbound's DX12 root signatures leave no
// room for an injected settings cbuffer, so presets are baked into shader variants instead.
float current_preset = 1.f;
float applied_preset = 1.f;

std::span<const uint8_t> SelectedVariant() {
  switch (static_cast<int>(current_preset)) {
    case 0:
      return __0x11111111;  // 400 nits
    case 2:
      return __0x22222222;  // 450 nits / game 150
    case 3:
      return __0x33333333;  // 450 nits / game 100
    case 4:
      return __0x44444444;  // 1000 nits
    default:
      return __0x72BE437B;  // 450 nits / game 203
  }
}

// Runtime replacements apply on the next present, so the setting takes effect without a restart.
void OnPresent(
    reshade::api::command_queue*,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect*,
    const reshade::api::rect*,
    uint32_t,
    const reshade::api::rect*) {
  if (applied_preset == current_preset) return;

  auto* device = swapchain->get_device();
  if (device == nullptr) return;

  renodx::utils::shader::AddRuntimeReplacement(device, 0x72BE437B, SelectedVariant());
  applied_preset = current_preset;
  reshade::log::message(reshade::log::level::info, "nfsunbound: applied runtime peak preset");
}

renodx::mods::shader::CustomShaders custom_shaders = {};

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .key = "PeakPreset",
        .binding = &current_preset,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Peak / Game Nits",
        .section = "Tone Mapping",
        .tooltip = "Baked shader preset. Applies immediately.",
        .labels = {"400", "450/203", "450/150", "450/100", "1000"},
        .on_change = []() { applied_preset = -1.f; },
    },
};

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Need for Speed Unbound";

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
    custom_shaders.emplace(0x72BE437B, renodx::mods::shader::CreateCustomShader(0x72BE437B, SelectedVariant()));
    applied_preset = current_preset;
  }
  renodx::mods::shader::Use(fdw_reason, custom_shaders);

  return TRUE;
}
