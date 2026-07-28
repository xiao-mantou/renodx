/*
 * Copyright (C) 2025 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#include <algorithm>
#include <array>
#include <atomic>
#include <d3d11.h>
#include <d3d12.h>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <embed/shaders.h>

#include <wrl/client.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>
#include <sl_core_api.h>
#include <sl_dlss_g.h>

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

// 0 = HDR10 (RGB10 + BT.2100 PQ), 1 = scRGB (FP16 + linear).
// DLSS Frame Generation requires HDR10/PQ, so that is the default. The value is
// read once in DllMain because a swapchain container cannot change at runtime.
float swap_chain_format_setting = 0.f;
// Immutable copy used by the proxy shader. ReShade may reload the global
// setting after DllMain, but the swapchain container cannot change then.
float swap_chain_use_hdr10 = 1.f;

float dlss_fg_tag_clone = 0.f;
float dlss_fg_suppress_color_tags = 0.f;
float dlss_fg_skip_generated_proxy = 0.f;
float dlss_fg_bypass_all_proxy = 0.f;
bool dlss_fg_tag_capture = false;
bool dlss_fg_present_cadence_capture = false;
std::atomic_uint32_t dlss_fg_backbuffer_barrier_capture = 0u;
std::atomic_uint32_t dlss_fg_color_tag_serial = 0u;
std::atomic_uint64_t dlss_fg_latest_color_original = 0u;
std::atomic_uint64_t dlss_fg_latest_color_clone = 0u;
std::atomic_bool dlss_fg_tag_transfer_capture_active = false;
uint32_t dlss_fg_last_present_tag_serial = 0u;
bool dlss_fg_hook_installed = false;
bool dlss_fg_waiting_for_streamline_logged = false;
bool dlss_fg_tag_clone_logged = false;
bool dlss_fg_color_tag_suppression_logged = false;

struct DlssFgHandoffAudit {
  bool armed = false;
  bool submitted = false;
  bool result_received = false;
  bool set_tag_for_frame = false;
  bool submitted_clone = false;
  uint32_t tag_serial = 0u;
  uint32_t color_tag_count = 0u;
  uint64_t original_resource = 0u;
  uint32_t original_format = 0u;
  uint64_t clone_resource = 0u;
  uint32_t clone_format = 0u;
  uint64_t submitted_resource = 0u;
  uint32_t submitted_format = 0u;
  int32_t result = 0;
  std::array<uint64_t, 2> back_buffers = {};
  uint32_t present_count = 0u;
};

std::mutex dlss_fg_handoff_audit_mutex;
DlssFgHandoffAudit dlss_fg_handoff_audit = {};

sl::Result (*real_sl_set_tag)(
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags,
    sl::CommandBuffer* cmd_buffer) = nullptr;

decltype(&slSetTagForFrame) real_sl_set_tag_for_frame = nullptr;
decltype(&slDLSSGSetOptions) real_sl_dlssg_set_options = nullptr;
decltype(&slDLSSGGetState) real_sl_dlssg_get_state = nullptr;
using SlDlssGHookPresent = HRESULT(IDXGISwapChain*, UINT, UINT, bool&);
using SlDlssGHookPresent1 = HRESULT(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, bool&);
SlDlssGHookPresent* real_sl_dlssg_hook_present = nullptr;
SlDlssGHookPresent1* real_sl_dlssg_hook_present1 = nullptr;
bool dlss_fg_options_logged = false;
bool dlss_fg_options_hook_installed = false;
bool dlss_fg_options_hook_wait_logged = false;
bool dlss_fg_state_hook_installed = false;
bool dlss_fg_state_hook_wait_logged = false;
bool dlss_fg_present_hook_installed = false;
bool dlss_fg_present_hook_wait_logged = false;
std::atomic_uint32_t dlss_fg_state_diagnostic_count = 0u;

struct DlssFgSwapchainSnapshot {
  uintptr_t wrapper = 0u;
  uintptr_t native = 0u;
  uintptr_t hwnd = 0u;
  uintptr_t back_buffer = 0u;
  uint32_t api = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t format = 0u;
  uint32_t buffer_count = 0u;
  uint32_t back_buffer_index = 0u;
  uint64_t serial = 0u;
};

std::mutex dlss_fg_swapchain_snapshot_mutex;
std::array<DlssFgSwapchainSnapshot, 8> dlss_fg_swapchain_snapshots = {};
uint64_t dlss_fg_swapchain_snapshot_serial = 0u;
std::atomic_uint32_t dlss_fg_present_identity_count = 0u;

std::mutex dlss_fg_fence_mutex;
ID3D12Fence* dlss_fg_inputs_fence = nullptr;
uint64_t dlss_fg_inputs_fence_value = 0u;
std::atomic_uint32_t dlss_fg_viewport = UINT_MAX;
std::vector<Microsoft::WRL::ComPtr<IUnknown>> dlss_fg_retained_resources;

enum class DlssFgWaitResult : uint8_t {
  no_fence,
  already_complete,
  wait_completed,
  timeout,
  set_event_failed,
};

void UpdateDlssFgFence(const sl::DLSSGState& state) {
  auto* next_fence = static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
  if (next_fence != nullptr) next_fence->AddRef();
  std::scoped_lock lock(dlss_fg_fence_mutex);
  if (dlss_fg_inputs_fence != nullptr) dlss_fg_inputs_fence->Release();
  dlss_fg_inputs_fence = next_fence;
  dlss_fg_inputs_fence_value = state.lastPresentInputsProcessingCompletionFenceValue;
}

DlssFgWaitResult WaitForDlssFgInputs(uint64_t *target_value, uint64_t *completed_before) {
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  uint64_t value = 0u;
  {
    std::scoped_lock lock(dlss_fg_fence_mutex);
    if (target_value != nullptr) *target_value = dlss_fg_inputs_fence_value;
    if (dlss_fg_inputs_fence == nullptr || dlss_fg_inputs_fence_value == 0u) {
      if (completed_before != nullptr) *completed_before = 0u;
      return DlssFgWaitResult::no_fence;
    }
    fence = dlss_fg_inputs_fence;
    value = dlss_fg_inputs_fence_value;
  }
  const uint64_t completed = fence->GetCompletedValue();
  if (completed_before != nullptr) *completed_before = completed;
  if (completed >= value) {
    std::scoped_lock lock(dlss_fg_fence_mutex);
    dlss_fg_retained_resources.clear();
    return DlssFgWaitResult::already_complete;
  }

  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event == nullptr) return DlssFgWaitResult::set_event_failed;
  const HRESULT set_event_result = fence->SetEventOnCompletion(value, event);
  const DWORD wait_result = set_event_result == S_OK ? WaitForSingleObject(event, 1000u) : WAIT_FAILED;
  CloseHandle(event);
  if (wait_result == WAIT_OBJECT_0) {
    std::scoped_lock lock(dlss_fg_fence_mutex);
    dlss_fg_retained_resources.clear();
    return DlssFgWaitResult::wait_completed;
  }
  return set_event_result == S_OK ? DlssFgWaitResult::timeout : DlssFgWaitResult::set_event_failed;
}

const char *DlssFgWaitResultName(DlssFgWaitResult result) {
  switch (result) {
    case DlssFgWaitResult::no_fence: return "no_fence";
    case DlssFgWaitResult::already_complete: return "already_complete";
    case DlssFgWaitResult::wait_completed: return "wait_completed";
    case DlssFgWaitResult::timeout: return "timeout";
    case DlssFgWaitResult::set_event_failed: return "set_event_failed";
  }
  return "unknown";
}

void RetainDlssFgResource(uint64_t handle) {
  if (handle == 0u) return;
  auto* resource = reinterpret_cast<IUnknown*>(handle);
  resource->AddRef();
  Microsoft::WRL::ComPtr<IUnknown> retained;
  retained.Attach(resource);
  std::scoped_lock lock(dlss_fg_fence_mutex);
  dlss_fg_retained_resources.push_back(std::move(retained));
}

sl::Result HookedSlDLSSGGetState(
    const sl::ViewportHandle& viewport,
    sl::DLSSGState& state,
    const sl::DLSSGOptions* options) {
  const auto result = real_sl_dlssg_get_state(viewport, state, options);
  if (result == sl::Result::eOk) {
    UpdateDlssFgFence(state);
    const uint32_t diagnostic_count = dlss_fg_state_diagnostic_count.fetch_add(1u);
    if (diagnostic_count < 64u) {
      auto *fence = static_cast<ID3D12Fence *>(state.inputsProcessingCompletionFence);
      const uint64_t completed = fence != nullptr ? fence->GetCompletedValue() : 0u;
      std::ostringstream message;
      message << "DL2 DLSS FG: GetState viewport=" << static_cast<uint32_t>(viewport)
              << " status=" << static_cast<uint32_t>(state.status)
              << " presented=" << state.numFramesActuallyPresented
              << " generate_max=" << state.numFramesToGenerateMax
              << " fence=" << static_cast<void *>(fence)
              << " target=" << state.lastPresentInputsProcessingCompletionFenceValue
              << " completed=" << completed;
      renodx::utils::log::i(message.str().c_str());
    }
  } else {
    renodx::utils::log::w("DL2 DLSS FG: GetState failed before resize/state update.");
  }
  return result;
}

DlssFgSwapchainSnapshot ReadNativeSwapchainSnapshot(IDXGISwapChain* swapchain) {
  DlssFgSwapchainSnapshot snapshot = {};
  snapshot.native = reinterpret_cast<uintptr_t>(swapchain);
  if (swapchain == nullptr) return snapshot;

  DXGI_SWAP_CHAIN_DESC desc = {};
  if (SUCCEEDED(swapchain->GetDesc(&desc))) {
    snapshot.hwnd = reinterpret_cast<uintptr_t>(desc.OutputWindow);
    snapshot.width = desc.BufferDesc.Width;
    snapshot.height = desc.BufferDesc.Height;
    snapshot.format = static_cast<uint32_t>(desc.BufferDesc.Format);
    snapshot.buffer_count = desc.BufferCount;
  }

  Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain3;
  if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3)))) {
    snapshot.back_buffer_index = swapchain3->GetCurrentBackBufferIndex();
  }
  Microsoft::WRL::ComPtr<IUnknown> back_buffer;
  if (SUCCEEDED(swapchain->GetBuffer(snapshot.back_buffer_index, IID_PPV_ARGS(&back_buffer)))) {
    snapshot.back_buffer = reinterpret_cast<uintptr_t>(back_buffer.Get());
  }
  return snapshot;
}

void CaptureReshadeSwapchainSnapshot(
    reshade::api::command_queue* queue,
    reshade::api::swapchain* swapchain) {
  if (queue == nullptr || swapchain == nullptr) return;

  DlssFgSwapchainSnapshot snapshot = ReadNativeSwapchainSnapshot(
      reinterpret_cast<IDXGISwapChain*>(swapchain->get_native()));
  snapshot.wrapper = reinterpret_cast<uintptr_t>(swapchain);
  snapshot.api = static_cast<uint32_t>(queue->get_device()->get_api());
  snapshot.hwnd = reinterpret_cast<uintptr_t>(swapchain->get_hwnd());
  snapshot.back_buffer = swapchain->get_current_back_buffer().handle;

  std::scoped_lock lock(dlss_fg_swapchain_snapshot_mutex);
  snapshot.serial = ++dlss_fg_swapchain_snapshot_serial;
  DlssFgSwapchainSnapshot* slot = nullptr;
  for (auto& existing : dlss_fg_swapchain_snapshots) {
    if (existing.native == snapshot.native && snapshot.native != 0u) {
      slot = &existing;
      break;
    }
    if (slot == nullptr && existing.native == 0u) slot = &existing;
  }
  if (slot == nullptr) {
    slot = &*std::min_element(
        dlss_fg_swapchain_snapshots.begin(),
        dlss_fg_swapchain_snapshots.end(),
        [](const auto& left, const auto& right) { return left.serial < right.serial; });
  }
  *slot = snapshot;
}

void LogDlssFgPresentIdentity(const char* call_name, IDXGISwapChain* swapchain) {
  const uint32_t count = dlss_fg_present_identity_count.fetch_add(1u, std::memory_order_relaxed);
  if (count >= 48u) return;

  const auto native = ReadNativeSwapchainSnapshot(swapchain);
  DlssFgSwapchainSnapshot matched = {};
  const char* match_kind = "none";
  {
    std::scoped_lock lock(dlss_fg_swapchain_snapshot_mutex);
    for (const auto& candidate : dlss_fg_swapchain_snapshots) {
      if (candidate.native != 0u && candidate.native == native.native) {
        matched = candidate;
        match_kind = "native";
        break;
      }
    }
    if (matched.native == 0u) {
      for (const auto& candidate : dlss_fg_swapchain_snapshots) {
        if (candidate.native != 0u && candidate.hwnd == native.hwnd
            && candidate.width == native.width && candidate.height == native.height
            && candidate.format == native.format) {
          matched = candidate;
          match_kind = candidate.back_buffer == native.back_buffer ? "hwnd_desc_buffer" : "hwnd_desc";
          break;
        }
      }
    }
  }

  std::ostringstream message;
  message << "DL2 DLSS FG swapchain identity: call=" << call_name
          << " thread=" << GetCurrentThreadId()
          << " hook_native=0x" << std::hex << std::uppercase << native.native
          << " hwnd=0x" << native.hwnd
          << " size=" << std::dec << native.width << "x" << native.height
          << " format=" << native.format
          << " buffers=" << native.buffer_count
          << " index=" << native.back_buffer_index
          << " backbuffer=0x" << std::hex << native.back_buffer
          << " match=" << match_kind;
  if (matched.native != 0u) {
    message << " reshade_wrapper=0x" << matched.wrapper
            << " reshade_native=0x" << matched.native
            << " api=" << std::dec << matched.api
            << " reshade_hwnd=0x" << std::hex << matched.hwnd
            << " reshade_backbuffer=0x" << matched.back_buffer
            << " reshade_index=" << std::dec << matched.back_buffer_index
            << " snapshot_serial=" << matched.serial;
  }
  renodx::utils::log::i(message.str().c_str());
}

HRESULT HookedSlDlssGPresent(IDXGISwapChain* swapchain, UINT sync_interval, UINT flags, bool& skip) {
  LogDlssFgPresentIdentity("Present", swapchain);
  return real_sl_dlssg_hook_present(swapchain, sync_interval, flags, skip);
}

HRESULT HookedSlDlssGPresent1(
    IDXGISwapChain* swapchain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters,
    bool& skip) {
  LogDlssFgPresentIdentity("Present1", swapchain);
  return real_sl_dlssg_hook_present1(swapchain, sync_interval, flags, parameters, skip);
}

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
      if (tags[index].resource != nullptr && tags[index].resource->native != nullptr) {
        const auto resource = reshade::api::resource{reinterpret_cast<uintptr_t>(tags[index].resource->native)};
        const uint64_t previous_resource = dlss_fg_latest_color_original.load(std::memory_order_relaxed);
        const uint64_t previous_clone = dlss_fg_latest_color_clone.load(std::memory_order_relaxed);
        if (previous_resource != resource.handle || previous_clone == 0u) {
          dlss_fg_latest_color_original.store(resource.handle, std::memory_order_relaxed);
          const auto* resource_info = renodx::utils::resource::GetResourceInfo(resource);
          dlss_fg_latest_color_clone.store(
              resource_info == nullptr ? 0u : resource_info->clone.handle,
              std::memory_order_relaxed);
        }
      }
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
  if (tags == nullptr || num_tags == 0u) return routed;

  if (dlss_fg_suppress_color_tags >= 0.5f) {
    routed.tags_storage.reserve(num_tags);
    for (uint32_t index = 0u; index < num_tags; ++index) {
      const auto type = tags[index].type;
      if (type == sl::kBufferTypeHUDLessColor || type == sl::kBufferTypeUIColorAndAlpha) continue;
      routed.tags_storage.push_back(tags[index]);
    }
    if (routed.tags_storage.size() != num_tags) {
      routed.count = static_cast<uint32_t>(routed.tags_storage.size());
      routed.tags = routed.tags_storage.data();
      if (!dlss_fg_color_tag_suppression_logged) {
        dlss_fg_color_tag_suppression_logged = true;
        renodx::utils::log::i("DL2 DLSS FG: suppressed pre-PQ HUDLessColor/UIColorAndAlpha tags.");
      }
    }
    if (routed.count == 0u) return routed;
  }

  if (dlss_fg_tag_clone < 0.5f) return routed;

  std::unordered_map<uint64_t, size_t> replacement_indices;
  for (uint32_t index = 0u; index < routed.count; ++index) {
    const auto& tag = routed.tags[index];
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
      if (routed.tags_storage.empty()) routed.tags_storage.assign(tags, tags + num_tags);
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

void BeginDlssFgHandoffAudit(
    bool set_tag_for_frame,
    const sl::ResourceTag* original_tags,
    const RoutedStreamlineTags& routed) {
  if (original_tags == nullptr || routed.tags == nullptr) return;

  std::scoped_lock lock(dlss_fg_handoff_audit_mutex);
  auto& audit = dlss_fg_handoff_audit;
  if (!audit.armed || audit.submitted) return;

  for (uint32_t index = 0u; index < routed.count; ++index) {
    const auto type = original_tags[index].type;
    if (type != sl::kBufferTypeHUDLessColor && type != sl::kBufferTypeUIColorAndAlpha) continue;

    ++audit.color_tag_count;
    if (audit.original_resource != 0u || original_tags[index].resource == nullptr
        || routed.tags[index].resource == nullptr) {
      continue;
    }

    const auto* original_resource = original_tags[index].resource;
    const auto* submitted_resource = routed.tags[index].resource;
    audit.original_resource = reinterpret_cast<uint64_t>(original_resource->native);
    audit.original_format = original_resource->nativeFormat;
    audit.submitted_resource = reinterpret_cast<uint64_t>(submitted_resource->native);
    audit.submitted_format = submitted_resource->nativeFormat;
    audit.submitted_clone = audit.original_resource != audit.submitted_resource;

    if (audit.original_resource == 0u) continue;
    const auto* resource_info = renodx::utils::resource::GetResourceInfo(
        reshade::api::resource{static_cast<uintptr_t>(audit.original_resource)});
    if (resource_info == nullptr) continue;

    audit.clone_resource = resource_info->clone.handle;
    audit.clone_format = static_cast<uint32_t>(resource_info->clone_desc.texture.format);
  }

  if (audit.color_tag_count == 0u || audit.original_resource == 0u) return;
  audit.armed = false;
  audit.submitted = true;
  audit.set_tag_for_frame = set_tag_for_frame;
  audit.tag_serial = dlss_fg_color_tag_serial.load(std::memory_order_relaxed);
}

void CompleteDlssFgHandoffAudit(sl::Result result) {
  std::scoped_lock lock(dlss_fg_handoff_audit_mutex);
  auto& audit = dlss_fg_handoff_audit;
  if (!audit.submitted || audit.result_received) return;
  audit.result = static_cast<int32_t>(result);
  audit.result_received = true;
}

void LogDlssFgHandoffAudit(const DlssFgHandoffAudit& audit) {
  std::stringstream stream;
  stream << "DL2 DLSS FG handoff audit ("
         << (audit.set_tag_for_frame ? "slSetTagForFrame" : "slSetTag")
         << "): color_tags=" << audit.color_tag_count
         << " route=" << (audit.submitted_clone ? "clone" : "original")
         << " result=" << audit.result
         << " original=0x" << std::hex << std::uppercase << audit.original_resource
         << "," << audit.original_format
         << " clone=0x" << audit.clone_resource
         << "," << audit.clone_format
         << " submitted=0x" << audit.submitted_resource
         << "," << audit.submitted_format
         << " presents=" << std::dec << audit.present_count;
  for (uint32_t index = 0u; index < audit.present_count; ++index) {
    stream << " #" << (index + 1u) << "=0x" << std::hex << std::uppercase << audit.back_buffers[index];
  }
  reshade::log::message(reshade::log::level::info, stream.str().c_str());
}

sl::Result HookedSlSetTag(
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags,
    sl::CommandBuffer* cmd_buffer) {
  MarkStreamlineColorTagSubmission(tags, num_tags);
  CaptureStreamlineTags("slSetTag", tags, num_tags);
  const auto routed = RouteStreamlineColorTags(tags, num_tags);
  BeginDlssFgHandoffAudit(false, tags, routed);
  const auto result = real_sl_set_tag(viewport, routed.tags, routed.count, cmd_buffer);
  CompleteDlssFgHandoffAudit(result);
  return result;
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
  BeginDlssFgHandoffAudit(true, tags, routed);
  const auto result = real_sl_set_tag_for_frame(frame, viewport, routed.tags, routed.count, cmd_buffer);
  CompleteDlssFgHandoffAudit(result);
  return result;
}

sl::Result HookedSlDLSSGSetOptions(
    const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options) {
  dlss_fg_viewport.store(static_cast<uint32_t>(viewport), std::memory_order_relaxed);
  auto corrected = options;
  const uint32_t incoming_color_format = corrected.colorBufferFormat;
  if (swap_chain_use_hdr10 >= 0.5f) {
    corrected.colorBufferFormat = static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM);
  }
  if (dlss_fg_tag_clone >= 0.5f) {
    corrected.hudLessBufferFormat = static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    corrected.uiBufferFormat = static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT);
  }

  const auto result = real_sl_dlssg_set_options(viewport, corrected);
  if (!dlss_fg_options_logged) {
    dlss_fg_options_logged = true;
    std::stringstream stream;
    stream << "DL2 DLSS FG options: color=" << incoming_color_format
           << "=>" << corrected.colorBufferFormat
           << " hudless=" << options.hudLessBufferFormat
           << "=>" << corrected.hudLessBufferFormat
           << " ui=" << options.uiBufferFormat
           << "=>" << corrected.uiBufferFormat
           << " buffers=" << corrected.numBackBuffers
           << " size=" << corrected.colorWidth << "x" << corrected.colorHeight
           << " result=" << static_cast<int32_t>(result);
    reshade::log::message(reshade::log::level::info, stream.str().c_str());
  }
  return result;
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

bool TryInstallDlssGOptionsHook(HMODULE module) {
  if (dlss_fg_options_hook_installed) return true;
  auto get_feature_function = reinterpret_cast<decltype(&slGetFeatureFunction)>(
      GetProcAddress(module, "slGetFeatureFunction"));
  if (get_feature_function == nullptr) return false;

  void* function = nullptr;
  if (get_feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", function) != sl::Result::eOk
      || function == nullptr) {
    return false;
  }

  real_sl_dlssg_set_options = reinterpret_cast<decltype(&slDLSSGSetOptions)>(function);
  if (DetourTransactionBegin() != NO_ERROR) return false;
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
      || DetourAttach(
             reinterpret_cast<void**>(&real_sl_dlssg_set_options),
             reinterpret_cast<void*>(&HookedSlDLSSGSetOptions)) != NO_ERROR
      || DetourTransactionCommit() != NO_ERROR) {
    DetourTransactionAbort();
    real_sl_dlssg_set_options = nullptr;
    return false;
  }

  dlss_fg_options_hook_installed = true;
  dlss_fg_options_hook_wait_logged = false;
  renodx::utils::log::i("DL2 DLSS FG: options format hook installed.");
  return true;
}

bool TryInstallDlssGStateHook(HMODULE module) {
  if (dlss_fg_state_hook_installed) return true;
  auto get_feature_function = reinterpret_cast<decltype(&slGetFeatureFunction)>(
      GetProcAddress(module, "slGetFeatureFunction"));
  if (get_feature_function == nullptr) return false;

  void* function = nullptr;
  if (get_feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState", function) != sl::Result::eOk
      || function == nullptr) {
    return false;
  }
  real_sl_dlssg_get_state = reinterpret_cast<decltype(&slDLSSGGetState)>(function);
  if (DetourTransactionBegin() != NO_ERROR) return false;
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
      || DetourAttach(
             reinterpret_cast<void**>(&real_sl_dlssg_get_state),
             reinterpret_cast<void*>(&HookedSlDLSSGGetState)) != NO_ERROR
      || DetourTransactionCommit() != NO_ERROR) {
    DetourTransactionAbort();
    real_sl_dlssg_get_state = nullptr;
    return false;
  }
  dlss_fg_state_hook_installed = true;
  dlss_fg_state_hook_wait_logged = false;
  renodx::utils::log::i("DL2 DLSS FG: state fence hook installed.");
  return true;
}

bool TryInstallDlssGPresentHook() {
  if (dlss_fg_present_hook_installed) return true;
  auto* module = GetModuleHandleA("sl.dlss_g.dll");
  if (module == nullptr) return false;
  using GetPluginFunction = void*(const char*);
  auto* get_plugin_function = reinterpret_cast<GetPluginFunction*>(
      GetProcAddress(module, "slGetPluginFunction"));
  if (get_plugin_function == nullptr) return false;

  real_sl_dlssg_hook_present = reinterpret_cast<SlDlssGHookPresent*>(
      get_plugin_function("slHookPresent"));
  real_sl_dlssg_hook_present1 = reinterpret_cast<SlDlssGHookPresent1*>(
      get_plugin_function("slHookPresent1"));
  if (real_sl_dlssg_hook_present == nullptr || real_sl_dlssg_hook_present1 == nullptr) return false;

  if (DetourTransactionBegin() != NO_ERROR) return false;
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
      || DetourAttach(
             reinterpret_cast<void**>(&real_sl_dlssg_hook_present),
             reinterpret_cast<void*>(&HookedSlDlssGPresent)) != NO_ERROR
      || DetourAttach(
             reinterpret_cast<void**>(&real_sl_dlssg_hook_present1),
             reinterpret_cast<void*>(&HookedSlDlssGPresent1)) != NO_ERROR
      || DetourTransactionCommit() != NO_ERROR) {
    DetourTransactionAbort();
    real_sl_dlssg_hook_present = nullptr;
    real_sl_dlssg_hook_present1 = nullptr;
    return false;
  }
  dlss_fg_present_hook_installed = true;
  dlss_fg_present_hook_wait_logged = false;
  renodx::utils::log::i("DL2 DLSS FG: read-only plugin Present identity hook installed.");
  return true;
}

void TryInstallStreamlineHook() {
  if (dlss_fg_hook_installed && dlss_fg_options_hook_installed && dlss_fg_state_hook_installed
      && dlss_fg_present_hook_installed) return;

  auto* module = GetModuleHandleA("sl.interposer.dll");
  if (module == nullptr) {
    if (!dlss_fg_waiting_for_streamline_logged) {
      dlss_fg_waiting_for_streamline_logged = true;
      renodx::utils::log::w("DL2 DLSS FG: sl.interposer.dll is not loaded yet; tag hook will retry at Present.");
    }
    return;
  }

  if (!dlss_fg_hook_installed) {
    if (renodx::utils::vtable::Hook(module, GetStreamlineHooks())) {
      dlss_fg_hook_installed = true;
      renodx::utils::log::i("DL2 DLSS FG: Streamline tag capture hook installed.");
    } else {
      renodx::utils::log::w("DL2 DLSS FG: Streamline tag hook was not installed.");
    }
  }
  if (!dlss_fg_options_hook_installed && !TryInstallDlssGOptionsHook(module)
      && !dlss_fg_options_hook_wait_logged) {
    dlss_fg_options_hook_wait_logged = true;
    renodx::utils::log::w("DL2 DLSS FG: options format hook was not installed.");
  }
  if (!dlss_fg_state_hook_installed && !TryInstallDlssGStateHook(module)
      && !dlss_fg_state_hook_wait_logged) {
    dlss_fg_state_hook_wait_logged = true;
    renodx::utils::log::w("DL2 DLSS FG: state fence hook was not installed.");
  }
  if (!dlss_fg_present_hook_installed && !TryInstallDlssGPresentHook()
      && !dlss_fg_present_hook_wait_logged) {
    dlss_fg_present_hook_wait_logged = true;
    renodx::utils::log::w("DL2 DLSS FG: read-only plugin Present identity hook was not installed.");
  }
}

void RemoveStreamlineHook() {
  auto* module = GetModuleHandleA("sl.interposer.dll");
  if (module != nullptr && dlss_fg_hook_installed) {
    renodx::utils::vtable::Unhook(module, GetStreamlineHooks());
  }
  dlss_fg_hook_installed = false;
  if (dlss_fg_options_hook_installed && real_sl_dlssg_set_options != nullptr) {
    if (DetourTransactionBegin() == NO_ERROR
        && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
        && DetourDetach(
               reinterpret_cast<void**>(&real_sl_dlssg_set_options),
               reinterpret_cast<void*>(&HookedSlDLSSGSetOptions)) == NO_ERROR) {
      DetourTransactionCommit();
    } else {
      DetourTransactionAbort();
    }
  }
  real_sl_dlssg_set_options = nullptr;
  dlss_fg_options_hook_installed = false;
  dlss_fg_options_hook_wait_logged = false;
  if (dlss_fg_state_hook_installed && real_sl_dlssg_get_state != nullptr) {
    if (DetourTransactionBegin() == NO_ERROR
        && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
        && DetourDetach(
               reinterpret_cast<void**>(&real_sl_dlssg_get_state),
               reinterpret_cast<void*>(&HookedSlDLSSGGetState)) == NO_ERROR) {
      DetourTransactionCommit();
    } else {
      DetourTransactionAbort();
    }
  }
  real_sl_dlssg_get_state = nullptr;
  dlss_fg_state_hook_installed = false;
  dlss_fg_state_hook_wait_logged = false;
  if (dlss_fg_present_hook_installed) {
    if (DetourTransactionBegin() == NO_ERROR
        && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
        && DetourDetach(
               reinterpret_cast<void**>(&real_sl_dlssg_hook_present),
               reinterpret_cast<void*>(&HookedSlDlssGPresent)) == NO_ERROR
        && DetourDetach(
               reinterpret_cast<void**>(&real_sl_dlssg_hook_present1),
               reinterpret_cast<void*>(&HookedSlDlssGPresent1)) == NO_ERROR) {
      DetourTransactionCommit();
    } else {
      DetourTransactionAbort();
    }
  }
  real_sl_dlssg_hook_present = nullptr;
  real_sl_dlssg_hook_present1 = nullptr;
  dlss_fg_present_hook_installed = false;
  dlss_fg_present_hook_wait_logged = false;
  std::scoped_lock lock(dlss_fg_fence_mutex);
  if (dlss_fg_inputs_fence != nullptr) dlss_fg_inputs_fence->Release();
  dlss_fg_inputs_fence = nullptr;
  dlss_fg_inputs_fence_value = 0u;
  dlss_fg_retained_resources.clear();
}

void OnDestroySwapchain(reshade::api::swapchain*, bool resize) {
  if (!resize) return;
  const uint32_t viewport_value = dlss_fg_viewport.load(std::memory_order_relaxed);
  bool state_updated = false;
  if (real_sl_dlssg_get_state != nullptr && viewport_value != UINT_MAX) {
    sl::DLSSGState state{};
    if (real_sl_dlssg_get_state(sl::ViewportHandle(viewport_value), state, nullptr) == sl::Result::eOk) {
      UpdateDlssFgFence(state);
      state_updated = true;
    }
  }
  uint64_t target = 0u;
  uint64_t completed = 0u;
  const auto wait_result = WaitForDlssFgInputs(&target, &completed);
  std::ostringstream wait_message;
  wait_message << "DL2 DLSS FG: resize wait result=" << DlssFgWaitResultName(wait_result)
               << " state_updated=" << (state_updated ? "yes" : "no")
               << " target=" << target << " completed_before=" << completed;
  renodx::utils::log::i(wait_message.str().c_str());
  if (wait_result == DlssFgWaitResult::timeout || wait_result == DlssFgWaitResult::set_event_failed) {
    // Keep native resources alive if DLSS-G stopped progressing during focus
    // loss. This avoids destroying an object still referenced by FG.
    RetainDlssFgResource(dlss_fg_latest_color_original.load(std::memory_order_relaxed));
    RetainDlssFgResource(dlss_fg_latest_color_clone.load(std::memory_order_relaxed));
    renodx::utils::log::w("DL2 DLSS FG: retained tagged resources after fence wait failure.");
  }
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

struct DlssFgProducerAuditState {
  std::array<uint32_t, 16> pixel_shader_hashes = {};
  uint64_t original_resource = 0u;
  uint64_t clone_resource = 0u;
  uint32_t count = 0u;
  bool active = false;
};

struct DlssFgComputeWriter {
  uint32_t shader_hash = 0u;
  uint64_t resource = 0u;
  uint64_t effective_resource = 0u;
  reshade::api::format format = reshade::api::format::unknown;
  reshade::api::format effective_format = reshade::api::format::unknown;
  uint32_t group_count_x = 0u;
  uint32_t group_count_y = 0u;
  uint32_t group_count_z = 0u;
};

struct DlssFgComputeWriterAuditState {
  std::array<DlssFgComputeWriter, 8> writers = {};
  uint64_t original_resource = 0u;
  uint64_t clone_resource = 0u;
  uint32_t initial_tag_serial = 0u;
  uint32_t final_tag_serial = 0u;
  uint32_t present_count = 0u;
  uint32_t compute_uav_update_count = 0u;
  uint32_t compute_uav_view_count = 0u;
  uint32_t count = 0u;
  bool active = false;
};

struct DlssFgTagTransferAuditState {
  std::array<DownstreamTransfer, 8> transfers = {};
  uint64_t original_resource = 0u;
  uint64_t clone_resource = 0u;
  uint32_t count = 0u;
  bool active = false;
};

DownstreamDrawCaptureState downstream_draw_capture_state = {};
GammaDrawAuditState gamma_draw_audit_state = {};
GammaNativeInputAuditState gamma_native_input_audit_state = {};
DlssFgProducerAuditState dlss_fg_producer_audit_state = {};
DlssFgComputeWriterAuditState dlss_fg_compute_writer_audit_state = {};
DlssFgTagTransferAuditState dlss_fg_tag_transfer_audit_state = {};
std::mutex downstream_draw_capture_mutex;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> downstream_capture_rtvs;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> downstream_capture_t0_views;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> gamma_audit_t0_views;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> dlss_fg_compute_uav_views;

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
  const bool capture_fg_compute_writer = dlss_fg_compute_writer_audit_state.active;
  if ((!capture_gamma_input && !capture_downstream_inputs && !capture_fg_compute_writer) || update.count == 0u) {
    return;
  }

  if (capture_fg_compute_writer
      && renodx::utils::bitwise::HasFlag(stages, reshade::api::shader_stage::compute)
      && (update.type == reshade::api::descriptor_type::texture_unordered_access_view
          || update.type == reshade::api::descriptor_type::buffer_unordered_access_view)) {
    std::scoped_lock lock(downstream_draw_capture_mutex);
    auto& audit = dlss_fg_compute_writer_audit_state;
    if (!audit.active) return;
    ++audit.compute_uav_update_count;

    auto* device = cmd_list->get_device();
    if (device != nullptr) {
      for (uint32_t index = 0u; index < update.count; ++index) {
        const auto view = renodx::utils::descriptor::GetResourceViewFromDescriptorUpdate(update, index);
        if (view.handle == 0u) continue;
        ++audit.compute_uav_view_count;
        const auto resource = device->get_resource_from_view(view);
        if (resource.handle == audit.original_resource || resource.handle == audit.clone_resource) {
          dlss_fg_compute_uav_views[cmd_list] = view;
          break;
        }
      }
    }
  }

  if ((!capture_gamma_input && !capture_downstream_inputs)
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
      || dlss_fg_producer_audit_state.active
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
  auto& fg_producer_audit = dlss_fg_producer_audit_state;
  const bool capture_fg_producer = fg_producer_audit.active;
  auto& fg_compute_writer_audit = dlss_fg_compute_writer_audit_state;
  const bool capture_fg_compute_writer = fg_compute_writer_audit.active;
  if (!capture_commands && !capture_transfers && !capture_fg_producer && !capture_fg_compute_writer) {
    capture = {};
    downstream_capture_t0_views.clear();
    return {};
  }
  if (capture.consumed && !capture_fg_producer && !capture_fg_compute_writer) return {};

  auto* shader_state = renodx::utils::command_action::GetShaderState(&context);
  if (shader_state == nullptr) return {};
  const bool is_compute = context.IsDispatch();
  const uint32_t shader_hash = renodx::utils::shader::GetCurrentShaderHash(
      shader_state, is_compute ? renodx::utils::shader::COMPUTE_INDEX
                               : renodx::utils::shader::PIXEL_INDEX);

  if (capture_fg_producer && !is_compute && shader_hash != 0u) {
    const auto target = downstream_capture_rtvs.find(context.cmd_list);
    if (target != downstream_capture_rtvs.end()) {
      const auto output = DescribeGammaAuditView(context.cmd_list->get_device(), target->second);
      if (output.resource == fg_producer_audit.original_resource
          || output.effective == fg_producer_audit.clone_resource) {
        bool known_shader = false;
        for (uint32_t index = 0u; index < fg_producer_audit.count; ++index) {
          if (fg_producer_audit.pixel_shader_hashes[index] == shader_hash) { known_shader = true; break; }
        }
        if (!known_shader && fg_producer_audit.count < fg_producer_audit.pixel_shader_hashes.size()) {
          fg_producer_audit.pixel_shader_hashes[fg_producer_audit.count++] = shader_hash;
        }
      }
    }
  }

  if (capture_fg_compute_writer && is_compute && shader_hash != 0u) {
    const auto target = dlss_fg_compute_uav_views.find(context.cmd_list);
    if (target != dlss_fg_compute_uav_views.end()) {
      const auto output = DescribeGammaAuditView(context.cmd_list->get_device(), target->second);
      if (output.resource == fg_compute_writer_audit.original_resource
          || output.resource == fg_compute_writer_audit.clone_resource
          || output.effective == fg_compute_writer_audit.clone_resource) {
        bool known_shader = false;
        for (uint32_t index = 0u; index < fg_compute_writer_audit.count; ++index) {
          const auto& writer = fg_compute_writer_audit.writers[index];
          if (writer.shader_hash == shader_hash && writer.resource == output.resource
              && writer.effective_resource == output.effective) {
            known_shader = true;
            break;
          }
        }
        if (!known_shader && fg_compute_writer_audit.count < fg_compute_writer_audit.writers.size()) {
          auto& writer = fg_compute_writer_audit.writers[fg_compute_writer_audit.count++];
          writer = {
              .shader_hash = shader_hash,
              .resource = output.resource,
              .effective_resource = output.effective,
              .format = output.format,
              .effective_format = output.effective_format,
          };
          if constexpr (requires { context.arguments.group_count_x; }) {
            writer.group_count_x = context.arguments.group_count_x;
            writer.group_count_y = context.arguments.group_count_y;
            writer.group_count_z = context.arguments.group_count_z;
          }
        }
      }
    }
  }

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

bool DescribeDownstreamTransfer(
    DownstreamTransferType type,
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest,
    DownstreamTransfer* output) {
  if (cmd_list == nullptr || output == nullptr) return false;
  auto* device = cmd_list->get_device();
  if (device == nullptr) return false;

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
  *output = transfer;
  return true;
}

void RecordDownstreamTransfer(
    DownstreamTransferType type,
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest) {
  std::scoped_lock lock(downstream_draw_capture_mutex);
  auto& capture = downstream_draw_capture_state;
  if (!capture.active || capture.consumed || !capture.capture_transfers) return;
  if (capture.transfer_count >= capture.transfers.size()) return;

  DownstreamTransfer transfer = {};
  if (!DescribeDownstreamTransfer(type, cmd_list, source, dest, &transfer)) return;
  for (uint32_t index = 0u; index < capture.transfer_count; ++index) {
    const auto& existing = capture.transfers[index];
    if (existing.type == transfer.type && existing.source == transfer.source && existing.dest == transfer.dest) return;
  }
  capture.transfers[capture.transfer_count++] = transfer;
}

void RecordDlssFgTagTransfer(
    DownstreamTransferType type,
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest) {
  if (!dlss_fg_tag_transfer_capture_active.load(std::memory_order_relaxed)) return;

  std::scoped_lock lock(downstream_draw_capture_mutex);
  auto& audit = dlss_fg_tag_transfer_audit_state;
  if (!audit.active || audit.count >= audit.transfers.size()) return;
  if (source.handle != audit.original_resource && source.handle != audit.clone_resource
      && dest.handle != audit.original_resource && dest.handle != audit.clone_resource) {
    return;
  }

  DownstreamTransfer transfer = {};
  if (!DescribeDownstreamTransfer(type, cmd_list, source, dest, &transfer)) return;
  for (uint32_t index = 0u; index < audit.count; ++index) {
    const auto& existing = audit.transfers[index];
    if (existing.type == transfer.type && existing.source == transfer.source && existing.dest == transfer.dest) return;
  }
  audit.transfers[audit.count++] = transfer;
}

bool OnDownstreamCopyResource(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest) {
  RecordDlssFgTagTransfer(DownstreamTransferType::copy_resource, cmd_list, source, dest);
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
  RecordDlssFgTagTransfer(DownstreamTransferType::copy_texture_region, cmd_list, source, dest);
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
  RecordDlssFgTagTransfer(DownstreamTransferType::resolve_texture_region, cmd_list, source, dest);
  RecordDownstreamTransfer(DownstreamTransferType::resolve_texture_region, cmd_list, source, dest);
  return false;
}

void OnDlssFgBackbufferBarrier(
    reshade::api::command_list*,
    uint32_t count,
    const reshade::api::resource* resources,
    const reshade::api::resource_usage* old_states,
    const reshade::api::resource_usage* new_states) {
  if (dlss_fg_backbuffer_barrier_capture.load(std::memory_order_relaxed) == 0u
      || resources == nullptr || old_states == nullptr || new_states == nullptr) {
    return;
  }

  for (uint32_t index = 0u; index < count; ++index) {
    const auto resource = resources[index];
    bool is_backbuffer = false;
    uint32_t width = 0u;
    uint32_t height = 0u;
    renodx::utils::resource::GetResourceInfo(resource, [&](const renodx::utils::resource::ResourceInfo& info) {
      is_backbuffer = info.is_swap_chain;
      width = info.desc.texture.width;
      height = info.desc.texture.height;
    });
    if (!is_backbuffer || width < 128u || height < 128u) continue;

    uint32_t remaining = dlss_fg_backbuffer_barrier_capture.load(std::memory_order_relaxed);
    while (remaining != 0u
           && !dlss_fg_backbuffer_barrier_capture.compare_exchange_weak(
               remaining, remaining - 1u, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    }
    if (remaining == 0u) return;

    std::stringstream stream;
    stream << "DL2 DLSS FG backbuffer barrier: tag="
           << dlss_fg_color_tag_serial.load(std::memory_order_relaxed)
           << " resource=0x" << std::hex << std::uppercase << resource.handle
           << " size=" << std::dec << width << "x" << height
           << " old=0x" << std::hex << static_cast<uint32_t>(old_states[index])
           << " new=0x" << static_cast<uint32_t>(new_states[index])
           << " remaining=" << std::dec << (remaining - 1u);
    reshade::log::message(reshade::log::level::info, stream.str().c_str());
  }
}

void OnDownstreamDrawCapturePresent(
    reshade::api::command_queue* queue,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect*,
    const reshade::api::rect*,
    uint32_t,
    const reshade::api::rect*) {
  CaptureReshadeSwapchainSnapshot(queue, swapchain);
  TryInstallStreamlineHook();
  // One-shot, first-Present only. Reports the final presentation state so an
  // HDR10 black screen can be told apart from a wrong-encoding black screen.
  // It reads descriptors and handles only: no readback, dump, or redirection.
  static std::atomic<bool> swapchain_state_logged{false};
  if (!swapchain_state_logged.exchange(true, std::memory_order_relaxed)) {
    auto* device = queue->get_device();
    const auto back_buffer = swapchain->get_current_back_buffer();
    const auto desc = device->get_resource_desc(back_buffer);
    uint64_t clone_handle = 0u;
    bool clone_enabled = false;
    bool has_clone_target = false;
    reshade::api::format clone_format = reshade::api::format::unknown;
    renodx::utils::resource::GetResourceInfo(back_buffer, [&](const renodx::utils::resource::ResourceInfo& info) {
      clone_handle = info.clone.handle;
      clone_enabled = info.clone_enabled;
      has_clone_target = (info.clone_target != nullptr);
      if (info.clone_target != nullptr) clone_format = info.clone_target->new_format;
    });
    std::stringstream s;
    s << "DL2 present state(";
    s << "back_buffer=0x" << std::hex << std::uppercase << back_buffer.handle << std::dec << std::nouppercase;
    s << ", format=" << desc.texture.format;
    s << ", size=" << desc.texture.width << "x" << desc.texture.height;
    s << ", clone=0x" << std::hex << std::uppercase << clone_handle << std::dec << std::nouppercase;
    s << ", clone_enabled=" << (clone_enabled ? "1" : "0");
    s << ", clone_target=" << (has_clone_target ? "1" : "0");
    s << ", clone_format=" << clone_format;
    s << ", swap_chain_use_hdr10=" << swap_chain_use_hdr10;
    s << ")";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }

  {
    std::scoped_lock lock(dlss_fg_handoff_audit_mutex);
    auto& audit = dlss_fg_handoff_audit;
    const uint32_t tag_serial = dlss_fg_color_tag_serial.load(std::memory_order_relaxed);
    if (audit.submitted && audit.result_received && tag_serial == audit.tag_serial
        && audit.present_count < audit.back_buffers.size()) {
      audit.back_buffers[audit.present_count++] = swapchain->get_current_back_buffer().handle;
      if (audit.present_count == audit.back_buffers.size()) {
        LogDlssFgHandoffAudit(audit);
        audit = {};
      }
    } else if (audit.submitted && audit.result_received && tag_serial != audit.tag_serial) {
      LogDlssFgHandoffAudit(audit);
      audit = {};
    }
  }

  if (dlss_fg_bypass_all_proxy >= 0.5f) {
    renodx::mods::swapchain::skip_next_proxy_draw.store(true, std::memory_order_release);
  } else if (dlss_fg_skip_generated_proxy >= 0.5f) {
    const uint32_t tag_serial = dlss_fg_color_tag_serial.load(std::memory_order_acquire);
    if (tag_serial != 0u && tag_serial != dlss_fg_last_present_tag_serial) {
      dlss_fg_last_present_tag_serial = tag_serial;
      renodx::mods::swapchain::skip_next_proxy_draw.store(true, std::memory_order_release);
    }
  } else {
    dlss_fg_last_present_tag_serial = dlss_fg_color_tag_serial.load(std::memory_order_relaxed);
    renodx::mods::swapchain::skip_next_proxy_draw.store(false, std::memory_order_release);
  }

  std::scoped_lock lock(downstream_draw_capture_mutex);
  if (dlss_fg_tag_transfer_audit_state.active) {
    const auto& audit = dlss_fg_tag_transfer_audit_state;
    std::stringstream stream;
    stream << "DL2 DLSS FG tag transfers: original=0x"
           << std::hex << std::uppercase << audit.original_resource
           << " clone=0x" << audit.clone_resource
           << " count=" << std::dec << audit.count;
    for (uint32_t index = 0u; index < audit.count; ++index) {
      const auto& transfer = audit.transfers[index];
      const char* type = transfer.type == DownstreamTransferType::copy_resource ? "CopyResource"
                         : transfer.type == DownstreamTransferType::copy_texture_region ? "CopyTexture"
                                                                                         : "ResolveTexture";
      stream << " " << type << "(0x" << std::hex << std::uppercase << transfer.source
             << "," << static_cast<uint32_t>(transfer.source_format)
             << ",clone=0x" << transfer.source_clone
             << " => 0x" << transfer.dest
             << "," << static_cast<uint32_t>(transfer.dest_format)
             << ",clone=0x" << transfer.dest_clone << ")";
    }
    reshade::log::message(reshade::log::level::info, stream.str().c_str());
    dlss_fg_tag_transfer_audit_state = {};
    dlss_fg_tag_transfer_capture_active.store(false, std::memory_order_relaxed);
  }

  if (dlss_fg_compute_writer_audit_state.active) {
    auto& audit = dlss_fg_compute_writer_audit_state;
    audit.final_tag_serial = dlss_fg_color_tag_serial.load(std::memory_order_relaxed);
    ++audit.present_count;
    if (audit.present_count >= 3u) {
      std::stringstream stream;
      stream << "DL2 DLSS FG tag compute writers: original=0x"
             << std::hex << std::uppercase << audit.original_resource
             << " clone=0x" << audit.clone_resource
             << " presents=" << std::dec << audit.present_count
             << " tags=" << audit.initial_tag_serial << "=>" << audit.final_tag_serial
             << " uav_updates=" << audit.compute_uav_update_count
             << " uav_views=" << audit.compute_uav_view_count
             << " count=" << audit.count;
      for (uint32_t index = 0u; index < audit.count; ++index) {
        const auto& writer = audit.writers[index];
        stream << " cs=0x" << std::hex << std::uppercase << writer.shader_hash
               << " uav(0x" << writer.resource << "," << static_cast<uint32_t>(writer.format)
               << "=>0x" << writer.effective_resource << "," << static_cast<uint32_t>(writer.effective_format)
               << ",groups=" << std::dec << writer.group_count_x << "x" << writer.group_count_y
               << "x" << writer.group_count_z << ")";
      }
      reshade::log::message(reshade::log::level::info, stream.str().c_str());
      dlss_fg_compute_writer_audit_state = {};
      dlss_fg_compute_uav_views.clear();
    }
  }

  if (dlss_fg_producer_audit_state.active) {
    const auto& audit = dlss_fg_producer_audit_state;
    std::stringstream stream;
    stream << "DL2 DLSS FG tag graphics writers: original=0x"
           << std::hex << std::uppercase << audit.original_resource
           << " clone=0x" << audit.clone_resource
           << " count=" << std::dec << audit.count;
    for (uint32_t index = 0u; index < audit.count; ++index) {
      stream << " ps=0x" << std::hex << std::uppercase << audit.pixel_shader_hashes[index];
    }
    reshade::log::message(reshade::log::level::info, stream.str().c_str());
    dlss_fg_producer_audit_state = {};
  }
  if (dlss_fg_present_cadence_capture) {
    struct PresentSample {
      uint32_t tag_serial = 0u;
      uint64_t back_buffer = 0u;
      uint32_t format = 0u;
      uint32_t usage = 0u;
      uint64_t clone = 0u;
      bool clone_enabled = false;
      bool clone_target = false;
      uint32_t view_count = 0u;
    };
    static std::array<PresentSample, 16> samples = {};
    static uint32_t present_count = 0u;

    const auto back_buffer = swapchain->get_current_back_buffer();
    const auto desc = queue->get_device()->get_resource_desc(back_buffer);
    auto& sample = samples[present_count];
    sample.tag_serial = dlss_fg_color_tag_serial.load(std::memory_order_relaxed);
    sample.back_buffer = back_buffer.handle;
    sample.format = static_cast<uint32_t>(desc.texture.format);
    sample.usage = static_cast<uint32_t>(desc.usage);
    renodx::utils::resource::GetResourceInfo(back_buffer, [&](const renodx::utils::resource::ResourceInfo& info) {
      sample.clone = info.clone.handle;
      sample.clone_enabled = info.clone_enabled;
      sample.clone_target = info.clone_target != nullptr;
      sample.view_count = static_cast<uint32_t>(info.resource_view_handles.size());
    });
    ++present_count;

    if (present_count >= samples.size()) {
      std::stringstream stream;
      stream << "DL2 DLSS FG present cadence (" << present_count << "):";
      for (uint32_t index = 0u; index < present_count; ++index) {
        const auto& item = samples[index];
        stream << " #" << std::dec << (index + 1u) << " tag=" << item.tag_serial
               << " backbuffer=0x" << std::hex << std::uppercase << item.back_buffer
               << " format=" << std::dec << item.format
               << " usage=0x" << std::hex << item.usage
               << " clone=0x" << item.clone
               << " clone_enabled=" << (item.clone_enabled ? 1 : 0)
               << " clone_target=" << (item.clone_target ? 1 : 0)
               << " views=" << std::dec << item.view_count;
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
          .key = "ToneMapUINits",
          .binding = &shader_injection.graphics_white_nits,
          .default_value = 203.f,
          .label = "UI Brightness",
          .section = "Tone Mapping",
          .tooltip = "Sets the brightness reference for UI and HUD elements. Should match Game Brightness for correct scaling.",
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
        .default_value = 0.f,
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
        .key = "SwapChainFormat",
        .binding = &swap_chain_format_setting,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .can_reset = false,
        .label = "Swap Chain Format",
        .section = "Compatibility",
        .tooltip = "Requires a game restart. HDR10 presents R10G10B10A2 with BT.2100 PQ, the only HDR output DLSS Frame Generation supports. scRGB presents R16G16B16A16_FLOAT linear, which has more headroom but is unsupported by Frame Generation.",
        .labels = {"HDR10 (PQ, DLSS FG)", "scRGB (FP16, no FG)"},
        .is_global = true,
        .is_visible = []() { return current_settings_mode >= 1; },
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
        .label = "Capture DLSS FG Tag Transfers (1 frame)",
        .section = "Debug",
        .tooltip = "One-shot: records up to 8 copy or resolve operations that use the most recently tagged DLSS FG color resource during the next frame. It records only handles, formats, and clone handles; no dump, readback, or resource redirection.",
        .on_click = []() {
          std::scoped_lock lock(downstream_draw_capture_mutex);
          dlss_fg_tag_transfer_audit_state = {
              .original_resource = dlss_fg_latest_color_original.load(std::memory_order_relaxed),
              .clone_resource = dlss_fg_latest_color_clone.load(std::memory_order_relaxed),
              .active = true,
          };
          dlss_fg_tag_transfer_capture_active.store(true, std::memory_order_relaxed);
          return false;
        },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DLSS FG Compute Writers (3 Presents)",
        .section = "Debug",
        .tooltip = "One-shot: covers three Presents to include a rendered and DLSS-generated frame. It records compute-UAV update counts, and compute-shader hashes only when their UAV is the most recently tagged DLSS FG color resource or its RenoDX FP16 clone. No dump, readback, copy, or resource redirection.",
        .on_click = []() {
          std::scoped_lock lock(downstream_draw_capture_mutex);
          dlss_fg_compute_writer_audit_state = {
              .original_resource = dlss_fg_latest_color_original.load(std::memory_order_relaxed),
              .clone_resource = dlss_fg_latest_color_clone.load(std::memory_order_relaxed),
              .initial_tag_serial = dlss_fg_color_tag_serial.load(std::memory_order_relaxed),
              .active = true,
          };
          dlss_fg_compute_uav_views.clear();
          return false;
        },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DLSS FG Tag Writers (1 frame)",
        .section = "Debug",
        .tooltip = "One-shot: records up to 16 pixel-shader hashes that write the most recently tagged DLSS FG color resource during the next frame. It records only hashes and handles; no dump, readback, copy, or resource redirection.",
        .on_click = []() {
          std::scoped_lock lock(downstream_draw_capture_mutex);
          dlss_fg_producer_audit_state = {
              .original_resource = dlss_fg_latest_color_original.load(std::memory_order_relaxed),
              .clone_resource = dlss_fg_latest_color_clone.load(std::memory_order_relaxed),
              .active = true,
          };
          return false;
        },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DLSS FG Handoff (2 Presents)",
        .section = "Debug",
        .tooltip = "One-shot: logs the color resource submitted to Streamline, its RenoDX FP16 clone, the Streamline result, and the two matching Present backbuffers. It performs no GPU readback, dump, copy, or resource redirection.",
        .on_click = []() {
          std::scoped_lock lock(dlss_fg_handoff_audit_mutex);
          dlss_fg_handoff_audit = {.armed = true};
          return false;
        },
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
        .key = "DLSSFGBypassAllProxy",
        .binding = &dlss_fg_bypass_all_proxy,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .can_reset = false,
        .label = "DLSS FG Bypass All RenoDX Proxy",
        .section = "Compatibility",
        .tooltip = "Diagnostic A/B. Skips RenoDX's final proxy draw on every Present after the first. Colors will be incorrect; test only whether focused gameplay continues updating instead of freezing.",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "DLSSFGSuppressPrePQTags",
        .binding = &dlss_fg_suppress_color_tags,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .can_reset = false,
        .label = "DLSS FG Suppress Pre-PQ Color Tags",
        .section = "Compatibility",
        .tooltip = "Experimental. Omits DL2's pre-PQ HUDLessColor and UIColorAndAlpha tags so DLSS-G uses the automatically intercepted final HDR10/PQ color. This may reduce UI reconstruction quality but avoids mixing linear inputs with PQ output.",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DLSS FG Backbuffer Barriers (128)",
        .section = "Debug",
        .tooltip = "One-shot: records up to 128 barriers for full-size swapchain backbuffers and clones, including old/new states and the current Streamline tag serial. No mutation or readback.",
        .on_click = []() {
          dlss_fg_backbuffer_barrier_capture.store(128u, std::memory_order_release);
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
          .key = "ClampSwapchainOutput",
          .binding = &shader_injection.clamp_swapchain_output,
          .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
          .default_value = 0.f,
          .can_reset = false,
          .label = "Clamp Swapchain Output (Test)",
          .section = "Compatibility",
          .tooltip = "Experimental. Clips final proxy output to [0,1] range. If this stops DLSS-G flicker, it confirms extended-range values are causing Streamline interpolation errors.",
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
      {"ToneMapUINits", 203.f},
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
      {"FrameGenerationCompatibility", 0.f},
      {"DLSSFGUseTaggedClone", 0.f},
      {"DLSSFGSuppressPrePQTags", 0.f},
      {"DLSSFGSkipGeneratedProxy", 0.f},
      {"DLSSFGBypassAllProxy", 0.f},
      {"DebugMode", 0.f},
      {"CaptureDownstreamDraws", 0.f},
      {"CaptureDownstreamTransfers", 0.f},
      {"ClampSwapchainOutput", 0.f},
  });
}

// Historical scRGB experiments are superseded for DLSS Frame Generation.
// The final presentation must be RGB10/HDR10 PQ, while intermediate scene
// resources continue using FP16 clones to preserve the game's HDR signal.

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
      // Registered before mods::swapchain::Use below so the DLSS-G input
      // fence is observed before proxy clones are released during resize.
      reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);

      // Shader hook config (applies to both D3D11 and D3D12 custom shaders)
      // DL2 shared.h uses register(b13, space50) for SM5.1+ (D3D12) and
      // register(b13) for SM5.0 (D3D11). mods::shader::OnInitDevice reads
      // expected_constant_buffer_space only for d3d12/vulkan, so setting 50
      // here is correct: D3D12 gets 50, D3D11 keeps default 0.
      renodx::mods::shader::expected_constant_buffer_index = 13;
      renodx::mods::shader::expected_constant_buffer_space = 50;
      renodx::mods::shader::allow_multiple_push_constants = true;
      renodx::mods::shader::force_pipeline_cloning = true;

      // Final presentation container. HDR10 (RGB10 + BT.2100 PQ) is the only
      // HDR output DLSS Frame Generation supports; scRGB stays available as a
      // no-FG fallback. Scene resources keep their FP16 clones either way.
      {
        int32_t format_choice = 0;
        reshade::get_config_value(nullptr, renodx::utils::settings::global_name.c_str(), "SwapChainFormat", format_choice);
        swap_chain_format_setting = static_cast<float>(format_choice);
        const bool use_hdr10 = (format_choice == 0);
        swap_chain_use_hdr10 = use_hdr10 ? 1.f : 0.f;
        shader_injection.renodrt_padding_1 = swap_chain_use_hdr10;
        renodx::mods::swapchain::SetUseHDR10(use_hdr10);
        shader_injection.swap_chain_encoding =
            use_hdr10 ? 4.f /* ENCODING_PQ */ : 5.f /* ENCODING_SCRGB */;
        shader_injection.swap_chain_output_preset =
            use_hdr10 ? 1.f /* HDR10 */ : 2.f /* SCRGB */;
        std::stringstream s;
        s << "DL2 swapchain format: " << (use_hdr10 ? "HDR10 (RGB10+PQ)" : "scRGB (FP16)");
        reshade::log::message(reshade::log::level::info, s.str().c_str());
      }
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
      reshade::unregister_event<reshade::addon_event::barrier>(OnDlssFgBackbufferBarrier);
      reshade::unregister_event<reshade::addon_event::copy_resource>(OnDownstreamCopyResource);
      reshade::unregister_event<reshade::addon_event::copy_texture_region>(OnDownstreamCopyTextureRegion);
      reshade::unregister_event<reshade::addon_event::resolve_texture_region>(OnDownstreamResolveTextureRegion);
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnDownstreamBindRenderTargets);
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnGammaAuditPushDescriptors);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
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
    reshade::register_event<reshade::addon_event::barrier>(OnDlssFgBackbufferBarrier);
  }

  return TRUE;
}
