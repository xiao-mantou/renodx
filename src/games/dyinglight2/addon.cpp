/*
 * Copyright (C) 2025 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#include <array>
#include <d3d11.h>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <embed/shaders.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/descriptor.hpp"
#include "../../utils/pipeline_layout.hpp"
#include "../../utils/resource.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

// Disabled by default. When armed from the Advanced diagnostic setting, this
// records graphics and compute shader hashes after the known late Gamma pass,
// ending at the very next Present.
// It does not inspect resources, descriptor tables, or command history.
float downstream_draw_capture = 0.f;
float downstream_transfer_capture = 0.f;
bool gamma_draw_audit_capture = false;
bool gamma_native_input_audit_capture = false;
constexpr uint32_t kGammaFrameAuditDrawCount = 8u;

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
};

struct DownstreamDrawCaptureState {
  std::array<uint32_t, 16> hashes = {};
  std::array<bool, 16> is_compute = {};
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
  if (!gamma_draw_audit_capture || update.count == 0u
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
    gamma_audit_t0_views[cmd_list] = view;
    return;
  }
}

void OnDownstreamBindRenderTargets(
    reshade::api::command_list* cmd_list,
    uint32_t count,
    const reshade::api::resource_view* rtvs,
  reshade::api::resource_view) {
  std::scoped_lock lock(downstream_draw_capture_mutex);
  if ((downstream_draw_capture_state.consumed && downstream_transfer_capture >= 0.5f)
      || (downstream_transfer_capture < 0.5f && !gamma_draw_audit_capture)
      || count == 0u || rtvs == nullptr) {
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
      capture.active = true;
      capture.capture_commands = capture_commands;
      capture.capture_transfers = capture_transfers;
      if (capture_transfers) {
        const auto target = downstream_capture_rtvs.find(context.cmd_list);
        if (target != downstream_capture_rtvs.end()) {
          auto* device = context.cmd_list->get_device();
          const auto resource = device->get_resource_from_view(target->second);
          const auto desc = device->get_resource_desc(resource);
          capture.gamma_target = resource.handle;
          capture.gamma_target_format = desc.texture.format;
          capture.gamma_target_view = target->second.handle;
          renodx::utils::resource::upgrade::ArmCopyResourceAudit(resource);
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
  const DownstreamTransfer transfer = {
      .type = type,
      .source = source.handle,
      .dest = dest.handle,
      .source_format = source_desc.texture.format,
      .dest_format = dest_desc.texture.format,
  };

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
  std::scoped_lock lock(downstream_draw_capture_mutex);
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
             << static_cast<uint32_t>(transfer.dest_format) << ")";
    }
    if (queue != nullptr && swapchain != nullptr) {
      const auto back_buffer_desc =
          queue->get_device()->get_resource_desc(swapchain->get_current_back_buffer());
      stream << " present_backbuffer(" << std::dec << back_buffer_desc.texture.width << "x"
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
      renodx::utils::command_action::Unregister(OnGammaDrawAudit);
      reshade::unregister_event<reshade::addon_event::present>(OnDownstreamDrawCapturePresent);
      reshade::unregister_event<reshade::addon_event::copy_resource>(OnDownstreamCopyResource);
      reshade::unregister_event<reshade::addon_event::copy_texture_region>(OnDownstreamCopyTextureRegion);
      reshade::unregister_event<reshade::addon_event::resolve_texture_region>(OnDownstreamResolveTextureRegion);
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnDownstreamBindRenderTargets);
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnGammaAuditPushDescriptors);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
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
