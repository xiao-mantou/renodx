/*
 * Copyright (C) 2026 xiao-mantou
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../utils/date.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

renodx::mods::shader::CustomShaders custom_shaders = {__ALL_CUSTOM_SHADERS};

ShaderInjectData shader_injection;

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .key = "BlackLevelFix",
        .binding = &shader_injection.black_fix,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 1.f,
        .label = "Black Level Fix",
        .section = "HDR",
        .tooltip = "Removes the black point the game bakes into its LUT.\n"
                   "Measured at 0.2533 nits on a 450 nit peak, neutral\n"
                   "(R = G = B). Absolute, so it reads as a grey veil on OLED.\n"
                   "Off = untouched game output.",
        .labels = {"Off", "On"},
    },
    new renodx::utils::settings::Setting{
        .key = "ExtraBlackLevel",
        .binding = &shader_injection.crush_nits,
        .value_type = renodx::utils::settings::SettingValueType::FLOAT,
        .default_value = 0.f,
        .label = "Extra Black Level",
        .section = "HDR",
        .tooltip = "Manual black level, in nits, removed on top of the fix.\n"
                   "Applied after it, then rescaled so the peak never moves.\n"
                   "0.05+ starts clipping real shadow detail.",
        .min = 0.f,
        .max = 5.f,
        .format = "%.3f",
    },

    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = std::string("Build: ") + renodx::utils::date::ISO_DATE_TIME,
        .section = "About",
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = "HDR only. Tonemap curve and grading are untouched.",
        .section = "About",
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = "No chroma stage: the LUT's shadow cast is the creator's\n"
                 "split-tone, not an artifact.",
        .section = "About",
    },
};

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Clive Barker's Hellraiser: Revival";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      renodx::utils::settings::use_presets = false;

      renodx::mods::shader::expected_constant_buffer_index = 13;
      renodx::mods::shader::expected_constant_buffer_space = 50;
      renodx::mods::shader::allow_multiple_push_constants = true;
      renodx::mods::shader::force_pipeline_cloning = true;
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
