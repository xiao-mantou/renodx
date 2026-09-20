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
float current_preset = 5.f;
float applied_preset = 5.f;

std::span<const uint8_t> SelectedVariant() {
  switch (static_cast<int>(current_preset)) {
    case 0:
      return __0x40100101;  // 400 nits / paper white 100
    case 1:
      return __0x40150101;  // 400 nits / paper white 150
    case 2:
      return __0x40203101;  // 400 nits / paper white 203
    case 3:
      return __0x45100101;  // 450 nits / paper white 100
    case 4:
      return __0x45150101;  // 450 nits / paper white 150
    case 5:
      return __0x72BE437B;  // 450 nits / paper white 203
    case 6:
      return __0x50100101;  // 500 nits / paper white 100
    case 7:
      return __0x50150101;  // 500 nits / paper white 150
    case 8:
      return __0x50203101;  // 500 nits / paper white 203
    case 9:
      return __0x55100101;  // 550 nits / paper white 100
    case 10:
      return __0x55150101;  // 550 nits / paper white 150
    case 11:
      return __0x55203101;  // 550 nits / paper white 203
    case 12:
      return __0x60100101;  // 600 nits / paper white 100
    case 13:
      return __0x60150101;  // 600 nits / paper white 150
    case 14:
      return __0x60203101;  // 600 nits / paper white 203
    case 15:
      return __0x65100101;  // 650 nits / paper white 100
    case 16:
      return __0x65150101;  // 650 nits / paper white 150
    case 17:
      return __0x65203101;  // 650 nits / paper white 203
    case 18:
      return __0x10002031;  // 1000 nits / paper white 203
    case 19:
      return __0x14002031;  // 1400 nits / paper white 203
    default:
      return __0x72BE437B;  // 450 nits / paper white 203
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
        .default_value = 5.f,
        .label = "Peak / Paper White Nits",
        .section = "Tone Mapping",
        .tooltip = "Baked shader preset. Applies immediately.",
        .labels = {"400/100", "400/150", "400/203", "450/100", "450/150", "450/203", "500/100", "500/150", "500/203", "550/100", "550/150", "550/203", "600/100", "600/150", "600/203", "650/100", "650/150", "650/203", "1000/203", "1400/203"},
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
