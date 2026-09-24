/*
 * Copyright (C) 2025 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64
#define DEBUG_LEVEL_0

#include <embed/shaders.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain_v2.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

renodx::mods::shader::CustomShaders custom_shaders = {__ALL_CUSTOM_SHADERS};

ShaderInjectData shader_injection;

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .key = "ToneMapType",
        .binding = &shader_injection.tone_map_type,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .can_reset = true,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Sets the tone mapper type",
        .labels = {"Vanilla", "Vanilla+"},
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
        .key = "ToneMapUINits",
        .binding = &shader_injection.graphics_white_nits,
        .default_value = 203.f,
        .label = "UI Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the brightness of UI and HUD elements in nits",
        .min = 48.f,
        .max = 500.f,
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .max = 2.f,
        .format = "%.2f",
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlights",
        .binding = &shader_injection.tone_map_highlights,
        .default_value = 50.f,
        .label = "Highlights",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeShadows",
        .binding = &shader_injection.tone_map_shadows,
        .default_value = 50.f,
        .label = "Shadows",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeContrast",
        .binding = &shader_injection.tone_map_contrast,
        .default_value = 50.f,
        .label = "Contrast",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
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
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeFlare",
        .binding = &shader_injection.tone_map_flare,
        .default_value = 0.f,
        .label = "Flare",
        .section = "Color Grading",
        .tooltip = "Flare/Glare Compensation",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxAutoExposure",
        .binding = &shader_injection.custom_auto_exposure,
        .default_value = 0.f,
        .label = "Auto Exposure",
        .section = "Effects",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
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
};

void OnPresetOff() {
  renodx::utils::settings::UpdateSettings({
      {"ToneMapType", 0.f},
      {"ToneMapPeakNits", 203.f},
      {"ToneMapGameNits", 203.f},
      {"ToneMapUINits", 203.f},
      {"ColorGradeExposure", 1.f},
      {"ColorGradeHighlights", 50.f},
      {"ColorGradeShadows", 50.f},
      {"ColorGradeContrast", 50.f},
      {"ColorGradeSaturation", 50.f},
      {"ColorGradeHighlightSaturation", 50.f},
      {"ColorGradeBlowout", 0.f},
      {"ColorGradeFlare", 0.f},
      {"FxAutoExposure", 100.f},
      {"FxLensFlare", 100.f},
  });
}

void OnInitDevice(reshade::api::device* device) {
  if (device->get_api() == reshade::api::device_api::d3d11) {
    renodx::mods::shader::expected_constant_buffer_space = 0;
        renodx::mods::swapchain::v2::expected_constant_buffer_space = 0;
    return;
  }

  if (device->get_api() == reshade::api::device_api::d3d12) {
    renodx::mods::shader::expected_constant_buffer_space = 50;
        renodx::mods::swapchain::v2::expected_constant_buffer_space = 50;
  }
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Dying Light 2";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      renodx::mods::shader::allow_multiple_push_constants = true;
      renodx::mods::shader::force_pipeline_cloning = true;
      renodx::mods::shader::disable_custom_replacements_d3d12 = true;
      renodx::mods::shader::disable_shader_injection_d3d12 = true;
      renodx::utils::shader::use_replace_on_bind = false;

      renodx::mods::swapchain::v2::SetUseHDR10(true);
      renodx::mods::swapchain::v2::prevent_full_screen = false;
      renodx::mods::swapchain::v2::force_borderless = false;
      renodx::mods::swapchain::v2::swapchain_proxy_compatibility_mode = false;
      renodx::mods::swapchain::v2::expected_constant_buffer_index = 13;
      renodx::mods::swapchain::v2::expected_constant_buffer_space = 50;
      renodx::mods::swapchain::v2::use_resource_cloning = true;

      renodx::mods::swapchain::v2::swap_chain_proxy_shaders = {
          {reshade::api::device_api::d3d11,
           {.vertex_shader = __swap_chain_proxy_vertex_shader_dx11,
            .pixel_shader = __swap_chain_proxy_pixel_shader_dx11}},
          {reshade::api::device_api::d3d12,
           {.vertex_shader = __swap_chain_proxy_vertex_shader_dx12,
            .pixel_shader = __swap_chain_proxy_pixel_shader_dx12}},
      };

      renodx::mods::swapchain::v2::resource_upgrade_infos.push_back({
        .old_format = reshade::api::format::r8g8b8a8_typeless,
        .new_format = reshade::api::format::r16g16b16a16_float,
        // Keep the RAR/TGH baseline's exact resource matching for the first
        // DX12 validation pass. DLSS Off size compatibility is handled later.
        .ignore_size = false,
        .use_resource_view_cloning = true,
        .use_resource_view_hot_swap = false,
                .aspect_ratio = renodx::mods::swapchain::v2::ResourceUpgradeInfo::ANY,
        .usage_include = reshade::api::resource_usage::render_target,
            });
            renodx::mods::swapchain::v2::resource_upgrade_infos.push_back({
        .old_format = reshade::api::format::r8g8b8a8_unorm,
        .new_format = reshade::api::format::r16g16b16a16_float,
        .ignore_size = false,
        .use_resource_view_cloning = true,
                .aspect_ratio = renodx::mods::swapchain::v2::ResourceUpgradeInfo::ANY,
        .usage_include = reshade::api::resource_usage::render_target,
            });
            renodx::mods::swapchain::v2::resource_upgrade_infos.push_back({
        .old_format = reshade::api::format::r8g8b8a8_unorm_srgb,
        .new_format = reshade::api::format::r16g16b16a16_float,
        .ignore_size = false,
        .use_resource_view_cloning = true,
        .use_resource_view_hot_swap = true,
                .aspect_ratio = renodx::mods::swapchain::v2::ResourceUpgradeInfo::ANY,
        .usage_include = reshade::api::resource_usage::render_target,
            });

      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      break;

    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
    renodx::mods::swapchain::v2::Use(fdw_reason, &shader_injection);
  if (fdw_reason == DLL_PROCESS_ATTACH) {
    // TGH UI replacements are validated on DX11 only; isolate all of them on DX12.
    constexpr uint32_t dl2_ui_shader_hashes[] = {
        0x1BF90CDB,
        0x2280559E,
        0x2BECAD9C,
        0x43B22618,
        0x54F3F767,
        0x61DBDE91,
        0x6C349427,
        0x7D1BA5D4,
        0x93053DEF,
        0xC6ADA2E9,
        0xE46618DA,
        0xEDC2563A,
        0xEFC06591,
        0xF34DDC49,
    };
    for (const auto shader_hash : dl2_ui_shader_hashes) {
      if (auto shader = custom_shaders.find(shader_hash); shader != custom_shaders.end()) {
        shader->second.on_replace = [](reshade::api::command_list* cmd_list) {
          return cmd_list->get_device()->get_api() != reshade::api::device_api::d3d12;
        };
      }
    }
  }
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
