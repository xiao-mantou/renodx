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
        .key = "BlackFix",
        .binding = &shader_injection.black_fix,
        .value_type = renodx::utils::settings::SettingValueType::FLOAT,
        .default_value = 1.f,
        .label = "Black Point Removal",
        .section = "HDR",
        .tooltip = "Removes the LUT's black point (0.2533 nits at a 450 nit peak).\n"
                   "0 = untouched game output.",
        .min = 0.f,
        .max = 1.f,
        .format = "%.2f",
    },
    new renodx::utils::settings::Setting{
        .key = "ShadowNeutral",
        .binding = &shader_injection.shadow_neutral,
        .value_type = renodx::utils::settings::SettingValueType::FLOAT,
        .default_value = 1.f,
        .label = "Shadow Desaturation",
        .section = "HDR",
        .tooltip = "Neutralises the green cast the LUT adds below ~10 nits\n"
                   "(R/G = B/G = 0.79 at 1 nit). Luminance is preserved exactly.\n"
                   "0 = keep the original shadow tint.",
        .min = 0.f,
        .max = 1.f,
        .format = "%.2f",
    },
    new renodx::utils::settings::Setting{
        .key = "NeutralHiNits",
        .binding = &shader_injection.neutral_hi_nits,
        .value_type = renodx::utils::settings::SettingValueType::FLOAT,
        .default_value = 10.f,
        .label = "Desaturation Range",
        .section = "HDR",
        .tooltip = "Nits at which the shadow desaturation has fully faded out.",
        .min = 1.f,
        .max = 100.f,
        .format = "%.0f",
        .is_enabled = []() { return shader_injection.shadow_neutral > 0.f; },
    },
    new renodx::utils::settings::Setting{
        .key = "CrushNits",
        .binding = &shader_injection.crush_nits,
        .value_type = renodx::utils::settings::SettingValueType::FLOAT,
        .default_value = 0.f,
        .label = "Black Crush",
        .section = "HDR",
        .tooltip = "Additional nits removed on top of the black point.\n"
                   "0.05+ starts clipping real shadow detail.",
        .min = 0.f,
        .max = 5.f,
        .format = "%.2f",
    },
    new renodx::utils::settings::Setting{
        .key = "ShadowGain",
        .binding = &shader_injection.shadow_gain,
        .value_type = renodx::utils::settings::SettingValueType::FLOAT,
        .default_value = 1.f,
        .label = "Shadow Gain",
        .section = "HDR",
        .tooltip = "Lifts the shadows after the black point is removed.\n"
                   "1 = keep the LUT's own shadow gain.",
        .min = 1.f,
        .max = 3.f,
        .format = "%.2f",
        .is_enabled = []() { return shader_injection.black_fix > 0.f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ShadowKneeNits",
        .binding = &shader_injection.shadow_knee_nits,
        .value_type = renodx::utils::settings::SettingValueType::FLOAT,
        .default_value = 5.f,
        .label = "Shadow Gain Range",
        .section = "HDR",
        .tooltip = "Nits at which the shadow gain has fully faded out.",
        .min = 1.f,
        .max = 50.f,
        .format = "%.0f",
        .is_enabled = []() { return shader_injection.black_fix > 0.f && shader_injection.shadow_gain > 1.f; },
    },
    new renodx::utils::settings::Setting{
        .key = "KeepGrain",
        .binding = &shader_injection.keep_grain,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 1.f,
        .label = "Film Grain",
        .section = "HDR",
        .tooltip = "The grain is added in PQ code space and lifts the black floor.",
        .labels = {"Off", "On"},
    },

    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = std::string("Build: ") + renodx::utils::date::ISO_DATE_TIME,
        .section = "About",
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = "HDR only. The game's tonemap curve and grading are untouched.",
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
