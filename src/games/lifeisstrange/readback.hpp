/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

#include <include/reshade.hpp>

#include "../../utils/resource.hpp"
#include "../../utils/resource_upgrade.hpp"
#include "../../utils/swapchain.hpp"

namespace lifeisstrange::readback {

struct ReadbackConfig {
  // These fields make the helper reusable for another draw/resource without changing its readback logic.
  std::uint32_t shader_hash = 0x06A2A81D;
  std::uint32_t render_target_index = 0u;
  std::uint32_t mip_level = 0u;
  std::uint32_t layer = 0u;
  std::uint32_t sample_interval = 60u;
  bool prefer_clone = true;
};

struct ReadbackState {
  reshade::api::device* device = nullptr;
  reshade::api::command_queue* queue = nullptr;
  std::uint64_t draw_count = 0u;
  std::uint64_t sample_count = 0u;
  bool has_peak = false;
  float peak_max_rgb = 0.f;
  bool warned_no_target = false;
  bool warned_no_clone = false;
  bool warned_unsupported_format = false;
  bool warned_create_readback = false;
  bool warned_map_readback = false;
  std::uint32_t view_activation_attempts = 0u;
  bool logged_view_activation = false;
};

inline constexpr ReadbackConfig config = {};
inline ReadbackState state = {};

[[nodiscard]] inline float DecodeFloat16(std::uint16_t value) {
  const auto sign = static_cast<int>((value >> 15) & 0x1u);
  const auto exponent = static_cast<int>((value >> 10) & 0x1Fu);
  const auto mantissa = static_cast<std::uint32_t>(value & 0x03FFu);

  float decoded = 0.f;
  if (exponent == 0) {
    if (mantissa != 0u) {
      decoded = std::ldexp(static_cast<float>(mantissa), -24);
    }
  } else if (exponent == 0x1F) {
    decoded = mantissa == 0u
                  ? std::numeric_limits<float>::infinity()
                  : std::numeric_limits<float>::quiet_NaN();
  } else {
    decoded = std::ldexp(1.f + (static_cast<float>(mantissa) / 1024.f), exponent - 15);
  }

  return sign != 0 ? -decoded : decoded;
}

inline void LogWarningOnce(bool& warned, const std::string& message) {
  if (warned) return;
  warned = true;
  reshade::log::message(reshade::log::level::warning, message.c_str());
}

inline void ResetForDevice(reshade::api::device* device) {
  if (state.device == nullptr) {
    state.device = device;
    return;
  }
  if (state.device == device) return;
  state = {};
  state.device = device;
}

inline void OnInitCommandQueue(reshade::api::command_queue* queue) {
  if (queue == nullptr || (queue->get_type() & reshade::api::command_queue_type::graphics)
                              != reshade::api::command_queue_type::graphics) {
    return;
  }
  state.queue = queue;
}

inline void OnDestroyCommandQueue(reshade::api::command_queue* queue) {
  if (state.queue == queue) state.queue = nullptr;
}

inline void OnDrawn(reshade::api::command_list* cmd_list) {
  if (cmd_list == nullptr) return;

  auto* device = cmd_list->get_device();
  if (device == nullptr) return;
  ResetForDevice(device);

  ++state.draw_count;
  if (config.sample_interval == 0u || (state.draw_count % config.sample_interval) != 0u) return;
  ++state.sample_count;

  const auto& render_targets = renodx::utils::swapchain::GetRenderTargets(cmd_list);
  if (config.render_target_index >= render_targets.size()) {
    LogWarningOnce(
        state.warned_no_target,
        "LifeIsStrange Readback: configured render target index is not bound on the 06A2 draw.");
    return;
  }

  const auto requested_view = render_targets[config.render_target_index];
  if (requested_view.handle == 0u) {
    LogWarningOnce(state.warned_no_target, "LifeIsStrange Readback: configured render target view is null.");
    return;
  }

  reshade::api::resource source = {0u};
  reshade::api::resource_view source_view = requested_view;
  bool used_clone = false;
  bool found_view_info = false;
  std::string clone_diagnostic;
  auto inspect_view = [&](const renodx::utils::resource::ResourceViewInfo& info) {
    found_view_info = true;
    if (config.prefer_clone) {
      source = info.clone_resource;
      source_view = info.clone;
      used_clone = source.handle != 0u && source_view.handle != 0u;
      if (!used_clone) {
        const auto original_desc = info.original_resource.handle != 0u
                                       ? renodx::utils::resource::GetResourceDesc(device, info.original_resource)
                                       : reshade::api::resource_desc{};
        std::stringstream message;
        message << "LifeIsStrange Readback: 06A2 clone diagnostic"
                << " view=0x" << std::hex << requested_view.handle
                << " original=0x" << info.original_resource.handle
                << " clone_view=0x" << info.clone.handle
                << " clone_resource=0x" << info.clone_resource.handle
                << std::dec
                << " usage=" << info.usage
                << " view_format=" << info.desc.format
                << " resource_format=" << original_desc.texture.format
                << " size=" << original_desc.texture.width << "x" << original_desc.texture.height
                << " clone_target=" << (info.clone_target != nullptr ? info.clone_target->name.c_str() : "none");
        clone_diagnostic = message.str();
      }
    } else {
      source = info.original_resource;
    }
  };
  renodx::utils::resource::GetResourceViewInfo(requested_view, inspect_view);

  if (config.prefer_clone && !used_clone && state.view_activation_attempts < 3u) {
    ++state.view_activation_attempts;
    const auto activated_view = renodx::utils::resource::upgrade::GetResourceViewClone(
        requested_view,
        {
            .require_enabled = false,
            .allow_create = true,
            .activate = true,
        });
    if (activated_view.handle != 0u) {
      found_view_info = false;
      source = {0u};
      source_view = requested_view;
      used_clone = false;
      clone_diagnostic.clear();
      renodx::utils::resource::GetResourceViewInfo(requested_view, inspect_view);
      if (used_clone && !state.logged_view_activation) {
        state.logged_view_activation = true;
        reshade::log::message(
            reshade::log::level::info,
            "LifeIsStrange Readback: activated 06A2 FP16 render-target view clone.");
      }
    }
  }

  if (!found_view_info) {
    LogWarningOnce(state.warned_no_target, "LifeIsStrange Readback: render target view is not tracked.");
    return;
  }
  if (config.prefer_clone && !used_clone) {
    LogWarningOnce(
        state.warned_no_clone,
        clone_diagnostic.empty()
            ? "LifeIsStrange Readback: 06A2 render target has no active clone yet; waiting for the FP16 clone."
            : clone_diagnostic);
    return;
  }
  if (!config.prefer_clone) {
    source_view = requested_view;
  }
  if (source.handle == 0u) {
    source = renodx::utils::resource::GetResourceFromView(device, source_view);
  }
  if (source.handle == 0u) {
    LogWarningOnce(state.warned_no_target, "LifeIsStrange Readback: could not resolve the render target resource.");
    return;
  }

  const auto source_desc = renodx::utils::resource::GetResourceDesc(device, source);
  if (source_desc.type != reshade::api::resource_type::texture_2d
      && source_desc.type != reshade::api::resource_type::surface) {
    LogWarningOnce(state.warned_no_target, "LifeIsStrange Readback: resolved resource is not a 2D texture.");
    return;
  }
  if (source_desc.texture.format != reshade::api::format::r16g16b16a16_float) {
    std::stringstream message;
    message << "LifeIsStrange Readback: expected r16g16b16a16_float, got format enum "
            << static_cast<std::uint32_t>(source_desc.texture.format) << ".";
    LogWarningOnce(state.warned_unsupported_format, message.str());
    return;
  }
  const auto levels = std::max<std::uint32_t>(static_cast<std::uint32_t>(source_desc.texture.levels), 1u);
  const auto depth_or_layers =
      std::max<std::uint32_t>(static_cast<std::uint32_t>(source_desc.texture.depth_or_layers), 1u);
  if (config.mip_level >= levels || config.layer >= depth_or_layers) {
    LogWarningOnce(state.warned_no_target, "LifeIsStrange Readback: configured mip or layer is outside the resource.");
    return;
  }

  const auto width = std::max(source_desc.texture.width >> config.mip_level, 1u);
  const auto height = std::max(source_desc.texture.height >> config.mip_level, 1u);
  const auto subresource = config.mip_level + (config.layer * levels);
  const auto readback_desc = reshade::api::resource_desc(
      width,
      height,
      1u,
      1u,
      source_desc.texture.format,
      1u,
      reshade::api::memory_heap::gpu_to_cpu,
      reshade::api::resource_usage::copy_dest);

  reshade::api::resource readback = {0u};
  if (!device->create_resource(
          readback_desc,
          nullptr,
          reshade::api::resource_usage::copy_dest,
          &readback)) {
    LogWarningOnce(state.warned_create_readback, "LifeIsStrange Readback: failed to create GPU-to-CPU resource.");
    return;
  }

  auto* queue = state.queue;
  if (queue == nullptr) {
    device->destroy_resource(readback);
    LogWarningOnce(state.warned_create_readback, "LifeIsStrange Readback: graphics command queue is unavailable.");
    return;
  }

  auto* immediate_cmd_list = queue->get_immediate_command_list();
  if (immediate_cmd_list == nullptr) {
    device->destroy_resource(readback);
    LogWarningOnce(state.warned_create_readback, "LifeIsStrange Readback: graphics immediate command list is unavailable.");
    return;
  }
  immediate_cmd_list->copy_texture_region(source, subresource, nullptr, readback, 0u, nullptr);
  queue->flush_immediate_command_list();
  queue->wait_idle();

  reshade::api::subresource_data mapped = {};
  if (!device->map_texture_region(readback, 0u, nullptr, reshade::api::map_access::read_only, &mapped)) {
    device->destroy_resource(readback);
    LogWarningOnce(state.warned_map_readback, "LifeIsStrange Readback: failed to map GPU-to-CPU resource.");
    return;
  }

  float current_max_rgb = -std::numeric_limits<float>::infinity();
  float current_min_rgb = std::numeric_limits<float>::infinity();
  std::uint64_t above_one = 0u;
  std::uint64_t above_two = 0u;
  std::uint64_t above_four = 0u;
  const auto pixel_stride = static_cast<std::size_t>(reshade::api::format_row_pitch(source_desc.texture.format, 1u));
  for (std::uint32_t y = 0u; y < height; ++y) {
    const auto* row = static_cast<const std::uint8_t*>(mapped.data) + (static_cast<std::size_t>(mapped.row_pitch) * y);
    for (std::uint32_t x = 0u; x < width; ++x) {
      const auto* pixel = reinterpret_cast<const std::uint16_t*>(row + (pixel_stride * x));
      const auto r = DecodeFloat16(pixel[0]);
      const auto g = DecodeFloat16(pixel[1]);
      const auto b = DecodeFloat16(pixel[2]);
      if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) continue;
      const auto max_rgb = std::max(r, std::max(g, b));
      const auto min_rgb = std::min(r, std::min(g, b));
      current_max_rgb = std::max(current_max_rgb, max_rgb);
      current_min_rgb = std::min(current_min_rgb, min_rgb);
      above_one += max_rgb > 1.f ? 1u : 0u;
      above_two += max_rgb > 2.f ? 1u : 0u;
      above_four += max_rgb > 4.f ? 1u : 0u;
    }
  }

  device->unmap_texture_region(readback, 0u);
  device->destroy_resource(readback);

  if (!std::isfinite(current_max_rgb)) return;
  if (state.has_peak && current_max_rgb <= state.peak_max_rgb) return;

  state.has_peak = true;
  state.peak_max_rgb = current_max_rgb;
  std::stringstream message;
  message << std::fixed << std::setprecision(6)
          << "LifeIsStrange Readback peak: shader=0x" << std::hex << std::uppercase << config.shader_hash
          << " rtv_index=" << std::dec << config.render_target_index
          << " source=" << (used_clone ? "clone" : "original")
          << " format=r16g16b16a16_float"
          << " size=" << width << "x" << height
          << " sample=" << state.sample_count
          << " current_max_rgb=" << current_max_rgb
          << " peak_max_rgb=" << state.peak_max_rgb
          << " min_rgb=" << current_min_rgb
          << " above_1=" << above_one
          << " above_2=" << above_two
          << " above_4=" << above_four;
  reshade::log::message(reshade::log::level::info, message.str().c_str());
}

}  // namespace lifeisstrange::readback
