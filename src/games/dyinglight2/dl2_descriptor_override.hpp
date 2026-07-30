#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <include/reshade.hpp>

#include "../../utils/command_action.hpp"
#include "../../utils/data.hpp"
#include "../../utils/descriptor.hpp"
#include "../../utils/log.hpp"
#include "../../utils/pipeline_layout.hpp"
#include "../../utils/resource.hpp"
#include "../../utils/state.hpp"

namespace renodx::games::dyinglight2::descriptor_override {

struct TableKey {
  reshade::api::device* device = nullptr;
  uint64_t layout = 0u;
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
inline thread_local RestoreBinding pending_restore = {};
inline std::atomic_uint32_t success_log_count = 0u;
inline std::atomic_uint32_t skip_log_count = 0u;

inline void LogSkip(const char* reason) {
  if (skip_log_count.fetch_add(1u, std::memory_order_relaxed) >= 12u) return;
  renodx::utils::log::w("DL2 targeted t0 clone bind skipped: ", reason);
}

inline reshade::api::descriptor_table GetOrCreateCloneTable(
    reshade::api::device* device,
    reshade::api::pipeline_layout layout,
    uint32_t layout_param,
    uint32_t binding,
    reshade::api::resource_view clone_view) {
  const TableKey key = {
      .device = device,
      .layout = layout.handle,
      .layout_param = layout_param,
      .binding = binding,
      .clone_view = clone_view.handle,
  };

  std::scoped_lock lock(table_mutex);
  if (const auto existing = clone_tables.find(key); existing != clone_tables.end()) {
    return existing->second;
  }

  reshade::api::descriptor_table table = {0u};
  if (!device->allocate_descriptor_table(layout, layout_param, &table) || table.handle == 0u) {
    LogSkip("replacement table allocation failed");
    return {0u};
  }

  const reshade::api::descriptor_table_update update = {
      .table = table,
      .binding = binding,
      .array_offset = 0u,
      .count = 1u,
      .type = reshade::api::descriptor_type::texture_shader_resource_view,
      .descriptors = &clone_view,
  };
  device->update_descriptor_tables(1u, &update);
  clone_tables.emplace(key, table);
  return table;
}

inline bool BindCurrentT0Clone(reshade::api::command_list* cmd_list) {
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
  bool supported_layout = false;
  renodx::utils::pipeline_layout::GetPipelineLayoutData(
      state->graphics_pipeline_layout,
      [&](const renodx::utils::pipeline_layout::PipelineLayoutData* layout_data) {
        for (uint32_t index = 0u;
             index < layout_data->params.size() && index < state->graphics_descriptor_tables.size(); ++index) {
          const auto& param = layout_data->params[index];
          if (param.type != reshade::api::pipeline_layout_param_type::descriptor_table
              || param.descriptor_table.count != 1u) {
            continue;
          }
          const auto& range = param.descriptor_table.ranges[0];
          if (range.type != reshade::api::descriptor_type::texture_shader_resource_view
              || range.dx_register_index != 0u
              || range.dx_register_space != 0u
              || range.count != 1u) {
            continue;
          }
          layout_param = index;
          binding = range.binding;
          supported_layout = true;
          break;
        }
      });
  if (!supported_layout || layout_param >= state->graphics_descriptor_tables.size()) {
    LogSkip("t0 is not an isolated one-SRV descriptor table");
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
  renodx::utils::resource::GetLiveResourceViewInfo(
      original_view,
      [&](const renodx::utils::resource::ResourceViewInfo& info) {
        if (info.clone_enabled && info.clone.handle != 0u) clone_view = info.clone;
      });
  if (clone_view.handle == 0u || clone_view.handle == original_view.handle) {
    LogSkip("active FP16 t0 clone unavailable");
    return false;
  }

  const auto replacement_table = GetOrCreateCloneTable(
      device, state->graphics_pipeline_layout, layout_param, binding, clone_view);
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
        "DL2 targeted t0 clone bind: layout=", renodx::utils::log::AsPtr(state->graphics_pipeline_layout.handle),
        " param=", layout_param,
        " binding=", binding,
        " table=", renodx::utils::log::AsPtr(original_table.handle),
        "=>", renodx::utils::log::AsPtr(replacement_table.handle),
        " view=", renodx::utils::log::AsPtr(original_view.handle),
        "=>", renodx::utils::log::AsPtr(clone_view.handle));
  }
  return true;
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
  if (!BindCurrentT0Clone(context.cmd_list)) return {};
  return {
      .post_callback = RestoreOriginalT0<Context>,
      .post_data = nullptr,
  };
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
}

}  // namespace renodx::games::dyinglight2::descriptor_override
