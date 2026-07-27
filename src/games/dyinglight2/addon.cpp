/*
 * Copyright (C) 2025 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#include <array>
#include <atomic>
#include <d3d11.h>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <embed/shaders.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>
#include <sl_core_api.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/descriptor.hpp"
#include "../../utils/log.hpp"
#include "../../utils/pipeline_layout.hpp"
#include "../../utils/resource.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/vtable.hpp"
#include "./shared.h"

namespace {

float dlss_fg_tag_clone = 0.f;
float dlss_fg_skip_generated_proxy = 0.f;
bool dlss_fg_tag_capture = false;
bool dlss_fg_present_cadence_capture = false;
std::atomic_uint32_t dlss_fg_color_tag_serial = 0u;
uint32_t dlss_fg_last_present_tag_serial = 0u;
bool dlss_fg_hook_installed = false;
bool dlss_fg_waiting_for_streamline_logged = false;
bool dlss_fg_tag_clone_logged = false;

sl::Result (*real_sl_set_tag)(
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags,
    sl::CommandBuffer* cmd_buffer) = nullptr;

decltype(&slSetTagForFrame) real_sl_set_tag_for_frame = nullptr;

const char* GetStreamlineTagName(sl::BufferType type) {
  switch (type) {
    case sl::kBufferTypeBackbuffer:
      return "Backbuffer";
    case sl::kBufferTypeScalingOutputColor:
      return "ScalingOutputColor";
    case sl::kBufferTypeUIColorAndAlpha:
      return "UIColorAndAlpha";
    case sl::kBufferTypeHUDLessColor:
      return "HUDLessColor";
    case sl::kBufferTypeScalingInputColor:
      return "ScalingInputColor";
    default:
      return "Other";
  }
}

void MarkStreamlineColorTagSubmission(const sl::ResourceTag* tags, uint32_t num_tags) {
  if (tags == nullptr) return;
  for (uint32_t index = 0u; index < num_tags; ++index) {
    const auto type = tags[index].type;
    if (type == sl::kBufferTypeHUDLessColor || type == sl::kBufferTypeUIColorAndAlpha) {
      dlss_fg_color_tag_serial.fetch_add(1u, std::memory_order_relaxed);
      return;
    }
  }
}
void CaptureStreamlineTags(const char* call_name, const sl::ResourceTag* tags, uint32_t num_tags) {
  if (!dlss_fg_tag_capture || tags == nullptr) return;

  dlss_fg_tag_capture = false;
  std::stringstream stream;
  stream << "DL2 Streamline tags (" << call_name << ", " << num_tags << "):";
  const uint32_t count = num_tags > 32u ? 32u : num_tags;
  for (uint32_t index = 0u; index < count; ++index) {
    const auto& tag = tags[index];
    const uint64_t native = tag.resource == nullptr ? 0u : reinterpret_cast<uint64_t>(tag.resource->native);
    stream << " #" << std::dec << index << " " << GetStreamlineTagName(tag.type) << "(" << tag.type << ")"
           << " native=0x" << std::hex << std::uppercase << native;

    if (native == 0u) continue;
    const auto resource = reshade::api::resource{static_cast<uintptr_t>(native)};
    const auto* resource_info = renodx::utils::resource::GetResourceInfo(resource);
    if (resource_info == nullptr) continue;

    stream << " format=" << static_cast<uint32_t>(resource_info->desc.texture.format)
           << " usage=0x" << std::hex << static_cast<uint32_t>(resource_info->desc.usage)
           << " swapchain=" << (resource_info->is_swap_chain ? "yes" : "no")
           << " clone_enabled=" << (resource_info->clone_enabled ? "yes" : "no")
           << " clone_target=" << (resource_info->clone_target != nullptr ? "yes" : "no")
           << " views=" << std::dec << resource_info->resource_view_handles.size()
           << " clone=0x" << std::hex << resource_info->clone.handle;

    if (resource_info->clone.handle == 0u) continue;
    const auto* clone_info = renodx::utils::resource::GetResourceInfo(resource_info->clone);
    if (clone_info == nullptr) continue;

    stream << " clone_format=" << static_cast<uint32_t>(clone_info->desc.texture.format)
           << " clone_usage=0x" << static_cast<uint32_t>(clone_info->desc.usage)
           << " clone_is_clone=" << (clone_info->is_clone ? "yes" : "no");
  }
  if (num_tags > count) stream << " (truncated)";
  reshade::log::message(reshade::log::level::info, stream.str().c_str());
}

struct RoutedStreamlineTags {
  const sl::ResourceTag* tags = nullptr;
  uint32_t count = 0u;
  std::vector<sl::ResourceTag> tags_storage = {};
  std::vector<sl::Resource> resources_storage = {};
};

RoutedStreamlineTags RouteStreamlineColorTags(const sl::ResourceTag* tags, uint32_t num_tags) {
  RoutedStreamlineTags routed = {.tags = tags, .count = num_tags};
  if (dlss_fg_tag_clone < 0.5f || tags == nullptr || num_tags == 0u) return routed;

  std::unordered_map<uint64_t, size_t> replacement_indices;
  for (uint32_t index = 0u; index < num_tags; ++index) {
    const auto& tag = tags[index];
    if ((tag.type != sl::kBufferTypeHUDLessColor && tag.type != sl::kBufferTypeUIColorAndAlpha)
        || tag.resource == nullptr || tag.resource->native == nullptr) {
      continue;
    }

    const uint64_t native = reinterpret_cast<uint64_t>(tag.resource->native);
    const auto resource = reshade::api::resource{static_cast<uintptr_t>(native)};
    const auto* resource_info = renodx::utils::resource::GetResourceInfo(resource);
    if (resource_info == nullptr || !resource_info->clone_enabled || resource_info->clone.handle == 0u) continue;

    const auto* clone_info = renodx::utils::resource::GetResourceInfo(resource_info->clone);
    if (clone_info == nullptr || !clone_info->is_clone) continue;

    if (routed.tags_storage.empty()) {
      routed.tags_storage.assign(tags, tags + num_tags);
      routed.resources_storage.reserve(num_tags);
      routed.tags = routed.tags_storage.data();
    }

    auto [replacement, inserted] = replacement_indices.emplace(native, routed.resources_storage.size());
    if (inserted) {
      routed.resources_storage.push_back(*tag.resource);
      auto& clone_resource = routed.resources_storage.back();
      clone_resource.native = reinterpret_cast<void*>(resource_info->clone.handle);
      clone_resource.nativeFormat = static_cast<uint32_t>(clone_info->desc.texture.format);
      clone_resource.width = clone_info->desc.texture.width;
      clone_resource.height = clone_info->desc.texture.height;
      clone_resource.mipLevels = clone_info->desc.texture.levels;
      clone_resource.arrayLayers = clone_info->desc.texture.depth_or_layers;

      if (!dlss_fg_tag_clone_logged) {
        dlss_fg_tag_clone_logged = true;
        renodx::utils::log::i(
            "DL2 DLSS FG: routed tagged color resource ",
            renodx::utils::log::AsPtr(resource.handle),
            " format ",
            static_cast<uint32_t>(resource_info->desc.texture.format),
            " => clone ",
            renodx::utils::log::AsPtr(resource_info->clone.handle),
            " format ",
            static_cast<uint32_t>(clone_info->desc.texture.format),
            ".");
      }
    }

    routed.tags_storage[index].resource = &routed.resources_storage[replacement->second];
  }

  return routed;
}
sl::Result HookedSlSetTag(
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags,
    sl::CommandBuffer* cmd_buffer) {
  MarkStreamlineColorTagSubmission(tags, num_tags);
  CaptureStreamlineTags("slSetTag", tags, num_tags);
  const auto routed = RouteStreamlineColorTags(tags, num_tags);
  return real_sl_set_tag(viewport, routed.tags, routed.count, cmd_buffer);
}

sl::Result HookedSlSetTagForFrame(
    const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags,
    sl::CommandBuffer* cmd_buffer) {
  MarkStreamlineColorTagSubmission(tags, num_tags);
  CaptureStreamlineTags("slSetTagForFrame", tags, num_tags);
  const auto routed = RouteStreamlineColorTags(tags, num_tags);
  return real_sl_set_tag_for_frame(frame, viewport, routed.tags, routed.count, cmd_buffer);
}

const auto& GetStreamlineHooks() {
  static const std::array<renodx::utils::vtable::HookItem, 2> hooks = {
      renodx::utils::vtable::HookItem{
          "slSetTag",
          reinterpret_cast<void**>(&real_sl_set_tag),
          reinterpret_cast<void*>(&HookedSlSetTag),
      },
      renodx::utils::vtable::HookItem{
          "slSetTagForFrame",
          reinterpret_cast<void**>(&real_sl_set_tag_for_frame),
          reinterpret_cast<void*>(&HookedSlSetTagForFrame),
      },
  };
  return hooks;
}

void TryInstallStreamlineHook() {
  if (dlss_fg_hook_installed) return;

  auto* module = GetModuleHandleA("sl.interposer.dll");
  if (module == nullptr) {
    if (!dlss_fg_waiting_for_streamline_logged) {
      dlss_fg_waiting_for_streamline_logged = true;
      renodx::utils::log::w("DL2 DLSS FG: sl.interposer.dll is not loaded yet; tag hook will retry at Present.");
    }
    return;
  }

  if (renodx::utils::vtable::Hook(module, GetStreamlineHooks())) {
    dlss_fg_hook_installed = true;
    renodx::utils::log::i("DL2 DLSS FG: Streamline tag capture hook installed.");
  } else {
    renodx::utils::log::w("DL2 DLSS FG: Streamline tag hook was not installed.");
  }
}

void RemoveStreamlineHook() {
  if (!dlss_fg_hook_installed) return;
  auto* module = GetModuleHandleA("sl.interposer.dll");
  if (module != nullptr) renodx::utils::vtable::Unhook(module, GetStreamlineHooks());
  dlss_fg_hook_installed = false;
}
// Disabled by default. When armed from the Advanced diagnostic setting, this
// records graphics and compute shader hashes after the known late Gamma pass,
// ending at the very next Present.
// It does not inspect resources, descriptor tables, or command history.
float downstream_draw_capture = 0.f;
float downstream_transfer_capture = 0.f;
bool gamma_draw_audit_capture = false;
bool gamma_native_input_audit_capture = false;
constexpr uint32_t kGammaFrameAuditDrawCount = 8u;
constexpr size_t kDownstreamInputViewLimit = 64u;

enum class DownstreamTransferType : uint8_t {
  copy_resource,
  copy_texture_region,
  resolve_texture_region,
};

struct DownstreamTransfer {
  DownstreamTransferType type = DownstreamTransferType::copy_resource;
  uint64_t source = 0u;
  uint64_t dest = 0u;
  reshade::api::format source_format = reshade::api::format::unknown;
  reshade::api::format dest_format = reshade::api::format::unknown;
  uint32_t source_usage = 0u;
  uint32_t dest_usage = 0u;
  uint32_t source_flags = 0u;
  uint32_t dest_flags = 0u;
  uint64_t source_clone = 0u;
  uint64_t dest_clone = 0u;
  uint64_t source_proxy = 0u;
  uint64_t dest_proxy = 0u;
  bool source_clone_enabled = false;
  bool dest_clone_enabled = false;
  bool source_shared = false;
  bool dest_shared = false;
  bool source_is_swapchain = false;
  bool dest_is_swapchain = false;
  bool source_is_clone = false;
  bool dest_is_clone = false;
  uint32_t source_clone_usage = 0u;
  uint32_t dest_clone_usage = 0u;
  uint32_t source_clone_flags = 0u;
  uint32_t dest_clone_flags = 0u;
};

struct DownstreamTarget {
  uint64_t resource = 0u;
  uint64_t effective = 0u;
  reshade::api::format format = reshade::api::format::unknown;
  reshade::api::format effective_format = reshade::api::format::unknown;
  uint32_t width = 0u;
  uint32_t height = 0u;
  bool clone_enabled = false;
};

struct DownstreamDrawCaptureState {
  std::array<uint32_t, 16> hashes = {};
  std::array<bool, 16> is_compute = {};
  std::array<DownstreamTarget, 16> targets = {};
  std::array<DownstreamTarget, 16> inputs = {};
  std::array<DownstreamTransfer, 16> transfers = {};
  uint32_t count = 0u;
  uint32_t transfer_count = 0u;
  bool active = false;
  bool consumed = false;
  bool capture_commands = false;
  bool capture_transfers = false;
  uint64_t gamma_target = 0u;
  reshade::api::format gamma_target_format = reshade::api::format::unknown;
  uint64_t gamma_target_clone = 0u;
  reshade::api::format gamma_target_clone_format = reshade::api::format::unknown;
  bool gamma_target_clone_enabled = false;
  uint64_t gamma_target_view = 0u;
  uint64_t gamma_target_clone_view = 0u;
  bool gamma_target_clone_view_enabled = false;
  uint64_t gamma_target_effective = 0u;
  reshade::api::format gamma_target_effective_format = reshade::api::format::unknown;
};

struct GammaAuditResource {
  uint64_t resource = 0u;
  uint64_t clone = 0u;
  uint64_t effective = 0u;
  reshade::api::format format = reshade::api::format::unknown;
  reshade::api::format clone_format = reshade::api::format::unknown;
  reshade::api::format effective_format = reshade::api::format::unknown;
  uint32_t width = 0u;
  uint32_t height = 0u;
  bool clone_enabled = false;
  bool view_clone_enabled = false;
};

struct GammaDrawAudit {
  GammaAuditResource input = {};
  GammaAuditResource output = {};
  uint32_t input_from_draw = 0u;
  bool output_used_by_later_draw = false;
};

struct GammaDrawAuditState {
  std::array<GammaDrawAudit, 16> draws = {};
  uint32_t count = 0u;
  bool active = false;
  bool consumed = false;
};

struct GammaNativeInputAuditState {
  std::array<GammaAuditResource, 16> inputs = {};
  std::array<uint64_t, 16> native_views = {};
  GammaAuditResource output = {};
  uint64_t native_pixel_shader = 0u;
  uint64_t native_rtv = 0u;
  uint64_t native_command_context = 0u;
  uint64_t native_immediate_context = 0u;
  bool active = false;
  bool consumed = false;
};

DownstreamDrawCaptureState downstream_draw_capture_state = {};
GammaDrawAuditState gamma_draw_audit_state = {};
GammaNativeInputAuditState gamma_native_input_audit_state = {};
std::mutex downstream_draw_capture_mutex;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> downstream_capture_rtvs;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> downstream_capture_t0_views;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> gamma_audit_t0_views;

GammaAuditResource DescribeGammaAuditView(
    reshade::api::device* device,
    reshade::api::resource_view view) {
  GammaAuditResource result = {};
  if (device == nullptr || view.handle == 0u) return result;

  const auto resource = device->get_resource_from_view(view);
  if (resource.handle == 0u) return result;
  const auto desc = device->get_resource_desc(resource);
  result.resource = resource.handle;
  result.effective = resource.handle;
  result.format = desc.texture.format;
  result.effective_format = desc.texture.format;
  result.width = desc.texture.width;
  result.height = desc.texture.height;
  renodx::utils::resource::GetResourceInfo(resource, [&result](const renodx::utils::resource::ResourceInfo& info) {
    result.clone = info.clone.handle;
    result.clone_format = info.clone_desc.texture.format;
    result.clone_enabled = info.clone_enabled;
  });
  renodx::utils::resource::GetResourceViewInfo(view, [&result, device](const renodx::utils::resource::ResourceViewInfo& info) {
    result.view_clone_enabled = info.clone_enabled;
    if (!info.clone_enabled || info.clone.handle == 0u) return;
    const auto effective_resource = device->get_resource_from_view(info.clone);
    if (effective_resource.handle == 0u) return;
    const auto effective_desc = device->get_resource_desc(effective_resource);
    result.effective = effective_resource.handle;
    result.effective_format = effective_desc.texture.format;
  });
  return result;
}

GammaAuditResource DescribeNativeD3D11Resource(
    reshade::api::device* device,
    ID3D11Resource* native_resource) {
  GammaAuditResource result = {};
  if (device == nullptr || native_resource == nullptr) return result;

  const reshade::api::resource resource = {reinterpret_cast<uint64_t>(native_resource)};
  const auto desc = device->get_resource_desc(resource);
  result.resource = resource.handle;
  result.effective = resource.handle;
  result.format = desc.texture.format;
  result.effective_format = desc.texture.format;
  result.width = desc.texture.width;
  result.height = desc.texture.height;
  renodx::utils::resource::GetResourceInfo(resource, [&result](const renodx::utils::resource::ResourceInfo& info) {
    result.clone = info.clone.handle;
    result.clone_format = info.clone_desc.texture.format;
    result.clone_enabled = info.clone_enabled;
    if (info.clone_enabled && info.clone.handle != 0u) {
      result.effective = info.clone.handle;
      result.effective_format = info.clone_desc.texture.format;
    }
  });
  return result;
}

void OnGammaAuditPushDescriptors(
    reshade::api::command_list* cmd_list,
    reshade::api::shader_stage stages,
    reshade::api::pipeline_layout layout,
    uint32_t layout_param,
    const reshade::api::descriptor_table_update& update) {
  const bool capture_gamma_input = gamma_draw_audit_capture;
  // Bindings are normally established before the Gamma draw, so start this
  // bounded cache when the one-shot button is armed rather than afterwards.
  const bool capture_downstream_inputs = downstream_draw_capture >= 0.5f
      && !downstream_draw_capture_state.consumed;
  if ((!capture_gamma_input && !capture_downstream_inputs) || update.count == 0u
      || !renodx::utils::bitwise::HasFlag(stages, reshade::api::shader_stage::pixel)) {
    return;
  }
  switch (update.type) {
    case reshade::api::descriptor_type::sampler_with_resource_view:
    case reshade::api::descriptor_type::shader_resource_view:
    case reshade::api::descriptor_type::buffer_shader_resource_view:
      break;
    default:
      return;
  }

  uint32_t register_index = 0u;
  bool found_register_index = false;
  renodx::utils::pipeline_layout::GetPipelineLayoutData(layout, [&](const auto* layout_data) {
    if (layout_param >= layout_data->params.size()) return;
    const auto& param = layout_data->params[layout_param];
    if (param.type == reshade::api::pipeline_layout_param_type::descriptor_table) {
      if (param.descriptor_table.count != 1u) return;
      register_index = param.descriptor_table.ranges[0].dx_register_index;
      found_register_index = true;
    } else if (param.type == reshade::api::pipeline_layout_param_type::push_descriptors) {
      register_index = param.push_descriptors.dx_register_index;
      found_register_index = true;
    }
  });
  // DL2 uses the D3D11 immediate-context path, which can omit the ReShade
  // layout metadata even though update.binding is the native shader register.
  if (!found_register_index && cmd_list->get_device()->get_api() == reshade::api::device_api::d3d11) {
    found_register_index = true;
  }
  if (!found_register_index) return;

  for (uint32_t index = 0u; index < update.count; ++index) {
    if (register_index + update.binding + index != 0u) continue;
    const auto view = renodx::utils::descriptor::GetResourceViewFromDescriptorUpdate(update, index);
    std::scoped_lock lock(downstream_draw_capture_mutex);
    if (capture_gamma_input) gamma_audit_t0_views[cmd_list] = view;
    if (capture_downstream_inputs) {
      const auto existing = downstream_capture_t0_views.find(cmd_list);
      if (existing != downstream_capture_t0_views.end()
          || downstream_capture_t0_views.size() < kDownstreamInputViewLimit) {
        downstream_capture_t0_views[cmd_list] = view;
      }
    }
    return;
  }
}

void OnDownstreamBindRenderTargets(
    reshade::api::command_list* cmd_list,
    uint32_t count,
    const reshade::api::resource_view* rtvs,
  reshade::api::resource_view) {
  std::scoped_lock lock(downstream_draw_capture_mutex);
  // Keep the current target while any one-shot diagnostic is armed. The
  // command-candidate capture previously omitted downstream_draw_capture
  // here, so it could report post-Gamma shader hashes but had already erased
  // every target binding before those draws executed.
  const bool keep_target_binding = downstream_draw_capture >= 0.5f
      || downstream_transfer_capture >= 0.5f
      || gamma_draw_audit_capture
      || gamma_native_input_audit_capture
      || (downstream_draw_capture_state.active && !downstream_draw_capture_state.consumed);
  if (!keep_target_binding || count == 0u || rtvs == nullptr) {
    downstream_capture_rtvs.erase(cmd_list);
    return;
  }
  downstream_capture_rtvs[cmd_list] = rtvs[0];
}

inline constexpr auto OnGammaDrawAudit = []<typename Context>(Context& context)
    -> renodx::utils::command_action::CallbackResult<Context> {
  if (context.IsDispatch() || (!gamma_draw_audit_capture && !gamma_native_input_audit_capture)) return {};

  std::scoped_lock lock(downstream_draw_capture_mutex);
  if (gamma_native_input_audit_capture && !gamma_native_input_audit_state.consumed) {
    auto* device = context.cmd_list->get_device();
    gamma_native_input_audit_state.native_command_context = context.cmd_list->get_native();
    if (device != nullptr && device->get_api() == reshade::api::device_api::d3d11) {
      auto* native_device = reinterpret_cast<ID3D11Device*>(device->get_native());
      ID3D11DeviceContext* native_context = nullptr;
      if (native_device != nullptr) native_device->GetImmediateContext(&native_context);
      gamma_native_input_audit_state.native_immediate_context = reinterpret_cast<uint64_t>(native_context);
      if (native_context != nullptr) {
        ID3D11PixelShader* native_pixel_shader = nullptr;
        native_context->PSGetShader(&native_pixel_shader, nullptr, nullptr);
        gamma_native_input_audit_state.native_pixel_shader = reinterpret_cast<uint64_t>(native_pixel_shader);
        if (native_pixel_shader != nullptr) native_pixel_shader->Release();

        ID3D11RenderTargetView* native_rtv = nullptr;
        native_context->OMGetRenderTargets(1u, &native_rtv, nullptr);
        gamma_native_input_audit_state.native_rtv = reinterpret_cast<uint64_t>(native_rtv);
        if (native_rtv != nullptr) {
          ID3D11Resource* native_resource = nullptr;
          native_rtv->GetResource(&native_resource);
          if (native_resource != nullptr) {
            gamma_native_input_audit_state.output = DescribeNativeD3D11Resource(device, native_resource);
            native_resource->Release();
          }
          native_rtv->Release();
        }

        std::array<ID3D11ShaderResourceView*, 16> native_views = {};
        native_context->PSGetShaderResources(0u, static_cast<UINT>(native_views.size()), native_views.data());
        for (uint32_t index = 0u; index < native_views.size(); ++index) {
          if (native_views[index] == nullptr) continue;
          gamma_native_input_audit_state.native_views[index] = reinterpret_cast<uint64_t>(native_views[index]);
          ID3D11Resource* native_resource = nullptr;
          native_views[index]->GetResource(&native_resource);
          if (native_resource != nullptr) {
            gamma_native_input_audit_state.inputs[index] = DescribeNativeD3D11Resource(device, native_resource);
            native_resource->Release();
          }
          native_views[index]->Release();
        }
        native_context->Release();
      }
    }
    gamma_native_input_audit_state.active = true;
  }
  auto& audit = gamma_draw_audit_state;
  if (audit.consumed || audit.count >= audit.draws.size()) return {};

  auto* device = context.cmd_list->get_device();
  if (device == nullptr) return {};
  const auto input = gamma_audit_t0_views.find(context.cmd_list);
  const auto output = downstream_capture_rtvs.find(context.cmd_list);
  if (output == downstream_capture_rtvs.end()) return {};

  auto& draw = audit.draws[audit.count];
  draw.input = input != gamma_audit_t0_views.end()
      ? DescribeGammaAuditView(device, input->second)
      : GammaAuditResource{};
  draw.output = DescribeGammaAuditView(device, output->second);
  for (uint32_t index = 0u; index < audit.count; ++index) {
    const auto& prior_output = audit.draws[index].output;
    if ((draw.input.resource != 0u && draw.input.resource == prior_output.resource)
        || (draw.input.effective != 0u && draw.input.effective == prior_output.effective)) {
      draw.input_from_draw = index + 1u;
      audit.draws[index].output_used_by_later_draw = true;
      break;
    }
  }
  ++audit.count;
  audit.active = true;
  return {};
};

inline constexpr auto OnDownstreamDrawCapture = []<typename Context>(Context& context)
    -> renodx::utils::command_action::CallbackResult<Context> {
  auto& capture = downstream_draw_capture_state;
  std::scoped_lock lock(downstream_draw_capture_mutex);
  const bool capture_commands = downstream_draw_capture >= 0.5f;
  const bool capture_transfers = downstream_transfer_capture >= 0.5f;
  if (!capture_commands && !capture_transfers) {
    capture = {};
    downstream_capture_t0_views.clear();
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
      capture.count = 0u;
      capture.transfer_count = 0u;
      capture.targets.fill({});
      capture.inputs.fill({});
      // Keep t0 bindings collected while the one-shot capture was armed.
      capture.active = true;
      capture.capture_commands = capture_commands;
      capture.capture_transfers = capture_transfers;
      if (capture_commands || capture_transfers) {
        const auto target = downstream_capture_rtvs.find(context.cmd_list);
        if (target != downstream_capture_rtvs.end()) {
          auto* device = context.cmd_list->get_device();
          const auto resource = device->get_resource_from_view(target->second);
          const auto desc = device->get_resource_desc(resource);
          capture.gamma_target = resource.handle;
          capture.gamma_target_format = desc.texture.format;
          capture.gamma_target_view = target->second.handle;
          if (capture_transfers) renodx::utils::resource::upgrade::ArmCopyResourceAudit(resource);
          renodx::utils::resource::GetResourceInfo(resource, [&capture](const renodx::utils::resource::ResourceInfo& info) {
            capture.gamma_target_clone = info.clone.handle;
            capture.gamma_target_clone_format = info.clone_desc.texture.format;
            capture.gamma_target_clone_enabled = info.clone_enabled;
          });
          renodx::utils::resource::GetResourceViewInfo(
              target->second,
              [&capture, device](const renodx::utils::resource::ResourceViewInfo& info) {
                capture.gamma_target_clone_view = info.clone.handle;
                capture.gamma_target_clone_view_enabled = info.clone_enabled;
                if (!info.clone_enabled || info.clone.handle == 0u) return;
                const auto effective_resource = device->get_resource_from_view(info.clone);
                const auto effective_desc = device->get_resource_desc(effective_resource);
                capture.gamma_target_effective = effective_resource.handle;
                capture.gamma_target_effective_format = effective_desc.texture.format;
              });
        }
      }
    }
    return {};
  }

  if (!capture.capture_commands) return {};
  if (shader_hash == 0u) return {};

  // DL2 records late work across multiple command lists. Stay bounded by the
  // next Present, but keep a small unique candidate set rather than assuming
  // CPU command-list recording order is the GPU compositing order.
  for (uint32_t index = 0u; index < capture.count; ++index) {
    if (capture.hashes[index] == shader_hash && capture.is_compute[index] == is_compute) return {};
  }
  if (capture.count < capture.hashes.size()) {
    capture.hashes[capture.count] = shader_hash;
    capture.is_compute[capture.count] = is_compute;
    if (!is_compute) {
      const auto target = downstream_capture_rtvs.find(context.cmd_list);
      if (target != downstream_capture_rtvs.end()) {
        const auto resource = DescribeGammaAuditView(context.cmd_list->get_device(), target->second);
        capture.targets[capture.count] = {
            .resource = resource.resource,
            .effective = resource.effective,
            .format = resource.format,
            .effective_format = resource.effective_format,
            .width = resource.width,
            .height = resource.height,
            .clone_enabled = resource.view_clone_enabled,
        };
      }
      const auto input = downstream_capture_t0_views.find(context.cmd_list);
      if (input != downstream_capture_t0_views.end()) {
        const auto resource = DescribeGammaAuditView(context.cmd_list->get_device(), input->second);
        capture.inputs[capture.count] = {
            .resource = resource.resource,
            .effective = resource.effective,
            .format = resource.format,
            .effective_format = resource.effective_format,
            .width = resource.width,
            .height = resource.height,
            .clone_enabled = resource.view_clone_enabled,
        };
      }
    }
    ++capture.count;
  }
  return {};
};

void RecordDownstreamTransfer(
    DownstreamTransferType type,
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest) {
  std::scoped_lock lock(downstream_draw_capture_mutex);
  auto& capture = downstream_draw_capture_state;
  if (!capture.active || capture.consumed || !capture.capture_transfers) return;
  if (capture.transfer_count >= capture.transfers.size()) return;

  auto* device = cmd_list->get_device();
  if (device == nullptr) return;
  const auto source_desc = device->get_resource_desc(source);
  const auto dest_desc = device->get_resource_desc(dest);
  DownstreamTransfer transfer = {
      .type = type,
      .source = source.handle,
      .dest = dest.handle,
      .source_format = source_desc.texture.format,
      .dest_format = dest_desc.texture.format,
      .source_usage = static_cast<uint32_t>(source_desc.usage),
      .dest_usage = static_cast<uint32_t>(dest_desc.usage),
      .source_flags = static_cast<uint32_t>(source_desc.flags),
      .dest_flags = static_cast<uint32_t>(dest_desc.flags),
  };
  renodx::utils::resource::GetResourceInfo(source, [&transfer](const renodx::utils::resource::ResourceInfo& info) {
    transfer.source_clone = info.clone.handle;
    transfer.source_proxy = info.proxy_resource.handle;
    transfer.source_clone_enabled = info.clone_enabled;
    transfer.source_shared = info.shared_handle != nullptr;
    transfer.source_is_swapchain = info.is_swap_chain;
    transfer.source_is_clone = info.is_clone;
    transfer.source_clone_usage = static_cast<uint32_t>(info.clone_desc.usage);
    transfer.source_clone_flags = static_cast<uint32_t>(info.clone_desc.flags);
  });
  renodx::utils::resource::GetResourceInfo(dest, [&transfer](const renodx::utils::resource::ResourceInfo& info) {
    transfer.dest_clone = info.clone.handle;
    transfer.dest_proxy = info.proxy_resource.handle;
    transfer.dest_clone_enabled = info.clone_enabled;
    transfer.dest_shared = info.shared_handle != nullptr;
    transfer.dest_is_swapchain = info.is_swap_chain;
    transfer.dest_is_clone = info.is_clone;
    transfer.dest_clone_usage = static_cast<uint32_t>(info.clone_desc.usage);
    transfer.dest_clone_flags = static_cast<uint32_t>(info.clone_desc.flags);
  });

  for (uint32_t index = 0u; index < capture.transfer_count; ++index) {
    const auto& existing = capture.transfers[index];
    if (existing.type == transfer.type && existing.source == transfer.source && existing.dest == transfer.dest) {
      return;
    }
  }
  if (capture.transfer_count < capture.transfers.size()) {
    capture.transfers[capture.transfer_count++] = transfer;
  }
}

bool OnDownstreamCopyResource(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest) {
  RecordDownstreamTransfer(DownstreamTransferType::copy_resource, cmd_list, source, dest);
  return false;
}

bool OnDownstreamCopyTextureRegion(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    uint32_t,
    const reshade::api::subresource_box*,
    reshade::api::resource dest,
    uint32_t,
    const reshade::api::subresource_box*,
    reshade::api::filter_mode) {
  RecordDownstreamTransfer(DownstreamTransferType::copy_texture_region, cmd_list, source, dest);
  return false;
}

bool OnDownstreamResolveTextureRegion(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    uint32_t,
    const reshade::api::subresource_box*,
    reshade::api::resource dest,
    uint32_t,
    uint32_t,
    uint32_t,
    uint32_t,
    reshade::api::format) {
  RecordDownstreamTransfer(DownstreamTransferType::resolve_texture_region, cmd_list, source, dest);
  return false;
}

void OnDownstreamDrawCapturePresent(
    reshade::api::command_queue* queue,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect*,
    const reshade::api::rect*,
    uint32_t,
    const reshade::api::rect*) {
  TryInstallStreamlineHook();

  if (dlss_fg_skip_generated_proxy >= 0.5f) {
    const uint32_t tag_serial = dlss_fg_color_tag_serial.load(std::memory_order_acquire);
    if (tag_serial != 0u && tag_serial != dlss_fg_last_present_tag_serial) {
      dlss_fg_last_present_tag_serial = tag_serial;
      renodx::mods::swapchain::v1::skip_next_proxy_draw.store(true, std::memory_order_release);
    }
  } else {
    dlss_fg_last_present_tag_serial = dlss_fg_color_tag_serial.load(std::memory_order_relaxed);
    renodx::mods::swapchain::v1::skip_next_proxy_draw.store(false, std::memory_order_release);
  }

  std::scoped_lock lock(downstream_draw_capture_mutex);
  if (dlss_fg_present_cadence_capture) {
    static std::array<uint32_t, 16> tag_serials = {};
    static std::array<uint64_t, 16> back_buffers = {};
    static uint32_t present_count = 0u;

    tag_serials[present_count] = dlss_fg_color_tag_serial.load(std::memory_order_relaxed);
    back_buffers[present_count] = swapchain->get_current_back_buffer().handle;
    ++present_count;

    if (present_count >= tag_serials.size()) {
      std::stringstream stream;
      stream << "DL2 DLSS FG present cadence (" << present_count << "):";
      for (uint32_t index = 0u; index < present_count; ++index) {
        stream << " #" << std::dec << (index + 1u) << " tag=" << tag_serials[index]
               << " backbuffer=0x" << std::hex << std::uppercase << back_buffers[index];
      }
      reshade::log::message(reshade::log::level::info, stream.str().c_str());
      dlss_fg_present_cadence_capture = false;
      present_count = 0u;
    }
  }
  auto& gamma_audit = gamma_draw_audit_state;
  if (gamma_audit.active && gamma_audit.count >= kGammaFrameAuditDrawCount) {
    std::stringstream stream;
    stream << "DL2 Gamma frame audit (" << gamma_audit.count << "):";
    for (uint32_t index = 0u; index < gamma_audit.count; ++index) {
      const auto& draw = gamma_audit.draws[index];
      stream << " #" << std::dec << (index + 1u)
             << " t0(0x" << std::hex << std::uppercase << draw.input.resource
             << "," << static_cast<uint32_t>(draw.input.format)
             << "=>0x" << draw.input.effective
             << "," << static_cast<uint32_t>(draw.input.effective_format)
             << ",rclone=0x" << draw.input.clone
             << "," << static_cast<uint32_t>(draw.input.clone_format)
             << ",clone=" << (draw.input.view_clone_enabled ? "on" : "off")
             << "," << std::dec << draw.input.width << "x" << draw.input.height << ")"
             << " rtv(0x" << std::hex << draw.output.resource
             << "," << static_cast<uint32_t>(draw.output.format)
             << "=>0x" << draw.output.effective
             << "," << static_cast<uint32_t>(draw.output.effective_format)
             << ",rclone=0x" << draw.output.clone
             << "," << static_cast<uint32_t>(draw.output.clone_format)
             << ",clone=" << (draw.output.view_clone_enabled ? "on" : "off")
             << "," << std::dec << draw.output.width << "x" << draw.output.height << ")";
      if (draw.input_from_draw != 0u) stream << " input_from=#" << draw.input_from_draw;
      if (draw.output_used_by_later_draw) stream << " output_used_later";
    }
    reshade::log::message(reshade::log::level::info, stream.str().c_str());
    gamma_draw_audit_capture = false;
    gamma_audit.active = false;
    gamma_audit.consumed = true;
  }
  auto& native_input_audit = gamma_native_input_audit_state;
  if (native_input_audit.active) {
    std::stringstream stream;
    const auto& output = native_input_audit.output;
    stream << "DL2 Gamma native binding audit: ps(0x" << std::hex << std::uppercase
           << native_input_audit.native_pixel_shader << ") contexts(command=0x"
           << native_input_audit.native_command_context << ", immediate=0x"
           << native_input_audit.native_immediate_context << ") rtv(0x" << native_input_audit.native_rtv
           << ", resource=0x" << output.resource << ", " << static_cast<uint32_t>(output.format)
           << " => 0x" << output.effective << ", " << static_cast<uint32_t>(output.effective_format)
           << ") slots:";
    bool found_input = false;
    for (uint32_t index = 0u; index < native_input_audit.inputs.size(); ++index) {
      const auto& input = native_input_audit.inputs[index];
      if (native_input_audit.native_views[index] == 0u) continue;
      found_input = true;
      stream << " t" << std::dec << index << "(srv=0x" << std::hex << native_input_audit.native_views[index]
             << ", resource=0x" << input.resource << ", " << static_cast<uint32_t>(input.format)
             << " => 0x" << input.effective << ", " << static_cast<uint32_t>(input.effective_format)
             << ", rclone=0x" << input.clone << ", " << static_cast<uint32_t>(input.clone_format)
             << ", clone=" << (input.clone_enabled ? "on" : "off") << ", " << std::dec
             << input.width << "x" << input.height << ")";
    }
    if (!found_input) stream << " none";
    reshade::log::message(reshade::log::level::info, stream.str().c_str());
    gamma_native_input_audit_capture = false;
    native_input_audit.active = false;
    native_input_audit.consumed = true;
  }
  auto& capture = downstream_draw_capture_state;
  if (!capture.active) return;

  if (capture.capture_commands) {
    std::stringstream stream;
    stream << "DL2 same-Present command candidates after 0xAD085E81 (" << capture.count << "):";
    for (uint32_t index = 0u; index < capture.count; ++index) {
      stream << " " << (capture.is_compute[index] ? "CS" : "PS") << ":0x"
             << std::hex << std::uppercase << capture.hashes[index];
      const auto& target = capture.targets[index];
      if (target.resource != 0u) {
        stream << "->rtv(0x" << target.resource << ", "
               << static_cast<uint32_t>(target.format) << "=>0x" << target.effective << ", "
               << static_cast<uint32_t>(target.effective_format) << ", clone="
               << (target.clone_enabled ? "on" : "off") << ", " << std::dec
               << target.width << "x" << target.height << ")";
      }
      const auto& input = capture.inputs[index];
      if (input.resource != 0u) {
        const bool reads_gamma = input.resource == capture.gamma_target
            || input.resource == capture.gamma_target_clone
            || input.resource == capture.gamma_target_effective
            || input.effective == capture.gamma_target
            || input.effective == capture.gamma_target_clone
            || input.effective == capture.gamma_target_effective;
        stream << "<-t0(0x" << input.resource << ", " << static_cast<uint32_t>(input.format)
               << "=>0x" << input.effective << ", " << static_cast<uint32_t>(input.effective_format)
               << ", gamma=" << (reads_gamma ? "yes" : "no") << ")";
      }
    }
    reshade::log::message(reshade::log::level::info, stream.str().c_str());
  }
  if (capture.capture_transfers) {
    std::stringstream stream;
    stream << "DL2 post-Gamma transfers (" << capture.transfer_count << "):";
    for (uint32_t index = 0u; index < capture.transfer_count; ++index) {
      const auto& transfer = capture.transfers[index];
      const char* type = transfer.type == DownstreamTransferType::copy_resource ? "CopyResource"
                         : transfer.type == DownstreamTransferType::copy_texture_region ? "CopyTexture"
                                                                                         : "ResolveTexture";
      stream << " " << type << "(0x" << std::hex << std::uppercase << transfer.source << ", "
             << static_cast<uint32_t>(transfer.source_format) << " => 0x" << transfer.dest << ", "
             << static_cast<uint32_t>(transfer.dest_format)
             << ", src_usage=0x" << transfer.source_usage
             << ", dst_usage=0x" << transfer.dest_usage
             << ", src_flags=0x" << transfer.source_flags
             << ", dst_flags=0x" << transfer.dest_flags
             << ", src_clone=0x" << transfer.source_clone
             << ", dst_clone=0x" << transfer.dest_clone
             << ", src_proxy=0x" << transfer.source_proxy
             << ", dst_proxy=0x" << transfer.dest_proxy
             << ", src_clone_enabled=" << (transfer.source_clone_enabled ? "yes" : "no")
             << ", dst_clone_enabled=" << (transfer.dest_clone_enabled ? "yes" : "no")
             << ", src_shared=" << (transfer.source_shared ? "yes" : "no")
             << ", dst_shared=" << (transfer.dest_shared ? "yes" : "no")
             << ", src_swapchain=" << (transfer.source_is_swapchain ? "yes" : "no")
             << ", dst_swapchain=" << (transfer.dest_is_swapchain ? "yes" : "no")
             << ", src_is_clone=" << (transfer.source_is_clone ? "yes" : "no")
             << ", dst_is_clone=" << (transfer.dest_is_clone ? "yes" : "no")
             << ", src_clone_usage=0x" << transfer.source_clone_usage
             << ", dst_clone_usage=0x" << transfer.dest_clone_usage
             << ", src_clone_flags=0x" << transfer.source_clone_flags
             << ", dst_clone_flags=0x" << transfer.dest_clone_flags << ")";
    }
    if (queue != nullptr && swapchain != nullptr) {
      const auto back_buffer = swapchain->get_current_back_buffer();
      const auto back_buffer_desc = queue->get_device()->get_resource_desc(back_buffer);
      stream << " present_backbuffer(0x" << std::hex << std::uppercase << back_buffer.handle
             << ", " << std::dec << back_buffer_desc.texture.width << "x"
             << back_buffer_desc.texture.height << ", "
             << static_cast<uint32_t>(back_buffer_desc.texture.format) << ")";
    }
    if (capture.gamma_target != 0u) {
      const auto effective_copy = renodx::utils::resource::upgrade::GetCopyResourceAudit();
      bool copied_from_gamma_target = false;
      bool copied_from_gamma_clone = false;
      bool copied_from_effective_target = false;
      for (uint32_t index = 0u; index < capture.transfer_count; ++index) {
        copied_from_gamma_target |= capture.transfers[index].source == capture.gamma_target;
        copied_from_gamma_clone |= capture.transfers[index].source == capture.gamma_target_clone;
        copied_from_effective_target |= capture.transfers[index].source == capture.gamma_target_effective;
      }
      stream << " gamma_target(0x" << std::hex << std::uppercase << capture.gamma_target << ", "
             << static_cast<uint32_t>(capture.gamma_target_format) << ", copied="
             << (copied_from_gamma_target ? "yes" : "no") << ")"
             << " gamma_clone(0x" << capture.gamma_target_clone << ", "
             << static_cast<uint32_t>(capture.gamma_target_clone_format) << ", enabled="
             << (capture.gamma_target_clone_enabled ? "yes" : "no") << ", copied="
             << (copied_from_gamma_clone ? "yes" : "no") << ")"
             << " gamma_rtv(0x" << capture.gamma_target_view << " => 0x"
             << capture.gamma_target_clone_view << ", enabled="
             << (capture.gamma_target_clone_view_enabled ? "yes" : "no") << ")"
             << " effective_rtv(0x" << capture.gamma_target_effective << ", "
             << static_cast<uint32_t>(capture.gamma_target_effective_format) << ", copied="
             << (copied_from_effective_target ? "yes" : "no") << ")"
             << " effective_copy(" << (effective_copy.matched ? "matched" : "missing")
             << ",0x" << effective_copy.effective_source << ", "
             << static_cast<uint32_t>(effective_copy.effective_source_format)
             << " => 0x" << effective_copy.effective_dest << ", "
             << static_cast<uint32_t>(effective_copy.effective_dest_format) << ")";
    } else {
      stream << " gamma_target(unavailable)";
    }
    reshade::log::message(reshade::log::level::info, stream.str().c_str());
  }

  // One capture per manual arm. This leaves no per-frame work afterwards and
  // prevents draw recording from leaking into the next frame's G-buffer.
  downstream_draw_capture = 0.f;
  downstream_transfer_capture = 0.f;
  renodx::utils::settings::UpdateSetting("CaptureDownstreamDraws", 0.f);
  renodx::utils::settings::UpdateSetting("CaptureDownstreamTransfers", 0.f);
  capture.active = false;
  capture.consumed = true;
  downstream_capture_rtvs.clear();
  downstream_capture_t0_views.clear();
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
        .key = "FrameGenerationCompatibility",
        .binding = &renodx::mods::swapchain::v1::copy_swapchain_back_buffer_before_proxy,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .can_reset = false,
        .label = "DLSS Frame Generation Compatibility",
        .section = "Compatibility",
        .tooltip = "For DLSS Frame Generation only. Keeps DL2's final FP16 handoff on the real current backbuffer, then synchronizes it to the RenoDX proxy clone immediately before Present. This is a runtime A/B switch; it does not enable dumping or tracing.",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "DLSSFGUseTaggedClone",
        .binding = &dlss_fg_tag_clone,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .can_reset = false,
        .label = "DLSS FG Use Tagged FP16 Clone",
        .section = "Compatibility",
        .tooltip = "Experimental. Routes only DLSS FG's captured HUDLessColor and UIColorAndAlpha tags to the matching RenoDX FP16 clone, with clone format and dimensions preserved in the Streamline resource metadata.",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "DLSSFGSkipGeneratedProxy",
        .binding = &dlss_fg_skip_generated_proxy,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .can_reset = false,
        .label = "DLSS FG Skip Generated Proxy",
        .section = "Compatibility",
        .tooltip = "Experimental. After a new Streamline HUDLessColor tag, skips RenoDX's final proxy draw once for the immediately following DLSS-generated Present. Resource upgrades and all other Present work remain active.",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DLSS FG Streamline Tags",
        .section = "Debug",
        .tooltip = "One-shot: logs the next Streamline slSetTag/slSetTagForFrame resource tags, including native handles and RenoDX clone state. No dumping, readback, or resource interception.",
        .on_click = []() {
          dlss_fg_tag_capture = true;
          return false;
        },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DLSS FG Present Cadence (16)",
        .section = "Debug",
        .tooltip = "One-shot: records 16 Present events as color-tag serial plus current backbuffer handle. It does not capture draws, descriptors, resources, or GPU data.",
        .on_click = []() {
          dlss_fg_present_cadence_capture = true;
          return false;
        },
        .is_visible = []() { return current_settings_mode >= 2; },
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
        .labels = {"Off", "HDR Input Range", "Neutral SDR", "Graded SDR", "RenoDRT Output", "Output Probe (500-nit red)", "Scene Probe (Peak white)", "Output Luminance Ladder", "Raw Output Ladder", "Late LUT Output Ladder", "Source t0 Range", "Auto Exposure t1", "Bypass Late Gamma (Test)", "LUT Output Constant (500-nit white)", "Gamma Output Constant (500-nit white)", "Final Proxy Constant (500-nit white)", "Gamma Input t0 Range", "Gamma Power cb0", "Stability Probe (4 stages; top bypass, bottom Gamma)"},
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "CaptureDownstreamDraws",
        .binding = &downstream_draw_capture,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .can_reset = false,
        .label = "Capture Post-Gamma Command Candidates",
        .section = "Debug",
        .tooltip = "One-shot, records up to 16 unique graphics or compute shader hashes after 0xAD085E81 until the next Present, then turns itself off. No resource tracing or dumping.",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "CaptureDownstreamTransfers",
        .binding = &downstream_transfer_capture,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .can_reset = false,
        .label = "Capture Post-Gamma Transfers",
        .section = "Debug",
        .tooltip = "One-shot, records up to 16 unique copy or resolve operations after 0xAD085E81 until the next Present. It records only resource handles and formats, with no readback or interception.",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture Gamma Targets (8 frames)",
        .section = "Debug",
        .tooltip = "One-shot, records eight consecutive 0xAD085E81 draws before emitting one report. Use it to detect per-frame render-target or clone alternation behind highlight flicker. No readback or resource dump.",
        .on_click = []() {
          gamma_draw_audit_capture = true;
          gamma_draw_audit_state = {};
          return false;
        },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture Gamma Bindings (D3D11)",
        .section = "Debug",
        .tooltip = "One-shot D3D11-only audit. Reads the bound pixel shader, first RTV, and non-null pixel-SRV slots t0-t15 for 0xAD085E81 once, then stops at Present. Records only handles, formats, sizes, and clone state; no texture readback or resource interception.",
        .on_click = []() {
          gamma_native_input_audit_capture = true;
          gamma_native_input_audit_state = {};
          return false;
        },
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
      {"FrameGenerationCompatibility", 0.f},
      {"DLSSFGUseTaggedClone", 0.f},
      {"DLSSFGSkipGeneratedProxy", 0.f},
      {"DebugMode", 0.f},
      {"CaptureDownstreamDraws", 0.f},
      {"CaptureDownstreamTransfers", 0.f},
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

  TryInstallStreamlineHook();
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
      renodx::utils::command_action::Register(
          OnGammaDrawAudit,
          {.shader_hash = 0xAD085E81u,
           .command_types = renodx::utils::command_action::COMMAND_TYPE_DIRECT_DRAW});
      reshade::register_event<reshade::addon_event::copy_resource>(OnDownstreamCopyResource);
      reshade::register_event<reshade::addon_event::copy_texture_region>(OnDownstreamCopyTextureRegion);
      reshade::register_event<reshade::addon_event::resolve_texture_region>(OnDownstreamResolveTextureRegion);
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnDownstreamBindRenderTargets);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnGammaAuditPushDescriptors);

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

      // Upgrade targets: R8G8B8A8 -> R16G16B16A16_FLOAT.
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
      renodx::utils::command_action::Unregister(OnGammaDrawAudit);
      reshade::unregister_event<reshade::addon_event::present>(OnDownstreamDrawCapturePresent);
      reshade::unregister_event<reshade::addon_event::copy_resource>(OnDownstreamCopyResource);
      reshade::unregister_event<reshade::addon_event::copy_texture_region>(OnDownstreamCopyTextureRegion);
      reshade::unregister_event<reshade::addon_event::resolve_texture_region>(OnDownstreamResolveTextureRegion);
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnDownstreamBindRenderTargets);
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnGammaAuditPushDescriptors);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      RemoveStreamlineHook();
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  // Register after the swapchain proxy so the one-shot capture includes any
  // proxy copy/resolve work issued from its own Present callback.
  if (fdw_reason == DLL_PROCESS_ATTACH) {
    reshade::register_event<reshade::addon_event::present>(OnDownstreamDrawCapturePresent);
  }

  return TRUE;
}
