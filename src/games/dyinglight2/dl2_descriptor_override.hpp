#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <include/reshade.hpp>

#include "../../mods/swapchain.hpp"
#include "../../utils/command_action.hpp"
#include "../../utils/data.hpp"
#include "../../utils/descriptor.hpp"
#include "../../utils/log.hpp"
#include "../../utils/pipeline_layout.hpp"
#include "../../utils/resource.hpp"
#include "../../utils/resource_upgrade.hpp"
#include "../../utils/state.hpp"

namespace renodx::games::dyinglight2::descriptor_override {

// Crash-isolation A/B: keep the clone implementation available for later
// testing, but do not install either target draw hook by default.
inline constexpr bool kEnableTargetOverrides = false;

struct TableKey {
  reshade::api::device* device = nullptr;
  uint64_t layout = 0u;
  uint64_t command_list = 0u;
  uint32_t layout_param = 0u;
  uint32_t binding = 0u;
  uint64_t clone_view = 0u;

  bool operator==(const TableKey&) const = default;
};

struct TableKeyHash {
  size_t operator()(const TableKey& key) const noexcept {
    size_t value = std::hash<void*>{}(key.device);
    const auto combine = [&value](uint64_t item) {
      value ^= std::hash<uint64_t>{}(item) + 0x9E3779B97F4A7C15ull + (value << 6u) + (value >> 2u);
    };
    combine(key.layout);
    combine(key.command_list);
    combine(key.layout_param);
    combine(key.binding);
    combine(key.clone_view);
    return value;
  }
};

struct RestoreBinding {
  reshade::api::pipeline_layout layout = {0u};
  reshade::api::descriptor_table table = {0u};
  uint32_t layout_param = 0u;
  bool active = false;
};

inline std::mutex table_mutex;
inline std::unordered_map<TableKey, reshade::api::descriptor_table, TableKeyHash> clone_tables;
inline std::unordered_map<reshade::api::device*, std::vector<reshade::api::descriptor_table>> retired_tables;
inline thread_local RestoreBinding pending_restore = {};
inline std::atomic_uint32_t success_log_count = 0u;
inline std::atomic_uint32_t skip_log_count = 0u;
inline std::mutex output_audit_mutex;
inline std::unordered_set<uint64_t> output_audit_keys;

inline void LogSkip(const char* reason) {
  if (skip_log_count.fetch_add(1u, std::memory_order_relaxed) >= 12u) return;
  renodx::utils::log::w("DL2 targeted t0 clone bind skipped: ", reason);
}

inline reshade::api::descriptor_table GetOrCreateCloneTable(
    reshade::api::command_list* cmd_list,
    reshade::api::device* device,
    reshade::api::pipeline_layout layout,
    uint32_t layout_param,
    uint32_t binding,
    uint32_t descriptor_count,
    reshade::api::descriptor_table original_table,
    reshade::api::resource_view clone_view) {
  const TableKey key = {
      .device = device,
      .layout = layout.handle,
      .command_list = reinterpret_cast<uintptr_t>(cmd_list),
      .layout_param = layout_param,
      .binding = binding,
      .clone_view = clone_view.handle,
  };

  std::scoped_lock lock(table_mutex);
  reshade::api::descriptor_table table = {0u};
  if (const auto existing = clone_tables.find(key); existing != clone_tables.end()) {
    table = existing->second;
  } else {
    if (!device->allocate_descriptor_table(layout, layout_param, &table) || table.handle == 0u) {
      LogSkip("replacement table allocation failed");
      return {0u};
    }
    clone_tables.emplace(key, table);
  }

  const reshade::api::descriptor_table_copy copy = {
      .source_table = original_table,
      .source_binding = 0u,
      .source_array_offset = 0u,
      .dest_table = table,
      .dest_binding = 0u,
      .dest_array_offset = 0u,
      .count = descriptor_count,
  };
  device->copy_descriptor_tables(1u, &copy);

  const reshade::api::descriptor_table_update update = {
      .table = table,
      .binding = binding,
      .array_offset = 0u,
      .count = 1u,
      .type = reshade::api::descriptor_type::texture_shader_resource_view,
      .descriptors = &clone_view,
  };
  device->update_descriptor_tables(1u, &update);
  return table;
}

inline bool BindCurrentT0Clone(reshade::api::command_list* cmd_list, uint32_t shader_hash) {
  if (cmd_list == nullptr) return false;
  auto* device = cmd_list->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) return false;

  const auto* state = renodx::utils::state::GetCurrentState(cmd_list);
  if (state == nullptr || state->graphics_pipeline_layout.handle == 0u) {
    LogSkip("graphics state unavailable");
    return false;
  }

  uint32_t layout_param = UINT32_MAX;
  uint32_t binding = UINT32_MAX;
  uint32_t descriptor_count = 0u;
  bool supported_layout = false;
  bool unbounded_layout = false;
  renodx::utils::pipeline_layout::GetPipelineLayoutData(
      state->graphics_pipeline_layout,
      [&](const renodx::utils::pipeline_layout::PipelineLayoutData* layout_data) {
        for (uint32_t index = 0u;
             index < layout_data->params.size() && index < state->graphics_descriptor_tables.size(); ++index) {
          const auto& param = layout_data->params[index];
          if (param.type != reshade::api::pipeline_layout_param_type::descriptor_table) {
            continue;
          }

          bool contains_t0 = false;
          uint32_t table_descriptor_count = 0u;
          bool table_unbounded = false;
          for (uint32_t range_index = 0u; range_index < param.descriptor_table.count; ++range_index) {
            const auto& range = param.descriptor_table.ranges[range_index];
            if (range.count == UINT32_MAX) {
              table_unbounded = true;
            } else {
              table_descriptor_count = std::max(table_descriptor_count, range.binding + range.count);
            }
            if (range.type == reshade::api::descriptor_type::texture_shader_resource_view
                && range.dx_register_space == 0u
                && range.dx_register_index == 0u
                && range.count != 0u) {
              contains_t0 = true;
              binding = range.binding;
            }
          }
          if (!contains_t0) continue;
          layout_param = index;
          descriptor_count = table_descriptor_count;
          unbounded_layout = table_unbounded;
          supported_layout = !table_unbounded && table_descriptor_count != 0u;
          break;
        }
      });
  if (!supported_layout || layout_param >= state->graphics_descriptor_tables.size()) {
    LogSkip(unbounded_layout
                ? "t0 table contains an unbounded descriptor range"
                : "finite t0 descriptor table layout unavailable");
    return false;
  }

  const auto original_table = state->graphics_descriptor_tables[layout_param];
  if (original_table.handle == 0u) {
    LogSkip("original t0 table is unbound");
    return false;
  }

  reshade::api::descriptor_heap heap = {0u};
  uint32_t heap_offset = 0u;
  device->get_descriptor_heap_offset(original_table, binding, 0u, &heap, &heap_offset);
  auto* descriptor_data = renodx::utils::data::Get<renodx::utils::descriptor::DeviceData>(device);
  if (descriptor_data == nullptr || heap.handle == 0u) {
    LogSkip("descriptor heap tracking unavailable");
    return false;
  }

  reshade::api::resource_view original_view = {0u};
  {
    const std::shared_lock lock(descriptor_data->mutex);
    const auto heap_it = descriptor_data->heaps.find(heap.handle);
    if (heap_it == descriptor_data->heaps.end() || heap_offset >= heap_it->second.size()) {
      LogSkip("tracked t0 heap slot unavailable");
      return false;
    }
    const auto& slot = heap_it->second[heap_offset];
    if (slot.type != reshade::api::descriptor_type::texture_shader_resource_view) {
      LogSkip("tracked t0 descriptor type mismatch");
      return false;
    }
    original_view = slot.resource_view;
  }
  if (original_view.handle == 0u) {
    LogSkip("t0 resource view is null");
    return false;
  }

  reshade::api::resource_view clone_view = {0u};
  reshade::api::resource original_resource = {0u};
  renodx::utils::resource::GetLiveResourceViewInfo(
      original_view,
      [&](const renodx::utils::resource::ResourceViewInfo& info) {
        original_resource = info.original_resource;
        if (info.clone.handle != 0u) clone_view = info.clone;
      });
  bool resource_clone_enabled = false;
  if (original_resource.handle != 0u) {
    renodx::utils::resource::GetLiveResourceInfo(
        original_resource,
        [&](const renodx::utils::resource::ResourceInfo& info) {
          resource_clone_enabled = info.clone_enabled && info.clone.handle != 0u;
        });
  }
  if (resource_clone_enabled && clone_view.handle == 0u) {
    clone_view = renodx::utils::resource::upgrade::GetResourceViewClone(
        original_view,
        {
            .require_enabled = false,
            .allow_create = true,
            .activate = false,
        });
  }
  if (!resource_clone_enabled || clone_view.handle == 0u || clone_view.handle == original_view.handle) {
    LogSkip("active FP16 t0 clone unavailable");
    return false;
  }

  const auto replacement_table = GetOrCreateCloneTable(
      cmd_list,
      device,
      state->graphics_pipeline_layout,
      layout_param,
      binding,
      descriptor_count,
      original_table,
      clone_view);
  if (replacement_table.handle == 0u) return false;

  pending_restore = {
      .layout = state->graphics_pipeline_layout,
      .table = original_table,
      .layout_param = layout_param,
      .active = true,
  };
  cmd_list->bind_descriptor_table(
      reshade::api::shader_stage::pixel,
      state->graphics_pipeline_layout,
      layout_param,
      replacement_table);

  if (success_log_count.fetch_add(1u, std::memory_order_relaxed) < 12u) {
    renodx::utils::log::i(
        "DL2 targeted t0 clone bind: shader=", renodx::utils::log::AsHex(shader_hash),
        " layout=", renodx::utils::log::AsPtr(state->graphics_pipeline_layout.handle),
        " param=", layout_param,
        " binding=", binding,
        " table=", renodx::utils::log::AsPtr(original_table.handle),
        "=>", renodx::utils::log::AsPtr(replacement_table.handle),
        " view=", renodx::utils::log::AsPtr(original_view.handle),
        "=>", renodx::utils::log::AsPtr(clone_view.handle));
  }
  return true;
}

inline void RewriteActiveRenderTargets(reshade::api::command_list* cmd_list, uint32_t shader_hash) {
  const auto rtvs = renodx::utils::swapchain::GetRenderTargets(cmd_list);
  if (rtvs.empty()) return;

  bool has_active_clone = false;
  reshade::api::resource_view first_clone = {0u};
  reshade::api::resource first_resource = {0u};
  reshade::api::format first_resource_format = reshade::api::format::unknown;
  reshade::api::format first_view_format = reshade::api::format::unknown;
  bool first_resource_clone_enabled = false;
  bool first_view_clone_enabled = false;
  for (const auto rtv : rtvs) {
    if (rtv.handle == 0u) continue;
    renodx::mods::swapchain::ActivateCloneHotSwap(cmd_list->get_device(), rtv);
    if (first_resource.handle == 0u) {
      first_resource = cmd_list->get_device()->get_resource_from_view(rtv);
      first_resource_format = cmd_list->get_device()->get_resource_desc(first_resource).texture.format;
      first_view_format = cmd_list->get_device()->get_resource_view_desc(rtv).format;
      renodx::utils::resource::GetLiveResourceInfo(
          first_resource,
          [&](const renodx::utils::resource::ResourceInfo& info) {
            first_resource_clone_enabled = info.clone_enabled;
          });
    }
    renodx::utils::resource::GetLiveResourceViewInfo(
        rtv,
        [&](const renodx::utils::resource::ResourceViewInfo& info) {
          has_active_clone = has_active_clone || (info.clone_enabled && info.clone.handle != 0u);
          if (first_clone.handle == 0u && info.clone.handle != 0u) first_clone = info.clone;
          if (rtv == rtvs.front()) first_view_clone_enabled = info.clone_enabled;
        });
  }
  if (has_active_clone) {
    renodx::mods::swapchain::RewriteRenderTargets(
        cmd_list, static_cast<uint32_t>(rtvs.size()), rtvs.data(), {0u});
  }

  const auto& rebound_rtvs = renodx::utils::swapchain::GetRenderTargets(cmd_list);
  const auto rebound = rebound_rtvs.empty() ? reshade::api::resource_view{0u} : rebound_rtvs.front();
  uint64_t audit_key = shader_hash;
  const auto combine = [&audit_key](uint64_t value) {
    audit_key ^= value + 0x9E3779B97F4A7C15ull + (audit_key << 6u) + (audit_key >> 2u);
  };
  combine(rtvs.front().handle);
  combine(first_clone.handle);
  combine(rebound.handle);
  combine(first_resource_clone_enabled ? 1u : 0u);
  bool should_log = false;
  {
    std::scoped_lock lock(output_audit_mutex);
    should_log = output_audit_keys.size() < 64u && output_audit_keys.insert(audit_key).second;
  }
  if (should_log) {
    renodx::utils::log::i(
        "DL2 target output audit: shader=", renodx::utils::log::AsHex(shader_hash),
        " rtv=", renodx::utils::log::AsPtr(rtvs.front().handle),
        " clone=", renodx::utils::log::AsPtr(first_clone.handle),
        " rebound=", renodx::utils::log::AsPtr(rebound.handle),
        " resource=", renodx::utils::log::AsPtr(first_resource.handle),
        " format=", static_cast<uint32_t>(first_resource_format),
        " view_format=", static_cast<uint32_t>(first_view_format),
        " resource_active=", first_resource_clone_enabled ? 1 : 0,
        " view_active=", first_view_clone_enabled ? 1 : 0,
        " rewrite=", has_active_clone ? 1 : 0);
  }
}

template <typename Context>
inline void RestoreOriginalT0(Context& context, const void*) {
  if (!pending_restore.active) return;
  const auto restore = pending_restore;
  pending_restore = {};
  context.cmd_list->bind_descriptor_table(
      reshade::api::shader_stage::pixel,
      restore.layout,
      restore.layout_param,
      restore.table);
}

inline constexpr auto OnTargetDraw = []<typename Context>(
                                         Context& context)
    -> renodx::utils::command_action::CallbackResult<Context> {
  const uint32_t shader_hash = context.matched_shader_hash;
  RewriteActiveRenderTargets(context.cmd_list, shader_hash);
  if (!BindCurrentT0Clone(context.cmd_list, shader_hash)) return {};
  return {
      .post_callback = RestoreOriginalT0<Context>,
      .post_data = nullptr,
  };
};

inline constexpr auto OnTargetOutputDraw = []<typename Context>(
                                               Context& context)
    -> renodx::utils::command_action::CallbackResult<Context> {
  RewriteActiveRenderTargets(context.cmd_list, context.matched_shader_hash);
  return {};
};

inline void OnDestroyDevice(reshade::api::device* device) {
  if (device == nullptr) return;
  std::scoped_lock lock(table_mutex);
  for (auto it = clone_tables.begin(); it != clone_tables.end();) {
    if (it->first.device != device) {
      ++it;
      continue;
    }
    if (it->second.handle != 0u) device->free_descriptor_table(it->second);
    it = clone_tables.erase(it);
  }
  if (const auto retired = retired_tables.find(device); retired != retired_tables.end()) {
    for (const auto table : retired->second) {
      if (table.handle != 0u) device->free_descriptor_table(table);
    }
    retired_tables.erase(retired);
  }
}

inline void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool resize) {
  if (swapchain == nullptr) return;
  auto* device = swapchain->get_device();
  if (device == nullptr) return;

  std::scoped_lock lock(table_mutex);
  size_t retired_count = 0u;
  for (auto it = clone_tables.begin(); it != clone_tables.end();) {
    if (it->first.device != device) {
      ++it;
      continue;
    }
    if (it->second.handle != 0u) {
      retired_tables[device].push_back(it->second);
      ++retired_count;
    }
    it = clone_tables.erase(it);
  }
  renodx::utils::log::i(
      "DL2 descriptor clone tables reset on swapchain ",
      resize ? "resize" : "destroy",
      ": retired=", retired_count);
}

}  // namespace renodx::games::dyinglight2::descriptor_override
