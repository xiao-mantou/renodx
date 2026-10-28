/*
 * Copyright (C) 2025 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64
#define DEBUG_LEVEL_0

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_set>

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
void ArmDl2Dx11AdPathAudit();
void ArmDl2Dx11BffcPathAudit();

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
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DX11 Color Path",
        .section = "Diagnostics",
        .tooltip = "Arms a short DX11 resource-binding capture for the DL2 color path. It stops automatically after 256 target draws.",
        .on_change = []() { ArmDl2Dx11ColorPathAudit(); },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DX11 AD Path",
        .section = "Diagnostics",
        .tooltip = "Captures only the final 0xAD085E81 DX11 color-path draw. It stops automatically after 64 target draws.",
        .on_change = []() { ArmDl2Dx11AdPathAudit(); },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DX11 BFFC Path",
        .section = "Diagnostics",
        .tooltip = "Captures only the 0xBFFC45AC DX11 path for black-block investigation. It stops automatically after 64 target draws.",
        .on_change = []() { ArmDl2Dx11BffcPathAudit(); },
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

  // ReShade.log shows the swapchain is 1920x1080 while the game renders the
  // scene at 2560x1440 and downsamples on present. With the RAR/TGH exact-size
  // matching the scene target therefore never matched the back buffer, no
  // create-time upgrade was applied at all (0 hits on
  // "OnInitResource(applied create-time upgrade"), and the
  // 0x3E36DA5B -> 0x268BAB6D chain ran through an 8-bit view.
  // Relax the size check for the two RGBA8 render-target entries on DX11 only;
  // the sRGB entry keeps its exact-size matching and DX12 is untouched.
  auto dx11_resource_upgrade_infos = renodx::mods::swapchain::v2::resource_upgrade_infos;
  if (dx11_resource_upgrade_infos.empty()) return;
  const auto relaxed_entries = std::min<size_t>(dx11_resource_upgrade_infos.size(), 2);
  for (size_t i = 0; i < relaxed_entries; ++i) {
    dx11_resource_upgrade_infos[i].ignore_size = true;
  }
  renodx::utils::resource::upgrade::SetUpgradeInfos(device, dx11_resource_upgrade_infos);

  std::stringstream stream;
  stream << "[RenoDX] DL2 DX11 resource upgrades: size-independent matching for "
         << relaxed_entries << "/" << dx11_resource_upgrade_infos.size() << " RGBA8 targets.";
  reshade::log::message(reshade::log::level::info, stream.str().c_str());
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

enum class Dl2Dx11AuditMode : uint32_t {
  NONE = 0,
  COLOR_PATH = 1,
  AD_PATH = 2,
  BFFC_PATH = 3,
};

static std::atomic<Dl2Dx11AuditMode> dl2_dx11_audit_mode = Dl2Dx11AuditMode::NONE;
static std::atomic<uint32_t> dl2_dx11_audit_count = 0;
static std::atomic<uint32_t> dl2_dx11_audit_generation = 0;
static std::atomic_bool dl2_dx11_constant_observers_registered = false;
static constexpr uint32_t DL2_DX11_COLOR_PATH_AUDIT_LIMIT = 256;
static constexpr uint32_t DL2_DX11_TARGETED_AUDIT_LIMIT = 64;
static constexpr std::size_t DL2_DX11_CONSTANT_SAMPLE_LIMIT = 128;

// D3D11 DEFAULT constant buffers are not directly readable, so arm-time CPU updates
// are cached only to correlate the AD draw's b0 binding with cb0[0].x.
struct Dl2Dx11ConstantSample {
  uint64_t resource = 0;
  uint32_t generation = 0;
  uint32_t size = 0;
  std::array<uint8_t, 16> bytes = {};
  bool valid = false;
};

struct Dl2Dx11MappedConstantBuffer {
  uint64_t resource = 0;
  uint32_t generation = 0;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint8_t* data = nullptr;
  bool valid = false;
};

static std::array<Dl2Dx11ConstantSample, DL2_DX11_CONSTANT_SAMPLE_LIMIT> dl2_dx11_constant_samples = {};
static std::array<Dl2Dx11MappedConstantBuffer, DL2_DX11_CONSTANT_SAMPLE_LIMIT> dl2_dx11_mapped_constant_buffers = {};
static std::size_t dl2_dx11_next_constant_sample = 0;
static std::size_t dl2_dx11_next_mapped_constant_buffer = 0;
static std::mutex dl2_dx11_constant_observer_mutex;

void ClearDl2Dx11ConstantSamples() {
  const std::scoped_lock lock(dl2_dx11_constant_observer_mutex);
  for (auto& sample : dl2_dx11_constant_samples) sample = {};
  for (auto& mapping : dl2_dx11_mapped_constant_buffers) mapping = {};
  dl2_dx11_next_constant_sample = 0;
  dl2_dx11_next_mapped_constant_buffer = 0;
}

void CaptureDl2Dx11ConstantSample(
    reshade::api::resource resource,
    const void* buffer_data,
    uint64_t offset,
    uint64_t size) {
  if (resource.handle == 0u || buffer_data == nullptr || offset != 0u || size < sizeof(float) || size == UINT64_MAX) return;

  const auto generation = dl2_dx11_audit_generation.load(std::memory_order_acquire);
  const auto copy_size = static_cast<std::size_t>(std::min<uint64_t>(size, 16u));
  const auto* source = static_cast<const uint8_t*>(buffer_data);
  const std::scoped_lock lock(dl2_dx11_constant_observer_mutex);

  Dl2Dx11ConstantSample* sample = nullptr;
  for (auto& candidate : dl2_dx11_constant_samples) {
    if (candidate.valid && candidate.resource == resource.handle) {
      sample = &candidate;
      break;
    }
  }
  if (sample == nullptr) {
    sample = &dl2_dx11_constant_samples[dl2_dx11_next_constant_sample++ % DL2_DX11_CONSTANT_SAMPLE_LIMIT];
  }

  sample->resource = resource.handle;
  sample->generation = generation;
  sample->size = static_cast<uint32_t>(size);
  sample->bytes.fill(0u);
  std::memcpy(sample->bytes.data(), source, copy_size);
  sample->valid = true;
}

bool OnDl2Dx11UpdateBufferRegion(
    reshade::api::device* device,
    const void* buffer_data,
    reshade::api::resource resource,
    uint64_t offset,
    uint64_t size) {
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11) return false;
  const auto mode = dl2_dx11_audit_mode.load(std::memory_order_acquire);
  if (mode != Dl2Dx11AuditMode::COLOR_PATH && mode != Dl2Dx11AuditMode::AD_PATH) return false;
  CaptureDl2Dx11ConstantSample(resource, buffer_data, offset, size);
  return false;
}

void OnDl2Dx11MapBufferRegion(
    reshade::api::device* device,
    reshade::api::resource resource,
    uint64_t offset,
    uint64_t size,
    reshade::api::map_access /*access*/,
    void** mapped_data) {
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11) return;
  const auto mode = dl2_dx11_audit_mode.load(std::memory_order_acquire);
  if ((mode != Dl2Dx11AuditMode::COLOR_PATH && mode != Dl2Dx11AuditMode::AD_PATH)
      || resource.handle == 0u || mapped_data == nullptr || *mapped_data == nullptr) {
    return;
  }

  const std::scoped_lock lock(dl2_dx11_constant_observer_mutex);
  Dl2Dx11MappedConstantBuffer* mapping = nullptr;
  for (auto& candidate : dl2_dx11_mapped_constant_buffers) {
    if (candidate.valid && candidate.resource == resource.handle) {
      mapping = &candidate;
      break;
    }
  }
  if (mapping == nullptr) {
    mapping = &dl2_dx11_mapped_constant_buffers[dl2_dx11_next_mapped_constant_buffer++ % DL2_DX11_CONSTANT_SAMPLE_LIMIT];
  }
  *mapping = {
      .resource = resource.handle,
      .generation = dl2_dx11_audit_generation.load(std::memory_order_relaxed),
      .offset = offset,
      .size = size,
      .data = static_cast<uint8_t*>(*mapped_data),
      .valid = true,
  };
}

void OnDl2Dx11UnmapBufferRegion(reshade::api::device* device, reshade::api::resource resource) {
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11) return;
  const auto mode = dl2_dx11_audit_mode.load(std::memory_order_acquire);
  if ((mode != Dl2Dx11AuditMode::COLOR_PATH && mode != Dl2Dx11AuditMode::AD_PATH) || resource.handle == 0u) return;

  const auto generation = dl2_dx11_audit_generation.load(std::memory_order_acquire);
  const std::scoped_lock lock(dl2_dx11_constant_observer_mutex);
  for (auto& mapping : dl2_dx11_mapped_constant_buffers) {
    if (!mapping.valid || mapping.resource != resource.handle) continue;
    if (mapping.generation == generation && mapping.offset == 0u && mapping.data != nullptr) {
      uint64_t mapped_size = mapping.size;
      if (mapped_size == UINT64_MAX) {
        const auto desc = device->get_resource_desc(resource);
        mapped_size = desc.type == reshade::api::resource_type::buffer ? desc.buffer.size : 0u;
      }
      const auto copy_size = static_cast<std::size_t>(std::min<uint64_t>(mapped_size, 16u));
      if (copy_size >= sizeof(float)) {
        Dl2Dx11ConstantSample* sample = nullptr;
        for (auto& candidate : dl2_dx11_constant_samples) {
          if (candidate.valid && candidate.resource == mapping.resource) {
            sample = &candidate;
            break;
          }
        }
        if (sample == nullptr) {
          sample = &dl2_dx11_constant_samples[dl2_dx11_next_constant_sample++ % DL2_DX11_CONSTANT_SAMPLE_LIMIT];
        }
        sample->resource = mapping.resource;
        sample->generation = generation;
        sample->size = static_cast<uint32_t>(mapped_size);
        sample->bytes.fill(0u);
        std::memcpy(sample->bytes.data(), mapping.data, copy_size);
        sample->valid = true;
      }
    }
    mapping = {};
    return;
  }
}

void RegisterDl2Dx11ConstantObservers() {
  if (dl2_dx11_constant_observers_registered.exchange(true, std::memory_order_acq_rel)) return;
  reshade::register_event<reshade::addon_event::update_buffer_region>(OnDl2Dx11UpdateBufferRegion);
  reshade::register_event<reshade::addon_event::map_buffer_region>(OnDl2Dx11MapBufferRegion);
  reshade::register_event<reshade::addon_event::unmap_buffer_region>(OnDl2Dx11UnmapBufferRegion);
}

void UnregisterDl2Dx11ConstantObservers() {
  if (!dl2_dx11_constant_observers_registered.exchange(false, std::memory_order_acq_rel)) return;
  reshade::unregister_event<reshade::addon_event::update_buffer_region>(OnDl2Dx11UpdateBufferRegion);
  reshade::unregister_event<reshade::addon_event::map_buffer_region>(OnDl2Dx11MapBufferRegion);
  reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(OnDl2Dx11UnmapBufferRegion);
}

void DisarmDl2Dx11Audit() {
  dl2_dx11_audit_mode.store(Dl2Dx11AuditMode::NONE, std::memory_order_release);
  UnregisterDl2Dx11ConstantObservers();
  ClearDl2Dx11ConstantSamples();
}

const char* Dl2Dx11AuditModeName(Dl2Dx11AuditMode mode) {
  switch (mode) {
    case Dl2Dx11AuditMode::COLOR_PATH:
      return "color-path";
    case Dl2Dx11AuditMode::AD_PATH:
      return "ad-path";
    case Dl2Dx11AuditMode::BFFC_PATH:
      return "bffc-path";
    default:
      return "none";
  }
}

bool IsDl2Dx11AuditTarget(Dl2Dx11AuditMode mode, uint32_t shader_hash) {
  switch (mode) {
    case Dl2Dx11AuditMode::COLOR_PATH:
      return shader_hash == 0x3E36DA5B || shader_hash == 0x268BAB6D || shader_hash == 0xAD085E81;
    case Dl2Dx11AuditMode::AD_PATH:
      return shader_hash == 0xAD085E81;
    case Dl2Dx11AuditMode::BFFC_PATH:
      return shader_hash == 0xBFFC45AC;
    default:
      return false;
  }
}

uint32_t Dl2Dx11AuditLimit(Dl2Dx11AuditMode mode) {
  return mode == Dl2Dx11AuditMode::COLOR_PATH ? DL2_DX11_COLOR_PATH_AUDIT_LIMIT : DL2_DX11_TARGETED_AUDIT_LIMIT;
}

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

  const auto mode = dl2_dx11_audit_mode.load(std::memory_order_acquire);
  if (!IsDl2Dx11AuditTarget(mode, shader_hash)) return;

  const uint32_t sequence = dl2_dx11_audit_count.fetch_add(1, std::memory_order_relaxed);
  const uint32_t limit = Dl2Dx11AuditLimit(mode);
  if (sequence >= limit) {
    DisarmDl2Dx11Audit();
    return;
  }

  auto* context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  if (context == nullptr) return;

  ID3D11ShaderResourceView* srvs[2] = {};
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11Buffer* cb0 = nullptr;
  context->PSGetShaderResources(0, 2, srvs);
  context->OMGetRenderTargets(1, &rtv, nullptr);
  if (shader_hash == 0xAD085E81) context->PSGetConstantBuffers(0, 1, &cb0);

  const auto t0 = DescribeDl2Dx11Srv(srvs[0]);
  const auto t1 = DescribeDl2Dx11Srv(srvs[1]);
  const auto output = DescribeDl2Dx11Rtv(rtv);

  UINT viewport_count = 0;
  context->RSGetViewports(&viewport_count, nullptr);
  const UINT viewport_capacity = std::min<UINT>(
      viewport_count,
      D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
  std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports = {};
  if (viewport_capacity > 0u) {
    UINT requested_viewports = viewport_capacity;
    context->RSGetViewports(&requested_viewports, viewports.data());
    viewport_count = requested_viewports;
  }

  UINT scissor_count = 0;
  context->RSGetScissorRects(&scissor_count, nullptr);
  const UINT scissor_capacity = std::min<UINT>(
      scissor_count,
      D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
  std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors = {};
  if (scissor_capacity > 0u) {
    UINT requested_scissors = scissor_capacity;
    context->RSGetScissorRects(&requested_scissors, scissors.data());
    scissor_count = requested_scissors;
  }

  D3D11_BUFFER_DESC cb0_desc = {};
  bool cb0_x_valid = false;
  float cb0_x = 0.f;
  uint32_t cb0_sample_size = 0u;
  uint32_t cb0_sample_generation = 0u;
  if (cb0 != nullptr) {
    cb0->GetDesc(&cb0_desc);
    const auto cb0_handle = reinterpret_cast<uint64_t>(cb0);
    const auto generation = dl2_dx11_audit_generation.load(std::memory_order_acquire);
    const std::scoped_lock lock(dl2_dx11_constant_observer_mutex);
    for (const auto& sample : dl2_dx11_constant_samples) {
      if (!sample.valid || sample.resource != cb0_handle || sample.generation != generation) continue;
      std::memcpy(&cb0_x, sample.bytes.data(), sizeof(cb0_x));
      cb0_sample_size = sample.size;
      cb0_sample_generation = sample.generation;
      cb0_x_valid = true;
      break;
    }
  }

  uint64_t last_pipeline = 0u;
  uint64_t pixel_pipeline = 0u;
  uint64_t replacement_pipeline = 0u;
  uint32_t replacement_stages = 0u;
  bool pipeline_is_replacement = false;
  bool pipeline_details_found = false;
  if (auto* shader_state = renodx::utils::shader::GetCurrentState(cmd_list); shader_state != nullptr) {
    last_pipeline = shader_state->last_pipeline.handle;
    if (auto* pixel_state = renodx::utils::shader::GetCurrentPixelState(shader_state); pixel_state != nullptr) {
      pixel_pipeline = pixel_state->pipeline.handle;
    }
    const auto pipeline_for_details = pixel_pipeline != 0u ? pixel_pipeline : last_pipeline;
    if (pipeline_for_details != 0u) {
      pipeline_details_found = renodx::utils::shader::GetPipelineShaderDetails(
          reshade::api::pipeline{pipeline_for_details},
          [&](const auto& details) {
            replacement_pipeline = details.replacement_pipeline.handle;
            replacement_stages = static_cast<uint32_t>(details.replacement_stages);
            pipeline_is_replacement = details.is_replacement;
          });
    }
  }

  std::stringstream stream;
  stream << "[RenoDX] DL2 DX11 " << Dl2Dx11AuditModeName(mode) << " audit #" << (sequence + 1)
         << " shader=" << PRINT_CRC32(shader_hash)
         << " cmd=" << PRINT_PTR(cmd_list->get_native())
         << " generation=" << dl2_dx11_audit_generation.load(std::memory_order_relaxed);
  LogDl2Dx11AuditBinding(&stream, "t0", t0);
  LogDl2Dx11AuditBinding(&stream, "t1", t1);
  LogDl2Dx11AuditBinding(&stream, "rtv0", output);
  stream << " cb0=" << PRINT_PTR(reinterpret_cast<uint64_t>(cb0))
         << " cb0_bytes=" << cb0_desc.ByteWidth
         << " cb0_usage=" << static_cast<uint32_t>(cb0_desc.Usage)
         << " cb0_cpu=0x" << std::hex << cb0_desc.CPUAccessFlags << std::dec
         << " cb0_x_valid=" << (cb0_x_valid ? 1 : 0);
  if (cb0_x_valid) {
    stream << " cb0_x=" << cb0_x
           << " cb0_sample_size=" << cb0_sample_size
           << " cb0_sample_generation=" << cb0_sample_generation;
  }
  stream << " viewport_count=" << viewport_count;
  for (UINT i = 0; i < viewport_count && i < viewport_capacity; ++i) {
    const auto& viewport = viewports[i];
    stream << " vp" << i << "=(" << viewport.TopLeftX << "," << viewport.TopLeftY
           << "," << viewport.Width << "x" << viewport.Height
           << ",z=" << viewport.MinDepth << ".." << viewport.MaxDepth << ")";
  }
  stream << " scissor_count=" << scissor_count;
  for (UINT i = 0; i < scissor_count && i < scissor_capacity; ++i) {
    const auto& scissor = scissors[i];
    stream << " sc" << i << "=(" << scissor.left << "," << scissor.top
           << ".." << scissor.right << "," << scissor.bottom << ")";
  }
  stream << " last_pipeline=" << PRINT_PTR(last_pipeline)
         << " pixel_pipeline=" << PRINT_PTR(pixel_pipeline)
         << " pipeline_details=" << (pipeline_details_found ? 1 : 0)
         << " pipeline_is_replacement=" << (pipeline_is_replacement ? 1 : 0)
         << " replacement_pipeline=" << PRINT_PTR(replacement_pipeline)
         << " replacement_stages=0x" << std::hex << replacement_stages << std::dec
         << " replacement_bound=" << ((pipeline_is_replacement
                                             || (pixel_pipeline != 0u && pixel_pipeline == replacement_pipeline))
                                            ? 1
                                            : 0);
  reshade::log::message(reshade::log::level::info, stream.str().c_str());

  if (sequence + 1u >= limit) {
    DisarmDl2Dx11Audit();
    std::stringstream disarm_message;
    disarm_message << "[RenoDX] DL2 DX11 " << Dl2Dx11AuditModeName(mode)
                   << " audit disarmed after reaching its " << limit << "-draw budget.";
    reshade::log::message(
        reshade::log::level::info,
        disarm_message.str().c_str());
  }

  if (srvs[0] != nullptr) srvs[0]->Release();
  if (srvs[1] != nullptr) srvs[1]->Release();
  if (rtv != nullptr) rtv->Release();
  if (cb0 != nullptr) cb0->Release();
}

// One-shot "is this hash actually present in the running build" probe.
// Logs the first draw that uses each registered hash together with the active
// API, so a DX11-only hash list can be separated from a DX12 one without a dump.
void InstallReplacementHitLogging() {
  for (auto& [shader_hash, shader] : custom_shaders) {
    const auto prior_on_draw = shader.on_draw;
    shader.on_draw = [shader_hash, prior_on_draw](reshade::api::command_list* cmd_list) {
      if (prior_on_draw != nullptr && !prior_on_draw(cmd_list)) return false;

      static std::mutex hit_mutex;
      static std::unordered_set<uint32_t> logged_hashes;
      bool first_hit = false;
      {
        const std::scoped_lock lock(hit_mutex);
        first_hit = logged_hashes.insert(shader_hash).second;
      }
      if (!first_hit || cmd_list == nullptr || cmd_list->get_device() == nullptr) return true;

      const auto api = cmd_list->get_device()->get_api();
      std::stringstream stream;
      stream << "[RenoDX] DL2 replacement hit: " << PRINT_CRC32(shader_hash)
             << " api=" << (api == reshade::api::device_api::d3d11 ? "d3d11" : "d3d12");
      reshade::log::message(reshade::log::level::info, stream.str().c_str());
      return true;
    };
  }
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
  ClearDl2Dx11ConstantSamples();
  dl2_dx11_audit_generation.fetch_add(1u, std::memory_order_acq_rel);
  RegisterDl2Dx11ConstantObservers();
  dl2_dx11_audit_count.store(0, std::memory_order_release);
  dl2_dx11_audit_mode.store(Dl2Dx11AuditMode::COLOR_PATH, std::memory_order_release);
  reshade::log::message(
      reshade::log::level::info,
      "[RenoDX] DL2 DX11 color-path audit armed by user (0x3E36DA5B, 0x268BAB6D, 0xAD085E81; budget=256 draws).");
}

void ArmDl2Dx11AdPathAudit() {
  ClearDl2Dx11ConstantSamples();
  dl2_dx11_audit_generation.fetch_add(1u, std::memory_order_acq_rel);
  RegisterDl2Dx11ConstantObservers();
  dl2_dx11_audit_count.store(0, std::memory_order_release);
  dl2_dx11_audit_mode.store(Dl2Dx11AuditMode::AD_PATH, std::memory_order_release);
  reshade::log::message(
      reshade::log::level::info,
      "[RenoDX] DL2 DX11 ad-path audit armed by user (0xAD085E81; budget=64 draws).");
}

void ArmDl2Dx11BffcPathAudit() {
  UnregisterDl2Dx11ConstantObservers();
  ClearDl2Dx11ConstantSamples();
  dl2_dx11_audit_generation.fetch_add(1u, std::memory_order_acq_rel);
  dl2_dx11_audit_count.store(0, std::memory_order_release);
  dl2_dx11_audit_mode.store(Dl2Dx11AuditMode::BFFC_PATH, std::memory_order_release);
  reshade::log::message(
      reshade::log::level::info,
      "[RenoDX] DL2 DX11 bffc-path audit armed by user (0xBFFC45AC; budget=64 draws).");
}

void OnDestroyDl2Dx11AuditSwapchain(reshade::api::swapchain* /*swapchain*/, bool /*resize*/) {
  DisarmDl2Dx11Audit();
  dl2_dx11_audit_count.store(0, std::memory_order_release);
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
      // Match the register used by shared.h (b13, space50 for DX12). The old
      // DX12 path set this explicitly; leaving it at the auto-selected b0
      // causes replacement pipelines to be built against the wrong layout.
      renodx::mods::shader::expected_constant_buffer_index = 13;

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
        // Match the RAR/TGH baseline: only the scene target at the expected
        // dimensions receives the FP16 clone.
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
      DisarmDl2Dx11Audit();
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  if (fdw_reason == DLL_PROCESS_DETACH) {
    reshade::unregister_event<reshade::addon_event::init_device>(ConfigureDl2Dx11ResourceUpgrades);
    reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnDestroyDl2Dx11AuditSwapchain);
  }
    renodx::mods::swapchain::v2::Use(fdw_reason, &shader_injection);
  if (fdw_reason == DLL_PROCESS_ATTACH) {
    // Register after swapchain_v2 so its per-device upgrade list is installed
    // first and this DX11-only override runs afterwards.
    reshade::register_event<reshade::addon_event::init_device>(ConfigureDl2Dx11ResourceUpgrades);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroyDl2Dx11AuditSwapchain);
  }
  if (fdw_reason == DLL_PROCESS_ATTACH) {
    // Keep the DX11 resource-chain diagnostics scoped to the proven color-path
    // shaders plus the separately armed BFFC investigation target.
    InstallDl2Dx11ColorPathAudit();
    InstallReplacementHitLogging();
  }
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
