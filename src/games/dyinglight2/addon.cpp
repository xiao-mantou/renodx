/*
 * Copyright (C) 2025 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64
#define DEBUG_LEVEL_0

#include <atomic>
#include <cstdint>
#include <sstream>

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

void ArmDl2Dx11ColorPathAudit();

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
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DX11 Color Path",
        .section = "Diagnostics",
        .tooltip = "Arms a short DX11 resource-binding capture for the DL2 color path. It stops automatically after 256 target draws.",
        .on_change = []() { ArmDl2Dx11ColorPathAudit(); },
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

void ConfigureDl2Dx11ResourceUpgrades(reshade::api::device* device) {
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11) return;

  // DLSS Off and non-native quality modes render the 0x3E scene target at an
  // internal resolution that does not match the swapchain. Keep DX12's exact
  // TGH matching, but let this DX11 scene target receive its size-preserving
  // FP16 clone so the 0x3E -> 0x268 chain does not fall back to an 8-bit view.
  auto dx11_resource_upgrade_infos = renodx::mods::swapchain::v2::resource_upgrade_infos;
  if (dx11_resource_upgrade_infos.empty()) return;
  dx11_resource_upgrade_infos[0].ignore_size = true;
  renodx::utils::resource::upgrade::SetUpgradeInfos(device, dx11_resource_upgrade_infos);
  reshade::log::message(
      reshade::log::level::info,
      "[RenoDX] DL2 DX11 resource upgrades: enabled size-independent typeless scene target matching.");
}

struct Dl2Dx11AuditBinding {
  uint64_t view = 0;
  uint64_t resource = 0;
  uint32_t view_format = DXGI_FORMAT_UNKNOWN;
  uint32_t resource_format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth_or_layers = 0;
  uint32_t bind_flags = 0;
  uint32_t misc_flags = 0;
  uint32_t first_level = 0;
  uint32_t level_count = 0;
  uint32_t first_slice = 0;
  uint32_t slice_count = 0;
  uint32_t samples = 1;
  bool tracked = false;
  bool is_clone = false;
  bool upgraded = false;
  uint64_t original_resource = 0;
  uint64_t clone_resource = 0;
  uint64_t clone_view = 0;
};

static std::atomic<bool> dl2_dx11_audit_armed = false;
static std::atomic<uint32_t> dl2_dx11_audit_count = 0;
static constexpr uint32_t DL2_DX11_AUDIT_LIMIT = 256;

void PopulateDl2Dx11TrackedViewData(Dl2Dx11AuditBinding* binding) {
  if (binding == nullptr || binding->view == 0u) return;

  const reshade::api::resource_view view = {binding->view};
  renodx::utils::resource::GetResourceViewInfo(view, [binding](const renodx::utils::resource::ResourceViewInfo& info) {
    binding->tracked = !info.destroyed;
    binding->is_clone = info.is_clone;
    binding->upgraded = info.upgraded;
    binding->original_resource = info.original_resource.handle;
    binding->clone_resource = info.clone_resource.handle;
    binding->clone_view = info.clone.handle;
  });
}

void PopulateDl2Dx11TextureData(Dl2Dx11AuditBinding* binding, ID3D11View* native_view) {
  if (binding == nullptr || native_view == nullptr) return;

  ID3D11Resource* native_resource = nullptr;
  native_view->GetResource(&native_resource);
  if (native_resource == nullptr) return;
  binding->resource = reinterpret_cast<uint64_t>(native_resource);

  D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  native_resource->GetType(&dimension);
  if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(native_resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
      D3D11_TEXTURE2D_DESC desc = {};
      texture->GetDesc(&desc);
      binding->resource_format = desc.Format;
      binding->width = desc.Width;
      binding->height = desc.Height;
      binding->depth_or_layers = desc.ArraySize;
      binding->bind_flags = desc.BindFlags;
      binding->misc_flags = desc.MiscFlags;
      binding->level_count = desc.MipLevels;
      binding->samples = desc.SampleDesc.Count;
      texture->Release();
    }
  } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
    ID3D11Texture3D* texture = nullptr;
    if (SUCCEEDED(native_resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
      D3D11_TEXTURE3D_DESC desc = {};
      texture->GetDesc(&desc);
      binding->resource_format = desc.Format;
      binding->width = desc.Width;
      binding->height = desc.Height;
      binding->depth_or_layers = desc.Depth;
      binding->bind_flags = desc.BindFlags;
      binding->misc_flags = desc.MiscFlags;
      binding->level_count = desc.MipLevels;
      texture->Release();
    }
  }

  native_resource->Release();
}

Dl2Dx11AuditBinding DescribeDl2Dx11Srv(ID3D11ShaderResourceView* srv) {
  Dl2Dx11AuditBinding binding = {};
  if (srv == nullptr) return binding;
  binding.view = reinterpret_cast<uint64_t>(srv);

  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  srv->GetDesc(&desc);
  binding.view_format = desc.Format;
  switch (desc.ViewDimension) {
    case D3D11_SRV_DIMENSION_TEXTURE2D:
      binding.first_level = desc.Texture2D.MostDetailedMip;
      binding.level_count = desc.Texture2D.MipLevels;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
      binding.first_level = desc.Texture2DArray.MostDetailedMip;
      binding.level_count = desc.Texture2DArray.MipLevels;
      binding.first_slice = desc.Texture2DArray.FirstArraySlice;
      binding.slice_count = desc.Texture2DArray.ArraySize;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE3D:
      binding.first_level = desc.Texture3D.MostDetailedMip;
      binding.level_count = desc.Texture3D.MipLevels;
      break;
    default:
      break;
  }
  PopulateDl2Dx11TextureData(&binding, srv);
  PopulateDl2Dx11TrackedViewData(&binding);
  return binding;
}

Dl2Dx11AuditBinding DescribeDl2Dx11Rtv(ID3D11RenderTargetView* rtv) {
  Dl2Dx11AuditBinding binding = {};
  if (rtv == nullptr) return binding;
  binding.view = reinterpret_cast<uint64_t>(rtv);

  D3D11_RENDER_TARGET_VIEW_DESC desc = {};
  rtv->GetDesc(&desc);
  binding.view_format = desc.Format;
  switch (desc.ViewDimension) {
    case D3D11_RTV_DIMENSION_TEXTURE2D:
      binding.first_level = desc.Texture2D.MipSlice;
      binding.level_count = 1;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
      binding.first_level = desc.Texture2DArray.MipSlice;
      binding.level_count = 1;
      binding.first_slice = desc.Texture2DArray.FirstArraySlice;
      binding.slice_count = desc.Texture2DArray.ArraySize;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE3D:
      binding.first_level = desc.Texture3D.MipSlice;
      binding.level_count = 1;
      binding.first_slice = desc.Texture3D.FirstWSlice;
      binding.slice_count = desc.Texture3D.WSize;
      break;
    default:
      break;
  }
  PopulateDl2Dx11TextureData(&binding, rtv);
  PopulateDl2Dx11TrackedViewData(&binding);
  return binding;
}

void LogDl2Dx11AuditBinding(std::stringstream* stream, const char* label, const Dl2Dx11AuditBinding& binding) {
  if (stream == nullptr || label == nullptr) return;
  *stream << " " << label << "(view=" << PRINT_PTR(binding.view)
          << ",res=" << PRINT_PTR(binding.resource)
          << ",view_fmt=0x" << std::hex << binding.view_format
          << ",res_fmt=0x" << binding.resource_format << std::dec
          << ",size=" << binding.width << "x" << binding.height
          << ",depth_layers=" << binding.depth_or_layers
          << ",mips=" << binding.first_level << "/" << binding.level_count
          << ",slice=" << binding.first_slice << "/" << binding.slice_count
          << ",samples=" << binding.samples
          << ",bind=0x" << std::hex << binding.bind_flags
          << ",misc=0x" << binding.misc_flags << std::dec
          << ",tracked=" << (binding.tracked ? 1 : 0)
          << ",clone=" << (binding.is_clone ? 1 : 0)
          << ",upgraded=" << (binding.upgraded ? 1 : 0)
          << ",orig_res=" << PRINT_PTR(binding.original_resource)
          << ",clone_res=" << PRINT_PTR(binding.clone_resource)
          << ",clone_view=" << PRINT_PTR(binding.clone_view) << ")";
}

void AuditDl2Dx11ColorPath(uint32_t shader_hash, reshade::api::command_list* cmd_list) {
  if (cmd_list == nullptr || cmd_list->get_device() == nullptr
      || cmd_list->get_device()->get_api() != reshade::api::device_api::d3d11) {
    return;
  }

  if (!dl2_dx11_audit_armed.load(std::memory_order_acquire)) return;

  const uint32_t sequence = dl2_dx11_audit_count.fetch_add(1, std::memory_order_relaxed);
  if (sequence >= DL2_DX11_AUDIT_LIMIT) {
    dl2_dx11_audit_armed.store(false, std::memory_order_release);
    return;
  }

  auto* context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  if (context == nullptr) return;

  ID3D11ShaderResourceView* srvs[2] = {};
  ID3D11RenderTargetView* rtv = nullptr;
  context->PSGetShaderResources(0, 2, srvs);
  context->OMGetRenderTargets(1, &rtv, nullptr);

  const auto t0 = DescribeDl2Dx11Srv(srvs[0]);
  const auto t1 = DescribeDl2Dx11Srv(srvs[1]);
  const auto output = DescribeDl2Dx11Rtv(rtv);

  std::stringstream stream;
  stream << "[RenoDX] DL2 DX11 color path audit #" << (sequence + 1)
         << " shader=" << PRINT_CRC32(shader_hash)
         << " cmd=" << PRINT_PTR(cmd_list->get_native());
  LogDl2Dx11AuditBinding(&stream, "t0", t0);
  LogDl2Dx11AuditBinding(&stream, "t1", t1);
  LogDl2Dx11AuditBinding(&stream, "rtv0", output);
  reshade::log::message(reshade::log::level::info, stream.str().c_str());

  if (sequence + 1u >= DL2_DX11_AUDIT_LIMIT) {
    dl2_dx11_audit_armed.store(false, std::memory_order_release);
    reshade::log::message(
        reshade::log::level::info,
        "[RenoDX] DL2 DX11 color path audit disarmed after reaching its 256-draw budget.");
  }

  if (srvs[0] != nullptr) srvs[0]->Release();
  if (srvs[1] != nullptr) srvs[1]->Release();
  if (rtv != nullptr) rtv->Release();
}

void InstallDl2Dx11ColorPathAudit() {
  constexpr uint32_t audit_shader_hashes[] = {0x3E36DA5B, 0x268BAB6D, 0xAD085E81, 0xBFFC45AC};
  for (const auto shader_hash : audit_shader_hashes) {
    if (auto shader = custom_shaders.find(shader_hash); shader != custom_shaders.end()) {
      const auto prior_on_draw = shader->second.on_draw;
      shader->second.on_draw = [shader_hash, prior_on_draw](reshade::api::command_list* cmd_list) {
        if (prior_on_draw != nullptr && !prior_on_draw(cmd_list)) return false;
        AuditDl2Dx11ColorPath(shader_hash, cmd_list);
        return true;
      };
    }
  }
}

void ArmDl2Dx11ColorPathAudit() {
  dl2_dx11_audit_count.store(0, std::memory_order_release);
  dl2_dx11_audit_armed.store(true, std::memory_order_release);
  reshade::log::message(
      reshade::log::level::info,
      "[RenoDX] DL2 DX11 color path audit armed by user (0x3E36DA5B, 0x268BAB6D, 0xAD085E81, 0xBFFC45AC; budget=256 draws).");
}

bool AllowD3D12Replacement(reshade::api::device* device, uint32_t shader_hash) {
  if (!renodx::mods::shader::disable_custom_replacements_d3d12
      || device == nullptr
      || device->get_api() != reshade::api::device_api::d3d12) {
    return true;
  }
  // Keep the two core HDR bridge stages together for the combined-path A/B.
  return shader_hash == renodx::mods::shader::d3d12_custom_replacement_allow_hash
         || shader_hash == 0x268BAB6D
         // First isolated DX12 UI candidate; keep the remaining UI stages gated.
         || shader_hash == 0x93053DEF
         // Second isolated UI pair from the prior DX11 coverage split.
         || shader_hash == 0xC6ADA2E9
         || shader_hash == 0x6C349427
         // Third isolated UI pair from the prior DX11 coverage split.
         || shader_hash == 0xEFC06591
         || shader_hash == 0xE46618DA
         // Isolate EDC independently because its historical pair includes
         // the progress-bar candidate that is intentionally still gated.
         || shader_hash == 0xEDC2563A
         // First half of the original popup/UI compositor group.
         || shader_hash == 0x54F3F767
         || shader_hash == 0xF34DDC49
         || shader_hash == 0x43B22618
         // Second half of the original popup/UI compositor group.
         || shader_hash == 0x2280559E
         || shader_hash == 0x61DBDE91
         || shader_hash == 0x7D1BA5D4
         // Isolate the remaining radial-progress UI shader.
         || shader_hash == 0x2BECAD9C;
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
      renodx::mods::shader::disable_shader_injection_d3d12 = false;
      // Match the register used by shared.h (b13, space50 for DX12). The old
      // DX12 path set this explicitly; leaving it at the auto-selected b0
      // causes replacement pipelines to be built against the wrong layout.
      renodx::mods::shader::expected_constant_buffer_index = 13;
      renodx::mods::shader::d3d12_custom_replacement_allow_hash = 0x3E36DA5B;
      renodx::utils::shader::SetReplacementFilter(&AllowD3D12Replacement);
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
  if (fdw_reason == DLL_PROCESS_DETACH) {
    dl2_dx11_audit_armed.store(false, std::memory_order_release);
    reshade::unregister_event<reshade::addon_event::init_device>(ConfigureDl2Dx11ResourceUpgrades);
  }
    renodx::mods::swapchain::v2::Use(fdw_reason, &shader_injection);
  if (fdw_reason == DLL_PROCESS_ATTACH) {
    // Register after swapchain_v2 so its per-device upgrade list is overridden
    // only for the DX11 scene target and only after the baseline is installed.
    reshade::register_event<reshade::addon_event::init_device>(ConfigureDl2Dx11ResourceUpgrades);
  }
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
        shader->second.on_replace = [shader_hash](reshade::api::command_list* cmd_list) {
          return cmd_list->get_device()->get_api() != reshade::api::device_api::d3d12
                 || shader_hash == 0x93053DEF
                 || shader_hash == 0xC6ADA2E9
                 || shader_hash == 0x6C349427
                 || shader_hash == 0xEFC06591
                 || shader_hash == 0xE46618DA
                 || shader_hash == 0xEDC2563A
                 || shader_hash == 0x54F3F767
                 || shader_hash == 0xF34DDC49
                 || shader_hash == 0x43B22618
                 || shader_hash == 0x2280559E
                 || shader_hash == 0x61DBDE91
                 || shader_hash == 0x7D1BA5D4
                 || shader_hash == 0x2BECAD9C;
        };
      }
    }
  }
  if (fdw_reason == DLL_PROCESS_ATTACH) {
    // Keep the DX11 resource-chain audit scoped to the three proven color-path
    // shaders. The callback is copied into the shader runtime below.
    InstallDl2Dx11ColorPathAudit();
  }
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
