/*
 * Copyright (C) 2025 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#include <d3d11.h>
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <embed/shaders.h>

#include <wrl/client.h>

#include <deps/imgui/imgui.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <include/reshade.hpp>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/build_info.hpp"
#include "../../utils/constants.hpp"
#include "../../utils/descriptor.hpp"
#include "../../utils/dlss/DXGISwapChainWrapper.hpp"
#include "../../utils/log.hpp"
#include "../../utils/pipeline_layout.hpp"
#include "../../utils/resource.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/vtable.hpp"
#include "./dl2_descriptor_override.hpp"
#include "./shared.h"

namespace {

ShaderInjectData shader_injection;
HMODULE addon_module = nullptr;

bool ArmStreamlinePresentTrace() {
  if (addon_module == nullptr) {
    renodx::utils::log::e("DL2 Streamline Present1 trace arm failed: addon module unavailable");
    return false;
  }

  std::array<wchar_t, 32768> module_path = {};
  const DWORD length = GetModuleFileNameW(
      addon_module, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (length == 0 || length >= module_path.size()) {
    std::ostringstream stream;
    stream << "DL2 Streamline Present1 trace arm failed: module path error="
           << GetLastError();
    renodx::utils::log::e(stream.str().c_str());
    return false;
  }

  std::wstring marker_path(module_path.data(), length);
  const size_t separator = marker_path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    renodx::utils::log::e("DL2 Streamline Present1 trace arm failed: module directory unavailable");
    return false;
  }
  marker_path.resize(separator + 1);
  marker_path += L"dl2_streamline_trace.arm";

  const HANDLE marker = CreateFileW(
      marker_path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (marker == INVALID_HANDLE_VALUE) {
    std::ostringstream stream;
    stream << "DL2 Streamline Present1 trace arm failed: create error="
           << GetLastError();
    renodx::utils::log::e(stream.str().c_str());
    return false;
  }
  CloseHandle(marker);
  renodx::utils::log::i("DL2 Streamline Present1 trace armed: frames=256");
  return true;
}

// 0 = HDR10 (RGB10 + BT.2100 PQ), 1 = scRGB (FP16 + linear).
// DLSS Frame Generation requires HDR10/PQ, so that is the default. The value is
// read once in DllMain because a swapchain container cannot change at runtime.
float swap_chain_format_setting = 0.f;
// Immutable copy used by the proxy shader. ReShade may reload the global
// setting after DllMain, but the swapchain container cannot change then.
float swap_chain_use_hdr10 = 1.f;

float dlss_fg_tag_clone = 0.f;
float dlss_fg_suppress_color_tags = 0.f;
float dlss_fg_aux_tag_mode = 0.f;
float dlss_fg_color_buffer_format_mode = 0.f;
float dlss_fg_skip_generated_proxy = 0.f;
float dlss_fg_bypass_all_proxy = 0.f;
float dlss_fg_final_color_mode = 0.f;
float dlss_fg_creation_format_fix = 1.f;
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
std::atomic_int32_t dlss_fg_aux_tag_mode_logged = -1;
std::atomic_int32_t dlss_fg_color_buffer_format_mode_latched = -1;

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
decltype(&slGetNativeInterface) real_sl_get_native_interface = nullptr;
decltype(&slDLSSGSetOptions) real_sl_dlssg_set_options = nullptr;
decltype(&slDLSSSetOptions) real_sl_dlss_set_options = nullptr;
decltype(&slDLSSGGetState) real_sl_dlssg_get_state = nullptr;
using SlDlssGHookPresent = HRESULT(IDXGISwapChain*, UINT, UINT, bool&);
using SlDlssGHookPresent1 = HRESULT(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, bool&);
SlDlssGHookPresent* real_sl_dlssg_hook_present = nullptr;
SlDlssGHookPresent1* real_sl_dlssg_hook_present1 = nullptr;
bool dlss_fg_options_logged = false;
bool dlss_fg_options_hook_installed = false;
bool dlss_sr_options_hook_installed = false;
bool dlss_sr_options_logged = false;
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
  uint32_t resource_format = 0u;
  uint32_t buffer_count = 0u;
  uint32_t back_buffer_index = 0u;
  uint64_t serial = 0u;
};

std::mutex dlss_fg_swapchain_snapshot_mutex;
std::array<DlssFgSwapchainSnapshot, 8> dlss_fg_swapchain_snapshots = {};
uint64_t dlss_fg_swapchain_snapshot_serial = 0u;
std::atomic_uintptr_t dlss_fg_active_swapchain = 0u;
std::atomic_uint32_t dlss_fg_present_identity_count = 0u;
std::atomic_uint32_t dlss_fg_reshade_identity_count = 0u;
std::atomic_uint64_t dlss_fg_identity_event_serial = 0u;
std::atomic_uint32_t dlss_fg_exact_ad_ordering_remaining = 0u;
std::atomic_uint32_t dlss_fg_post_execute_diagnostic_count = 0u;
struct DlssFgTimingSnapshot {
  uint64_t last_execute_event = 0u;
  uint64_t last_execute_backbuffer = 0u;
  uint64_t last_ad_execute_event = 0u;
  uint64_t last_ad_command_list = 0u;
  uint64_t last_ad_epoch = 0u;
  uint64_t last_ad_target = 0u;
  uint64_t last_ad_effective_target = 0u;
  uint64_t last_post_proxy_event = 0u;
  uint64_t last_post_proxy_backbuffer = 0u;
  uint64_t last_present_entry_event = 0u;
  uint64_t last_present_entry_backbuffer = 0u;
  uint32_t last_present_entry_index = 0u;
  uint64_t last_consumed_ad_event = 0u;
  uint64_t last_consumed_ad_target = 0u;
  uint64_t last_consumed_ad_effective_target = 0u;
};
std::mutex dlss_fg_timing_mutex;
DlssFgTimingSnapshot dlss_fg_timing = {};

struct DlssFgPreservedCopyRecord {
  uint64_t event = 0u;
  uint64_t back_buffer = 0u;
  uint64_t copy_source = 0u;
  uint64_t command_buffer = 0u;
  uint32_t tag_serial = 0u;
  uint32_t frame_index = UINT_MAX;
  uint32_t source_format = 0u;
  bool tag_context_match = false;
  bool roundtrip = false;
};

struct DlssFgFrameClassificationAudit {
  std::array<DlssFgPreservedCopyRecord, 64> pending_copies = {};
  uint64_t generation = 0u;
  uint64_t last_ad_submission_serial = 0u;
  uint32_t pending_count = 0u;
  uint32_t dropped_count = 0u;
  uint32_t present_count = 0u;
  uint32_t remaining = 0u;
  uint32_t last_tag_serial = 0u;
};

std::mutex dlss_fg_frame_classification_mutex;
DlssFgFrameClassificationAudit dlss_fg_frame_classification = {};
std::atomic_uint32_t dlss_fg_frame_classification_remaining = 0u;
std::atomic_uint64_t dlss_fg_frame_classification_generation = 0u;
std::atomic_uint64_t dlss_fg_ad_submission_serial = 0u;
std::atomic_bool dlss_fg_mode_active = false;
std::atomic_uint32_t dlss_fg_execute_candidate_remaining = 0u;
std::atomic_uint32_t dlss_fg_final_copy_diagnostic_count = 0u;
std::atomic_uint32_t dlss_fg_final_copy_match_diagnostic_count = 0u;
std::atomic_uint64_t dlss_fg_swapchain_generation = 1u;

struct DlssFgTaggedResourceSnapshot {
  uint64_t resource = 0u;
  uint32_t native_format = 0u;
  uint32_t actual_format = 0u;
  uint32_t tagged_width = 0u;
  uint32_t tagged_height = 0u;
  uint32_t actual_width = 0u;
  uint32_t actual_height = 0u;
};

struct DlssFgInputSnapshot {
  uint64_t generation = 0u;
  uint32_t tag_serial = 0u;
  uint32_t frame_index = UINT_MAX;
  bool color_committed = false;
  bool final_copy_consumed = false;
  std::array<uint64_t, 4> command_buffers = {};
  uint32_t command_buffer_count = 0u;
  DlssFgTaggedResourceSnapshot depth = {};
  DlssFgTaggedResourceSnapshot motion = {};
  DlssFgTaggedResourceSnapshot exposure = {};
  DlssFgTaggedResourceSnapshot scaling_input = {};
  DlssFgTaggedResourceSnapshot scaling_output = {};
  DlssFgTaggedResourceSnapshot hudless = {};
  DlssFgTaggedResourceSnapshot ui = {};
};

std::mutex dlss_fg_input_snapshot_mutex;
std::array<DlssFgInputSnapshot, 64> dlss_fg_input_snapshots = {};
std::atomic_uint32_t dlss_fg_input_snapshot_evictions = 0u;

struct DlssFgCommandListCandidate {
  uint64_t swapchain_generation = 0u;
  uint64_t back_buffer = 0u;
  uint64_t copy_source = 0u;
  uint32_t transition_count = 0u;
  uint32_t tag_serial = 0u;
  uint32_t frame_index = UINT_MAX;
  uint32_t first_old_state = 0u;
  uint32_t last_new_state = 0u;
  bool entered_render_target = false;
  bool returned_to_present = false;
  bool bound_swapchain_rtv = false;
  bool copied_to_swapchain = false;
  bool preserved_native_copy = false;
  bool tag_context_match = false;
};

struct DlssFgAdCommandListMarker {
  uint64_t epoch = 0u;
  uint64_t target = 0u;
  uint64_t effective_target = 0u;
};

// Protected by downstream_draw_capture_mutex.
std::unordered_map<uintptr_t, DlssFgAdCommandListMarker> dlss_fg_ad_command_list_markers;

// The ReShade pre-submit event records the exact AD command list, while the
// native queue hook consumes it after the queue has accepted the list.
std::mutex dlss_fg_post_execute_mutex;
std::unordered_map<uintptr_t, DlssFgAdCommandListMarker> dlss_fg_post_execute_markers;

struct DlssFgBridgePass {
  reshade::api::resource resource = {};
  std::unique_ptr<renodx::utils::render::RenderPass> pass;
  Microsoft::WRL::ComPtr<ID3D12Fence> producer_fence;
  Microsoft::WRL::ComPtr<ID3D12Fence> consumer_fence;
  std::mutex recording_mutex;
  uint32_t slot = 0u;
  uint64_t handoff_epoch = 0u;
  uint64_t producer_serial = 0u;
  uint64_t producer_fence_value = 0u;
  uint64_t consumer_fence_value = 0u;
  bool producer_pending = false;
  bool consumer_reserved = false;
  bool consumer_assigned = false;
};

// Completion-tracked RGB10 slots prepared on Direct producer lists. Streamline
// Compute lists only copy submitted slots into their exact trays.
std::unordered_map<uint64_t, std::vector<std::shared_ptr<DlssFgBridgePass>>>
    dlss_fg_ad_prepared_sources;
std::vector<std::shared_ptr<DlssFgBridgePass>> dlss_fg_retired_prepared_sources;
std::unordered_map<uint64_t, std::weak_ptr<DlssFgBridgePass>> dlss_fg_prepared_slots;

struct DlssFgBridgeComputeUse {
  std::shared_ptr<DlssFgBridgePass> prepared;
  uint64_t handoff_epoch = 0u;
  uint64_t producer_serial = 0u;
  uint64_t producer_fence_value = 0u;
  uint64_t tray = 0u;
  uint64_t generation = 0u;
};

struct DlssFgBridgedTray {
  uint64_t swapchain_generation = 0u;
  uint64_t handoff_epoch = 0u;
  uint64_t producer_serial = 0u;
  uint64_t consumer_fence_value = 0u;
};

std::mutex dlss_fg_bridge_mutex;
std::unordered_map<uint64_t, uint64_t> dlss_fg_ad_effective_targets;
std::unordered_map<uint64_t, DlssFgBridgedTray> dlss_fg_bridged_trays;
std::unordered_map<uint64_t, reshade::api::resource_usage> dlss_fg_ad_clone_states;
std::atomic_uint32_t dlss_fg_bridge_diagnostic_count = 0u;
std::atomic_uint32_t dlss_fg_bridge_candidate_diagnostic_count = 0u;
std::atomic_bool dlss_fg_rgb10_uav_supported = false;
std::atomic_uint64_t dlss_fg_handoff_epoch = 1u;
std::mutex dlss_fg_prepared_command_mutex;
std::unordered_map<uintptr_t, std::unordered_map<uint64_t, uint64_t>>
    dlss_fg_prepared_command_resources;
std::unordered_map<uint64_t, uint64_t> dlss_fg_prepared_resource_record_serials;
std::unordered_map<uintptr_t, std::vector<DlssFgBridgeComputeUse>>
    dlss_fg_bridge_compute_uses;

void DestroyDlssFgBridgePasses(reshade::api::device* device) {
  if (device == nullptr) return;
  {
    std::scoped_lock lock(dlss_fg_prepared_command_mutex);
    dlss_fg_prepared_command_resources.clear();
    dlss_fg_prepared_resource_record_serials.clear();
    dlss_fg_bridge_compute_uses.clear();
    dlss_fg_prepared_slots.clear();
  }
  {
    std::scoped_lock lock(dlss_fg_bridge_mutex);
    dlss_fg_ad_effective_targets.clear();
    dlss_fg_bridged_trays.clear();
    dlss_fg_ad_clone_states.clear();
  }
  std::vector<std::shared_ptr<DlssFgBridgePass>> prepared_sources;
  {
    std::scoped_lock lock(dlss_fg_bridge_mutex);
    for (const auto& entry : dlss_fg_ad_prepared_sources) {
      prepared_sources.insert(
          prepared_sources.end(), entry.second.begin(), entry.second.end());
    }
    dlss_fg_ad_prepared_sources.clear();
    prepared_sources.insert(
        prepared_sources.end(),
        dlss_fg_retired_prepared_sources.begin(), dlss_fg_retired_prepared_sources.end());
    dlss_fg_retired_prepared_sources.clear();
  }
  for (auto& prepared : prepared_sources) {
    std::scoped_lock recording_lock(prepared->recording_mutex);
    prepared->pass->DestroyAll(device);
    if (prepared->resource.handle != 0u) device->destroy_resource(prepared->resource);
  }
}

using DlssFgNativeExecuteCommandLists = void(STDMETHODCALLTYPE*)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
DlssFgNativeExecuteCommandLists real_dlss_fg_native_execute_command_lists = nullptr;
bool dlss_fg_native_execute_hook_installed = false;
std::mutex dlss_fg_native_queue_mutex;
std::unordered_map<uintptr_t, reshade::api::command_queue*> dlss_fg_native_queues;

void RegisterDlssFgNativeQueue(reshade::api::command_queue* queue);
void UnregisterDlssFgNativeQueue(reshade::api::command_queue* queue);
void STDMETHODCALLTYPE HookedDlssFgNativeExecuteCommandLists(
    ID3D12CommandQueue* queue,
    UINT count,
    ID3D12CommandList* const* command_lists);
void ProcessDlssFgNativePostExecute(
    reshade::api::command_queue* queue,
    ID3D12CommandList* command_list);

std::mutex dlss_fg_command_list_candidate_mutex;
std::unordered_map<uintptr_t, DlssFgCommandListCandidate> dlss_fg_command_list_candidates;

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

DlssFgWaitResult WaitForDlssFgInputs(uint64_t* target_value, uint64_t* completed_before) {
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

const char* DlssFgWaitResultName(DlssFgWaitResult result) {
  switch (result) {
    case DlssFgWaitResult::no_fence:         return "no_fence";
    case DlssFgWaitResult::already_complete: return "already_complete";
    case DlssFgWaitResult::wait_completed:   return "wait_completed";
    case DlssFgWaitResult::timeout:          return "timeout";
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
      auto* fence = static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
      const uint64_t completed = fence != nullptr ? fence->GetCompletedValue() : 0u;
      std::ostringstream message;
      message << "DL2 DLSS FG: GetState viewport=" << static_cast<uint32_t>(viewport)
              << " status=" << static_cast<uint32_t>(state.status)
              << " presented=" << state.numFramesActuallyPresented
              << " generate_max=" << state.numFramesToGenerateMax
              << " fence=" << static_cast<void*>(fence)
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
  Microsoft::WRL::ComPtr<ID3D12Resource> d3d12_back_buffer;
  if (SUCCEEDED(swapchain->GetBuffer(snapshot.back_buffer_index, IID_PPV_ARGS(&d3d12_back_buffer)))) {
    snapshot.back_buffer = reinterpret_cast<uintptr_t>(d3d12_back_buffer.Get());
    snapshot.resource_format = static_cast<uint32_t>(d3d12_back_buffer->GetDesc().Format);
  } else {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11_back_buffer;
    if (SUCCEEDED(swapchain->GetBuffer(snapshot.back_buffer_index, IID_PPV_ARGS(&d3d11_back_buffer)))) {
      D3D11_TEXTURE2D_DESC resource_desc = {};
      d3d11_back_buffer->GetDesc(&resource_desc);
      snapshot.back_buffer = reinterpret_cast<uintptr_t>(d3d11_back_buffer.Get());
      snapshot.resource_format = static_cast<uint32_t>(resource_desc.Format);
    }
  }
  return snapshot;
}

void CaptureDlssFgFrameClassification(const DlssFgSwapchainSnapshot& snapshot) {
  const uint64_t generation =
      dlss_fg_frame_classification_generation.load(std::memory_order_acquire);
  if (dlss_fg_frame_classification_remaining.load(std::memory_order_acquire) == 0u) return;

  DlssFgTimingSnapshot timing = {};
  uint32_t tag_serial = 0u;
  uint64_t ad_submission_serial = 0u;
  std::array<DlssFgPreservedCopyRecord, 64> copies = {};
  uint32_t copy_count = 0u;
  uint32_t unmatched_copy_count = 0u;
  uint32_t dropped_count = 0u;
  uint32_t present_index = 0u;
  uint32_t remaining = 0u;
  bool new_tag = false;
  bool new_ad = false;
  bool copied_final_color = false;
  bool conflicting_copy_tags = false;
  {
    std::scoped_lock audit_lock(
        dlss_fg_timing_mutex,
        dlss_fg_frame_classification_mutex);
    timing = dlss_fg_timing;
    auto& audit = dlss_fg_frame_classification;
    if (audit.generation != generation || audit.remaining == 0u) return;
    remaining = --audit.remaining;
    dlss_fg_frame_classification_remaining.store(remaining, std::memory_order_release);
    ad_submission_serial = dlss_fg_ad_submission_serial.load(std::memory_order_acquire);
    copy_count = audit.pending_count;
    dropped_count = audit.dropped_count;
    for (uint32_t index = 0u; index < copy_count; ++index) {
      copies[index] = audit.pending_copies[index];
    }
    for (uint32_t index = 0u; index < copy_count; ++index) {
      if (!copies[index].tag_context_match) {
        ++unmatched_copy_count;
        continue;
      }
      const uint32_t copy_tag_serial = copies[index].tag_serial;
      if (copy_tag_serial == 0u) continue;
      conflicting_copy_tags |= tag_serial != 0u && tag_serial != copy_tag_serial;
      tag_serial = copy_tag_serial;
    }
    if (conflicting_copy_tags || unmatched_copy_count != 0u) tag_serial = 0u;
    copied_final_color = tag_serial != 0u
                         && !conflicting_copy_tags
                         && unmatched_copy_count == 0u;
    if (copy_count == 0u) tag_serial = audit.last_tag_serial;
    audit.pending_count = 0u;
    audit.dropped_count = 0u;
    new_tag = copied_final_color && tag_serial != audit.last_tag_serial;
    new_ad = ad_submission_serial != audit.last_ad_submission_serial;
    if (copied_final_color) audit.last_tag_serial = tag_serial;
    audit.last_ad_submission_serial = ad_submission_serial;
    present_index = ++audit.present_count;
  }

  const char* frame_class = "unknown";
  if (new_tag || new_ad) {
    frame_class = "rendered";
  } else if (tag_serial != 0u) {
    frame_class = "generated_candidate";
  }

  DlssFgInputSnapshot input_snapshot = {};
  DlssFgInputSnapshot latest_input_snapshot = {};
  {
    std::scoped_lock input_lock(dlss_fg_input_snapshot_mutex);
    for (const auto& candidate : dlss_fg_input_snapshots) {
      if (candidate.generation != generation) continue;
      if (candidate.color_committed && candidate.frame_index != UINT_MAX
          && candidate.tag_serial > latest_input_snapshot.tag_serial) {
        latest_input_snapshot = candidate;
      }
      if (candidate.color_committed && candidate.tag_serial == tag_serial) {
        input_snapshot = candidate;
      }
    }
  }
  const bool input_matches = input_snapshot.color_committed
                             && input_snapshot.frame_index != UINT_MAX
                             && input_snapshot.tag_serial != 0u
                             && input_snapshot.tag_serial == tag_serial;

  auto append_tagged_resource = [](std::ostringstream& stream,
                                   const char* name,
                                   const DlssFgTaggedResourceSnapshot& resource) {
    stream << " " << name << "=0x" << std::hex << std::uppercase << resource.resource
           << std::dec
           << "(native=" << resource.native_format
           << ",actual=" << resource.actual_format
           << ",tagged=" << resource.tagged_width << "x" << resource.tagged_height
           << ",actual_size=" << resource.actual_width << "x" << resource.actual_height
           << ")";
  };

  std::ostringstream classification;
  classification << "DL2 DLSS FG frame classification: generation=" << generation
                 << " present=" << present_index
                 << " class=" << frame_class
                 << " new_tag=" << (new_tag ? 1 : 0)
                 << " tag=" << tag_serial
                 << " tag_source="
                 << (conflicting_copy_tags
                         ? "conflict"
                     : unmatched_copy_count != 0u
                         ? "unmatched_copy"
                     : copied_final_color
                         ? "command_context"
                         : (tag_serial != 0u ? "carried" : "none"))
                 << " copy_tag_conflict=" << (conflicting_copy_tags ? 1 : 0)
                 << " unmatched_copies=" << unmatched_copy_count
                 << " new_ad=" << (new_ad ? 1 : 0)
                 << " ad_serial=" << ad_submission_serial
                 << " ad_event=" << timing.last_ad_execute_event
                 << " consumed_ad_event=" << timing.last_consumed_ad_event
                 << " ad_target=0x" << std::hex << std::uppercase
                 << timing.last_consumed_ad_target
                 << "=>0x" << timing.last_consumed_ad_effective_target
                 << " present_entry_event=" << std::dec << timing.last_present_entry_event
                 << " present_entry_backbuffer=0x" << std::hex
                 << timing.last_present_entry_backbuffer
                 << " present_entry_index=" << std::dec
                 << timing.last_present_entry_index
                 << " backbuffer=0x" << std::hex << std::uppercase << snapshot.back_buffer
                 << " index=" << std::dec << snapshot.back_buffer_index
                 << " input_match=" << (input_matches ? 1 : 0)
                 << " input_frame=" << input_snapshot.frame_index
                 << " latest_input_tag=" << latest_input_snapshot.tag_serial
                 << " latest_input_frame=" << latest_input_snapshot.frame_index
                 << " latest_input_cmds=" << latest_input_snapshot.command_buffer_count
                 << " input_evictions="
                 << dlss_fg_input_snapshot_evictions.load(std::memory_order_acquire)
                 << " copies=" << copy_count
                 << " dropped=" << dropped_count;
  for (uint32_t index = 0u; index < latest_input_snapshot.command_buffer_count; ++index) {
    classification << " latest_cmd" << index << "=0x" << std::hex << std::uppercase
                   << latest_input_snapshot.command_buffers[index];
  }
  if (input_matches) {
    append_tagged_resource(classification, "depth", input_snapshot.depth);
    append_tagged_resource(classification, "motion", input_snapshot.motion);
    append_tagged_resource(classification, "exposure", input_snapshot.exposure);
    append_tagged_resource(classification, "scaling_in", input_snapshot.scaling_input);
    append_tagged_resource(classification, "scaling_out", input_snapshot.scaling_output);
    append_tagged_resource(classification, "hudless", input_snapshot.hudless);
    append_tagged_resource(classification, "ui", input_snapshot.ui);
  }
  for (uint32_t index = 0u; index < copy_count; ++index) {
    const auto& copy = copies[index];
    const char* storage = "other";
    if (copy.source_format
        == static_cast<uint32_t>(reshade::api::format::r16g16b16a16_float)) {
      storage = "fp16";
    } else if (copy.source_format
               == static_cast<uint32_t>(reshade::api::format::r10g10b10a2_unorm)) {
      storage = "rgb10";
    }
    classification << " #" << (index + 1u)
                   << "(event=" << copy.event
                   << ",backbuffer=0x" << std::hex << copy.back_buffer
                   << ",source=0x" << copy.copy_source
                   << ",cmd=0x" << copy.command_buffer
                   << ",format=" << std::dec << copy.source_format
                   << ",storage=" << storage
                   << ",tag=" << copy.tag_serial
                   << ",frame=" << copy.frame_index
                   << ",context=" << (copy.tag_context_match ? 1 : 0)
                   << ",action=" << (copy.roundtrip ? "roundtrip" : "direct")
                   << ",contract=pq)";
  }
  renodx::utils::log::i(classification.str().c_str());

  if (remaining == 0u) {
    std::ostringstream complete;
    complete << "DL2 DLSS FG frame classification complete: generation=" << generation
             << " presents=16";
    renodx::utils::log::i(complete.str().c_str());
  }
}

void CaptureReshadeSwapchainSnapshot(
    reshade::api::command_queue* queue,
    reshade::api::swapchain* swapchain) {
  if (queue == nullptr || swapchain == nullptr) return;

  RegisterDlssFgNativeQueue(queue);
  dlss_fg_active_swapchain.store(reinterpret_cast<uintptr_t>(swapchain), std::memory_order_release);

  DlssFgSwapchainSnapshot snapshot = ReadNativeSwapchainSnapshot(
      reinterpret_cast<IDXGISwapChain*>(swapchain->get_native()));
  snapshot.wrapper = reinterpret_cast<uintptr_t>(swapchain);
  snapshot.api = static_cast<uint32_t>(queue->get_device()->get_api());
  snapshot.hwnd = reinterpret_cast<uintptr_t>(swapchain->get_hwnd());
  snapshot.back_buffer = swapchain->get_current_back_buffer().handle;
  snapshot.resource_format = static_cast<uint32_t>(
      queue->get_device()->get_resource_desc(swapchain->get_current_back_buffer()).texture.format);

  {
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

  const uint32_t count = dlss_fg_reshade_identity_count.fetch_add(1u, std::memory_order_relaxed);
  if (count < 48u) {
    uint64_t event = 0u;
    {
      std::scoped_lock timing_lock(dlss_fg_timing_mutex);
      event = dlss_fg_identity_event_serial.fetch_add(1u, std::memory_order_relaxed) + 1u;
      dlss_fg_timing.last_post_proxy_event = event;
      dlss_fg_timing.last_post_proxy_backbuffer = snapshot.back_buffer;
    }
    std::ostringstream message;
    message << "DL2 DLSS FG ReShade present identity: event=" << event
            << " thread=" << GetCurrentThreadId()
            << " wrapper=0x" << std::hex << std::uppercase << snapshot.wrapper
            << " native=0x" << snapshot.native
            << " hwnd=0x" << snapshot.hwnd
            << " size=" << std::dec << snapshot.width << "x" << snapshot.height
            << " reported_format=" << snapshot.format
            << " resource_format=" << snapshot.resource_format
            << " buffers=" << snapshot.buffer_count
            << " index=" << snapshot.back_buffer_index
            << " backbuffer=0x" << std::hex << snapshot.back_buffer;
    renodx::utils::log::i(message.str().c_str());
  }
  CaptureDlssFgFrameClassification(snapshot);
}

struct DlssFgPresentAuditToken {
  uint64_t entry_event = 0u;
  uint64_t ad_execute_event = 0u;
  bool logged = false;
};

DlssFgPresentAuditToken LogDlssFgPresentIdentity(
    const char* call_name,
    IDXGISwapChain* swapchain,
    bool consume_ad_token) {
  DlssFgPresentAuditToken token = {};
  const uint32_t count = dlss_fg_present_identity_count.fetch_add(1u, std::memory_order_relaxed);
  if (count >= 48u) return token;

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
        if (candidate.native != 0u && candidate.back_buffer != 0u
            && candidate.back_buffer == native.back_buffer) {
          matched = candidate;
          match_kind = "backbuffer";
          break;
        }
      }
    }
    if (matched.native == 0u) {
      for (const auto& candidate : dlss_fg_swapchain_snapshots) {
        if (candidate.native != 0u && candidate.hwnd == native.hwnd
            && candidate.width == native.width && candidate.height == native.height
            && candidate.buffer_count == native.buffer_count) {
          matched = candidate;
          match_kind = "hwnd_desc";
          break;
        }
      }
    }
  }

  uint64_t event = 0u;
  DlssFgTimingSnapshot timing = {};
  {
    std::scoped_lock timing_lock(dlss_fg_timing_mutex);
    event = dlss_fg_identity_event_serial.fetch_add(1u, std::memory_order_relaxed) + 1u;
    timing = dlss_fg_timing;
    token.entry_event = event;
    dlss_fg_timing.last_present_entry_event = event;
    dlss_fg_timing.last_present_entry_backbuffer = native.back_buffer;
    dlss_fg_timing.last_present_entry_index = native.back_buffer_index;
    if (consume_ad_token) {
      token.ad_execute_event = timing.last_ad_execute_event;
      dlss_fg_timing.last_consumed_ad_event = timing.last_ad_execute_event;
      dlss_fg_timing.last_consumed_ad_target = timing.last_ad_target;
      dlss_fg_timing.last_consumed_ad_effective_target = timing.last_ad_effective_target;
    }
    token.logged = true;
    if (consume_ad_token) {
      dlss_fg_timing.last_ad_execute_event = 0u;
      dlss_fg_timing.last_ad_command_list = 0u;
      dlss_fg_timing.last_ad_epoch = 0u;
      dlss_fg_timing.last_ad_target = 0u;
      dlss_fg_timing.last_ad_effective_target = 0u;
    }
  }
  std::ostringstream message;
  message << "DL2 DLSS FG swapchain identity: call=" << call_name
          << " event=" << event
          << " thread=" << GetCurrentThreadId()
          << " hook_native=0x" << std::hex << std::uppercase << native.native
          << " hwnd=0x" << native.hwnd
          << " size=" << std::dec << native.width << "x" << native.height
          << " reported_format=" << native.format
          << " resource_format=" << native.resource_format
          << " buffers=" << native.buffer_count
          << " index=" << native.back_buffer_index
          << " backbuffer=0x" << std::hex << native.back_buffer
          << " match=" << match_kind
          << " last_execute_event=" << std::dec
          << timing.last_execute_event
          << " last_execute_backbuffer=0x" << std::hex
          << timing.last_execute_backbuffer
          << " last_ad_execute_event=" << std::dec
          << timing.last_ad_execute_event
          << " last_ad_cmd=0x" << std::hex
          << timing.last_ad_command_list
          << " last_ad_epoch=" << std::dec
          << timing.last_ad_epoch
          << " last_ad_target=0x" << std::hex
          << timing.last_ad_target
          << "=>0x" << timing.last_ad_effective_target
          << " last_post_proxy_event=" << std::dec
          << timing.last_post_proxy_event
          << " last_post_proxy_backbuffer=0x" << std::hex
          << timing.last_post_proxy_backbuffer;
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
  return token;
}

void LogDlssFgPresentResult(
    const char* call_name,
    const DlssFgPresentAuditToken& token,
    HRESULT result,
    bool skip) {
  if (!token.logged) return;
  uint64_t event = 0u;
  {
    std::scoped_lock timing_lock(dlss_fg_timing_mutex);
    event = dlss_fg_identity_event_serial.fetch_add(1u, std::memory_order_relaxed) + 1u;
  }
  std::ostringstream message;
  message << "DL2 DLSS FG swapchain result: call=" << call_name
          << " event=" << event
          << " thread=" << GetCurrentThreadId()
          << " entry_event=" << token.entry_event
          << " consumed_ad_event=" << token.ad_execute_event
          << " result=0x" << std::hex << std::uppercase << static_cast<uint32_t>(result)
          << " skip=" << std::dec << (skip ? 1 : 0);
  renodx::utils::log::i(message.str().c_str());
}

HRESULT HookedSlDlssGPresent(IDXGISwapChain* swapchain, UINT sync_interval, UINT flags, bool& skip) {
  const auto token = LogDlssFgPresentIdentity("Present", swapchain, false);
  const HRESULT result = real_sl_dlssg_hook_present(swapchain, sync_interval, flags, skip);
  LogDlssFgPresentResult("Present", token, result, skip);
  return result;
}

HRESULT HookedSlDlssGPresent1(
    IDXGISwapChain* swapchain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters,
    bool& skip) {
  const auto token = LogDlssFgPresentIdentity("Present1", swapchain, true);
  const HRESULT result = real_sl_dlssg_hook_present1(swapchain, sync_interval, flags, parameters, skip);
  LogDlssFgPresentResult("Present1", token, result, skip);
  return result;
}

class Dl2HdrSwapChainWrapper final : public DXGISwapChainWrapper {
 public:
  explicit Dl2HdrSwapChainWrapper(IDXGISwapChain4* swapchain)
      : DXGISwapChainWrapper(swapchain) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) return E_POINTER;
    if (iid == __uuidof(IUnknown)
        || iid == __uuidof(IDXGIObject)
        || iid == __uuidof(IDXGIDeviceSubObject)
        || iid == __uuidof(IDXGISwapChain)
        || iid == __uuidof(IDXGISwapChain1)
        || iid == __uuidof(IDXGISwapChain2)
        || iid == __uuidof(IDXGISwapChain3)
        || iid == __uuidof(IDXGISwapChain4)) {
      *object = static_cast<IDXGISwapChain4*>(this);
      AddRef();
      return S_OK;
    }
    return DXGISwapChainWrapper::QueryInterface(iid, object);
  }

  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override {
    const HRESULT result = DXGISwapChainWrapper::GetDesc(desc);
    if (SUCCEEDED(result) && desc != nullptr) {
      desc->BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    return result;
  }

  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override {
    const HRESULT result = DXGISwapChainWrapper::GetDesc1(desc);
    if (SUCCEEDED(result) && desc != nullptr) {
      desc->Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    return result;
  }
};

sl::Result HookedSlGetNativeInterface(void* proxy_interface, void** base_interface) {
  const sl::Result result = real_sl_get_native_interface(proxy_interface, base_interface);
  if (result != sl::Result::eOk
      || base_interface == nullptr || *base_interface == nullptr
      || proxy_interface == nullptr
      || dlss_fg_creation_format_fix < 0.5f
      || swap_chain_use_hdr10 < 0.5f) {
    return result;
  }

  Microsoft::WRL::ComPtr<IDXGISwapChain4> proxy_swapchain;
  if (FAILED(static_cast<IUnknown*>(proxy_interface)->QueryInterface(IID_PPV_ARGS(&proxy_swapchain)))) {
    return result;
  }

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  Microsoft::WRL::ComPtr<ID3D12Resource> back_buffer;
  const bool target = SUCCEEDED(proxy_swapchain->GetDesc1(&desc))
                      && desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
                      && desc.Width >= 128u && desc.Height >= 128u
                      && desc.BufferCount >= 3u
                      && SUCCEEDED(proxy_swapchain->GetBuffer(
                          0u, IID_PPV_ARGS(&back_buffer)))
                      && back_buffer->GetDesc().Format
                             == DXGI_FORMAT_R10G10B10A2_UNORM;
  if (!target) return result;

  static_cast<IUnknown*>(*base_interface)->Release();
  *base_interface = new Dl2HdrSwapChainWrapper(proxy_swapchain.Detach());

  std::ostringstream message;
  message << "DL2 DLSS FG native swapchain contract: wrapped=1 reported="
          << static_cast<uint32_t>(desc.Format)
          << " resource=" << static_cast<uint32_t>(back_buffer->GetDesc().Format)
          << " forwarded=" << static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM)
          << " buffers=" << desc.BufferCount
          << " size=" << desc.Width << "x" << desc.Height;
  renodx::utils::log::i(message.str().c_str());
  return result;
}

const char* GetStreamlineTagName(sl::BufferType type) {
  switch (type) {
    case sl::kBufferTypeDepth:
      return "Depth";
    case sl::kBufferTypeMotionVectors:
      return "MotionVectors";
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
    case sl::kBufferTypeExposure:
      return "Exposure";
    default:
      return "Other";
  }
}

DlssFgTaggedResourceSnapshot DescribeDlssFgTaggedResource(const sl::ResourceTag& tag) {
  DlssFgTaggedResourceSnapshot snapshot = {};
  if (tag.resource == nullptr || tag.resource->native == nullptr) return snapshot;

  snapshot.resource = reinterpret_cast<uint64_t>(tag.resource->native);
  snapshot.native_format = tag.resource->nativeFormat;
  snapshot.tagged_width = tag.resource->width;
  snapshot.tagged_height = tag.resource->height;
  renodx::utils::resource::GetResourceInfo(
      reshade::api::resource{static_cast<uintptr_t>(snapshot.resource)},
      [&](const renodx::utils::resource::ResourceInfo& info) {
        snapshot.actual_format = static_cast<uint32_t>(info.desc.texture.format);
        snapshot.actual_width = info.desc.texture.width;
        snapshot.actual_height = info.desc.texture.height;
      });
  return snapshot;
}

void CaptureDlssFgInputSnapshot(
    const sl::ResourceTag* tags,
    uint32_t num_tags,
    uint32_t tag_serial,
    uint32_t frame_index,
    sl::CommandBuffer* command_buffer) {
  if (dlss_fg_frame_classification_remaining.load(std::memory_order_acquire) == 0u) return;
  const uint64_t generation =
      dlss_fg_frame_classification_generation.load(std::memory_order_acquire);

  DlssFgInputSnapshot partial = {
      .generation = generation,
      .frame_index = frame_index,
  };
  if (command_buffer != nullptr) {
    partial.command_buffers[0] = reinterpret_cast<uint64_t>(command_buffer);
    partial.command_buffer_count = 1u;
  }
  for (uint32_t index = 0u; index < num_tags; ++index) {
    const auto resource = DescribeDlssFgTaggedResource(tags[index]);
    switch (tags[index].type) {
      case sl::kBufferTypeDepth:
        partial.depth = resource;
        break;
      case sl::kBufferTypeMotionVectors:
        partial.motion = resource;
        break;
      case sl::kBufferTypeExposure:
        partial.exposure = resource;
        break;
      case sl::kBufferTypeScalingInputColor:
        partial.scaling_input = resource;
        break;
      case sl::kBufferTypeScalingOutputColor:
        partial.scaling_output = resource;
        break;
      case sl::kBufferTypeHUDLessColor:
        partial.hudless = resource;
        partial.color_committed = true;
        break;
      case sl::kBufferTypeUIColorAndAlpha:
        partial.ui = resource;
        partial.color_committed = true;
        break;
      default:
        break;
    }
  }
  if (frame_index == UINT_MAX && !partial.color_committed) return;
  if (partial.color_committed) partial.tag_serial = tag_serial;

  std::scoped_lock lock(dlss_fg_input_snapshot_mutex);
  if (dlss_fg_frame_classification_generation.load(std::memory_order_acquire) != generation
      || dlss_fg_frame_classification_remaining.load(std::memory_order_acquire) == 0u) {
    return;
  }
  DlssFgInputSnapshot* snapshot = nullptr;
  for (auto& candidate : dlss_fg_input_snapshots) {
    const bool same_generation = candidate.generation == generation;
    const bool same_frame = same_generation && frame_index != UINT_MAX
                            && candidate.frame_index == frame_index;
    const bool same_serial = same_generation && partial.color_committed
                             && candidate.color_committed
                             && candidate.tag_serial != 0u
                             && candidate.tag_serial == tag_serial;
    if (same_frame || same_serial) {
      snapshot = &candidate;
      break;
    }
  }
  if (snapshot == nullptr) {
    const uint32_t slot_key = partial.color_committed ? tag_serial : frame_index;
    snapshot = &dlss_fg_input_snapshots[slot_key % dlss_fg_input_snapshots.size()];
    if (snapshot->color_committed
        && (snapshot->tag_serial != partial.tag_serial
            || snapshot->frame_index != partial.frame_index)) {
      dlss_fg_input_snapshot_evictions.fetch_add(1u, std::memory_order_relaxed);
    }
    *snapshot = partial;
    return;
  }
  snapshot->frame_index = frame_index;
  if (partial.color_committed) {
    snapshot->tag_serial = tag_serial;
    snapshot->color_committed = true;
  }
  auto merge = [](DlssFgTaggedResourceSnapshot& target,
                  const DlssFgTaggedResourceSnapshot& source) {
    if (source.resource != 0u) target = source;
  };
  merge(snapshot->depth, partial.depth);
  merge(snapshot->motion, partial.motion);
  merge(snapshot->exposure, partial.exposure);
  merge(snapshot->scaling_input, partial.scaling_input);
  merge(snapshot->scaling_output, partial.scaling_output);
  merge(snapshot->hudless, partial.hudless);
  merge(snapshot->ui, partial.ui);
  for (uint32_t index = 0u; index < partial.command_buffer_count; ++index) {
    const uint64_t command_context = partial.command_buffers[index];
    const bool known = std::find(
                           snapshot->command_buffers.begin(),
                           snapshot->command_buffers.begin() + snapshot->command_buffer_count,
                           command_context)
                       != snapshot->command_buffers.begin() + snapshot->command_buffer_count;
    if (!known && snapshot->command_buffer_count < snapshot->command_buffers.size()) {
      snapshot->command_buffers[snapshot->command_buffer_count++] = command_context;
    }
  }
}

uint32_t MarkStreamlineColorTagSubmission(
    const sl::ResourceTag* tags,
    uint32_t num_tags) {
  if (tags == nullptr) {
    return dlss_fg_color_tag_serial.load(std::memory_order_acquire);
  }
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
      return dlss_fg_color_tag_serial.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    }
  }
  return dlss_fg_color_tag_serial.load(std::memory_order_acquire);
}
void CaptureStreamlineTags(const char* call_name, const sl::ResourceTag* tags, uint32_t num_tags) {
  if (!dlss_fg_tag_capture || tags == nullptr) return;

  bool has_fg_color_tag = false;
  for (uint32_t index = 0u; index < num_tags; ++index) {
    const auto type = tags[index].type;
    if (type == sl::kBufferTypeHUDLessColor || type == sl::kBufferTypeUIColorAndAlpha
        || type == sl::kBufferTypeBackbuffer) {
      has_fg_color_tag = true;
      break;
    }
  }
  if (!has_fg_color_tag) return;

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

    stream << " nativeFormat=" << tag.resource->nativeFormat
           << " size=" << tag.resource->width << "x" << tag.resource->height
           << " format=" << static_cast<uint32_t>(resource_info->desc.texture.format)
           << " usage=0x" << std::hex << static_cast<uint32_t>(resource_info->desc.usage)
           << " swapchain=" << (resource_info->is_swap_chain ? "yes" : "no")
           << " clone_enabled=" << (resource_info->clone_enabled ? "yes" : "no")
           << " clone_target=" << (resource_info->clone_target != nullptr ? "yes" : "no")
           << " views=" << std::dec << resource_info->resource_view_handles.size()
           << " clone=0x" << std::hex << resource_info->clone.handle
           << " tracked=" << (resource_info->is_clone ? "clone" : "original");

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

  const int32_t aux_tag_mode = dlss_fg_suppress_color_tags >= 0.5f
                                   ? 3
                                   : std::clamp(static_cast<int32_t>(dlss_fg_aux_tag_mode + 0.5f), 0, 3);
  const bool null_ui = aux_tag_mode == 2 || aux_tag_mode == 3;
  const bool null_hudless = aux_tag_mode == 1 || aux_tag_mode == 3;
  const bool has_aux_tag = std::any_of(tags, tags + num_tags, [](const sl::ResourceTag& tag) {
    return tag.type == sl::kBufferTypeUIColorAndAlpha || tag.type == sl::kBufferTypeHUDLessColor;
  });
  if (has_aux_tag && (null_ui || null_hudless)) {
    routed.tags_storage.assign(tags, tags + num_tags);
    routed.tags = routed.tags_storage.data();
    for (auto& tag : routed.tags_storage) {
      if ((tag.type == sl::kBufferTypeUIColorAndAlpha && null_ui)
          || (tag.type == sl::kBufferTypeHUDLessColor && null_hudless)) {
        tag.resource = nullptr;
      }
    }
    if (dlss_fg_suppress_color_tags >= 0.5f && !dlss_fg_color_tag_suppression_logged) {
      dlss_fg_color_tag_suppression_logged = true;
      renodx::utils::log::i("DL2 DLSS FG: explicitly cleared pre-PQ HUDLessColor/UIColorAndAlpha tags.");
    }
  }

  const int32_t previous_mode = has_aux_tag
                                    ? dlss_fg_aux_tag_mode_logged.exchange(aux_tag_mode, std::memory_order_relaxed)
                                    : aux_tag_mode;
  if (has_aux_tag && previous_mode != aux_tag_mode) {
    static constexpr std::array<const char*, 4> mode_names = {
        "original",
        "ui_only",
        "hudless_only",
        "none",
    };
    std::stringstream stream;
    stream << "DL2 DLSS FG auxiliary tags: mode=" << aux_tag_mode
           << " (" << mode_names[aux_tag_mode] << ")"
           << " ui=" << (null_ui ? "null" : "original")
           << " hudless=" << (null_hudless ? "null" : "original")
           << " count=" << num_tags;
    reshade::log::message(reshade::log::level::info, stream.str().c_str());
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
      routed.tags_storage.assign(tags, tags + num_tags);
      routed.tags = routed.tags_storage.data();
    }
    if (routed.resources_storage.empty()) routed.resources_storage.reserve(num_tags);

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
    if (audit.original_resource != 0u || original_tags[index].resource == nullptr) {
      continue;
    }

    const auto* original_resource = original_tags[index].resource;
    const auto* submitted_resource = routed.tags[index].resource;
    audit.original_resource = reinterpret_cast<uint64_t>(original_resource->native);
    audit.original_format = original_resource->nativeFormat;
    audit.submitted_resource = submitted_resource == nullptr
                                   ? 0u
                                   : reinterpret_cast<uint64_t>(submitted_resource->native);
    audit.submitted_format = submitted_resource == nullptr ? 0u : submitted_resource->nativeFormat;
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
         << " route=" << (audit.submitted_resource == 0u ? "null" : audit.submitted_clone ? "clone"
                                                                                          : "original")
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
  const uint32_t tag_serial = MarkStreamlineColorTagSubmission(tags, num_tags);
  CaptureStreamlineTags("slSetTag", tags, num_tags);
  const auto routed = RouteStreamlineColorTags(tags, num_tags);
  CaptureDlssFgInputSnapshot(
      routed.tags,
      routed.count,
      tag_serial,
      UINT_MAX,
      cmd_buffer);
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
  const uint32_t tag_serial = MarkStreamlineColorTagSubmission(tags, num_tags);
  CaptureStreamlineTags("slSetTagForFrame", tags, num_tags);
  const auto routed = RouteStreamlineColorTags(tags, num_tags);
  CaptureDlssFgInputSnapshot(
      routed.tags,
      routed.count,
      tag_serial,
      static_cast<uint32_t>(frame),
      cmd_buffer);
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
  const bool fg_active = corrected.mode != sl::DLSSGMode::eOff;
  const bool was_active = dlss_fg_mode_active.exchange(fg_active, std::memory_order_acq_rel);
  if (fg_active != was_active) {
    dlss_fg_handoff_epoch.fetch_add(1u, std::memory_order_acq_rel);
    std::scoped_lock bridge_lock(dlss_fg_bridge_mutex);
    dlss_fg_bridged_trays.clear();
  }
  if (fg_active && !was_active) {
    std::scoped_lock lock(dlss_fg_command_list_candidate_mutex);
    dlss_fg_command_list_candidates.clear();
    dlss_fg_identity_event_serial.store(0u, std::memory_order_release);
    {
      std::scoped_lock timing_lock(dlss_fg_timing_mutex);
      dlss_fg_timing = {};
    }
    dlss_fg_present_identity_count.store(0u, std::memory_order_release);
    dlss_fg_reshade_identity_count.store(0u, std::memory_order_release);
    dlss_fg_final_copy_diagnostic_count.store(0u, std::memory_order_release);
    dlss_fg_final_copy_match_diagnostic_count.store(0u, std::memory_order_release);
    dlss_fg_frame_classification_remaining.store(0u, std::memory_order_release);
    dlss_fg_frame_classification_generation.fetch_add(1u, std::memory_order_acq_rel);
    {
      std::scoped_lock classification_lock(dlss_fg_frame_classification_mutex);
      dlss_fg_frame_classification = {};
    }
    dlss_fg_execute_candidate_remaining.store(128u, std::memory_order_release);
    renodx::utils::log::i("DL2 DLSS FG: armed read-only backbuffer submission audit (128 candidates).");
  } else if (!fg_active && was_active) {
    {
      std::scoped_lock lock(dlss_fg_post_execute_mutex);
      dlss_fg_post_execute_markers.clear();
    }
    renodx::mods::swapchain::ClearProxyDrawBackBufferSkips();
    renodx::mods::swapchain::ClearProxySourceOverrides();
    dlss_fg_frame_classification_remaining.store(0u, std::memory_order_release);
    dlss_fg_frame_classification_generation.fetch_add(1u, std::memory_order_acq_rel);
    {
      std::scoped_lock classification_lock(dlss_fg_frame_classification_mutex);
      dlss_fg_frame_classification = {};
    }
  }
  const uint32_t incoming_color_format = corrected.colorBufferFormat;
  const int32_t requested_color_format_mode = std::clamp(
      static_cast<int32_t>(dlss_fg_color_buffer_format_mode + 0.5f), 0, 2);
  int32_t expected_color_format_mode = -1;
  dlss_fg_color_buffer_format_mode_latched.compare_exchange_strong(
      expected_color_format_mode, requested_color_format_mode, std::memory_order_acq_rel);
  const int32_t color_format_mode = dlss_fg_color_buffer_format_mode_latched.load(std::memory_order_acquire);
  if (color_format_mode == 1) {
    corrected.colorBufferFormat = static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM);
  } else if (color_format_mode == 2) {
    corrected.colorBufferFormat = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
  }
  if (dlss_fg_tag_clone >= 0.5f) {
    corrected.hudLessBufferFormat = static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    corrected.uiBufferFormat = static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT);
  }

  const auto result = real_sl_dlssg_set_options(viewport, corrected);
  if (!dlss_fg_options_logged) {
    dlss_fg_options_logged = true;
    std::stringstream stream;
    stream << "DL2 DLSS FG options: color_mode=" << color_format_mode
           << " color=" << incoming_color_format
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

sl::Result HookedSlDLSSSetOptions(
    const sl::ViewportHandle& viewport,
    const sl::DLSSOptions& options) {
  if (!dlss_sr_options_logged) {
    dlss_sr_options_logged = true;
    std::stringstream stream;
    stream << "DL2 DLSS SR options (read-only): mode=" << static_cast<int32_t>(options.mode)
           << " size=" << options.outputWidth << "x" << options.outputHeight
           << " preExposure=" << options.preExposure
           << " exposureScale=" << options.exposureScale
           << " colorBuffersHDR=" << static_cast<int32_t>(options.colorBuffersHDR)
           << " useAutoExposure=" << static_cast<int32_t>(options.useAutoExposure)
           << " result=forwarded";
    renodx::utils::log::i(stream.str().c_str());
  }
  return real_sl_dlss_set_options(viewport, options);
}

bool TryInstallDlssSrOptionsHook(HMODULE module) {
  if (dlss_sr_options_hook_installed) return true;
  auto get_feature_function = reinterpret_cast<decltype(&slGetFeatureFunction)>(
      GetProcAddress(module, "slGetFeatureFunction"));
  if (get_feature_function == nullptr) return false;
  void* function = nullptr;
  if (get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", function) != sl::Result::eOk
      || function == nullptr) return false;
  real_sl_dlss_set_options = reinterpret_cast<decltype(&slDLSSSetOptions)>(function);
  if (DetourTransactionBegin() != NO_ERROR) return false;
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
      || DetourAttach(reinterpret_cast<void**>(&real_sl_dlss_set_options),
                      reinterpret_cast<void*>(&HookedSlDLSSSetOptions))
             != NO_ERROR
      || DetourTransactionCommit() != NO_ERROR) {
    DetourTransactionAbort();
    real_sl_dlss_set_options = nullptr;
    return false;
  }
  dlss_sr_options_hook_installed = true;
  renodx::utils::log::i("DL2 DLSS SR: read-only options hook installed.");
  return true;
}

const auto& GetStreamlineHooks() {
  static const std::array<renodx::utils::vtable::HookItem, 3> hooks = {
      renodx::utils::vtable::HookItem{
          "slGetNativeInterface",
          reinterpret_cast<void**>(&real_sl_get_native_interface),
          reinterpret_cast<void*>(&HookedSlGetNativeInterface),
      },
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
             reinterpret_cast<void*>(&HookedSlDLSSGSetOptions))
             != NO_ERROR
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
             reinterpret_cast<void*>(&HookedSlDLSSGGetState))
             != NO_ERROR
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
             reinterpret_cast<void*>(&HookedSlDlssGPresent))
             != NO_ERROR
      || DetourAttach(
             reinterpret_cast<void**>(&real_sl_dlssg_hook_present1),
             reinterpret_cast<void*>(&HookedSlDlssGPresent1))
             != NO_ERROR
      || DetourTransactionCommit() != NO_ERROR) {
    DetourTransactionAbort();
    real_sl_dlssg_hook_present = nullptr;
    real_sl_dlssg_hook_present1 = nullptr;
    return false;
  }
  dlss_fg_present_hook_installed = true;
  dlss_fg_present_hook_wait_logged = false;
  renodx::utils::log::i("DL2 DLSS FG: Present identity hooks installed.");
  return true;
}

void TryInstallStreamlineHook() {
  if (dlss_fg_hook_installed && dlss_fg_options_hook_installed && dlss_fg_state_hook_installed
      && dlss_fg_present_hook_installed && dlss_sr_options_hook_installed) return;

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
    renodx::utils::log::w("DL2 DLSS FG: Present identity hooks were not installed.");
  }
  TryInstallDlssSrOptionsHook(module);
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
               reinterpret_cast<void*>(&HookedSlDLSSGSetOptions))
               == NO_ERROR) {
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
               reinterpret_cast<void*>(&HookedSlDLSSGGetState))
               == NO_ERROR) {
      DetourTransactionCommit();
    } else {
      DetourTransactionAbort();
    }
  }
  real_sl_dlssg_get_state = nullptr;
  dlss_fg_state_hook_installed = false;
  dlss_fg_state_hook_wait_logged = false;
  if (dlss_fg_present_hook_installed) {
    bool detached = false;
    if (DetourTransactionBegin() == NO_ERROR
        && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
        && DetourDetach(
               reinterpret_cast<void**>(&real_sl_dlssg_hook_present),
               reinterpret_cast<void*>(&HookedSlDlssGPresent))
               == NO_ERROR
        && DetourDetach(
               reinterpret_cast<void**>(&real_sl_dlssg_hook_present1),
               reinterpret_cast<void*>(&HookedSlDlssGPresent1))
               == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR) {
      detached = true;
    } else {
      DetourTransactionAbort();
      renodx::utils::log::w("DL2 DLSS FG: Present detour removal failed; retaining hook state for retry.");
    }
    if (detached) {
      real_sl_dlssg_hook_present = nullptr;
      real_sl_dlssg_hook_present1 = nullptr;
      dlss_fg_present_hook_installed = false;
      dlss_fg_present_hook_wait_logged = false;
    }
  }
  dlss_fg_mode_active.store(false, std::memory_order_release);
  dlss_fg_execute_candidate_remaining.store(0u, std::memory_order_release);
  {
    std::scoped_lock candidate_lock(dlss_fg_command_list_candidate_mutex);
    dlss_fg_command_list_candidates.clear();
  }
  {
    std::scoped_lock post_execute_lock(dlss_fg_post_execute_mutex);
    dlss_fg_post_execute_markers.clear();
  }
  {
    std::scoped_lock bridge_lock(dlss_fg_bridge_mutex);
    dlss_fg_bridged_trays.clear();
  }
  std::scoped_lock lock(dlss_fg_fence_mutex);
  if (dlss_fg_inputs_fence != nullptr) dlss_fg_inputs_fence->Release();
  dlss_fg_inputs_fence = nullptr;
  dlss_fg_inputs_fence_value = 0u;
  dlss_fg_retained_resources.clear();
}

void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool resize) {
  auto* device = swapchain != nullptr ? swapchain->get_device() : nullptr;
  dlss_fg_active_swapchain.store(0u, std::memory_order_release);
  const uint64_t generation = dlss_fg_swapchain_generation.fetch_add(1u, std::memory_order_acq_rel) + 1u;
  dlss_fg_handoff_epoch.fetch_add(1u, std::memory_order_acq_rel);
  {
    std::scoped_lock candidate_lock(dlss_fg_command_list_candidate_mutex);
    dlss_fg_command_list_candidates.clear();
  }
  {
    std::scoped_lock lock(dlss_fg_post_execute_mutex);
    dlss_fg_post_execute_markers.clear();
  }
  renodx::mods::swapchain::ClearProxyDrawBackBufferSkips();
  renodx::mods::swapchain::ClearProxySourceOverrides();
  std::ostringstream generation_message;
  generation_message << "DL2 swapchain generation advanced to " << generation
                     << " (resize=" << (resize ? "yes" : "no") << "); stale FG state cleared.";
  renodx::utils::log::i(generation_message.str().c_str());
  if (!resize) {
    DestroyDlssFgBridgePasses(device);
    return;
  }
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
  } else {
    DestroyDlssFgBridgePasses(device);
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
constexpr uint32_t kDL2TonemapperCurveFloatCount = 5u;

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
  reshade::api::format view_format = reshade::api::format::unknown;
  reshade::api::format effective_view_format = reshade::api::format::unknown;
  uint32_t width = 0u;
  uint32_t height = 0u;
  bool clone_enabled = false;
};

struct DownstreamDrawCaptureState {
  std::array<uint32_t, 64> hashes = {};
  std::array<bool, 64> is_compute = {};
  std::array<DownstreamTarget, 64> targets = {};
  std::array<DownstreamTarget, 64> inputs = {};
  std::array<DownstreamTransfer, 16> transfers = {};
  uint32_t count = 0u;
  uint32_t transfer_count = 0u;
  bool active = false;
  bool consumed = false;
  bool capture_commands = false;
  bool capture_transfers = false;
  uint32_t anchor_shader_hash = 0u;
  uint64_t gamma_target = 0u;
  reshade::api::format gamma_target_format = reshade::api::format::unknown;
  uint32_t gamma_target_width = 0u;
  uint32_t gamma_target_height = 0u;
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
  reshade::api::format view_format = reshade::api::format::unknown;
  reshade::api::format effective_view_format = reshade::api::format::unknown;
  uint32_t width = 0u;
  uint32_t height = 0u;
  int32_t creation_index = -1;
  int32_t upgrade_index = -1;
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

struct UpscalerColorPathEntry {
  uint32_t shader_hash = 0u;
  uint32_t input_writer_hash = 0u;
  std::array<uint32_t, 16> input_writer_hashes = {};
  uint32_t input_writer_count = 0u;
  uint32_t api = 0u;
  uint64_t generation = 0u;
  uint32_t draw_count = 0u;
  uint32_t instance_count = 0u;
  uint32_t input_table = UINT_MAX;
  uint32_t input_binding = UINT_MAX;
  uint32_t viewport_width = 0u;
  uint32_t viewport_height = 0u;
  uint32_t sequence = 0u;
  uint32_t present_index = 0u;
  GammaAuditResource input = {};
  GammaAuditResource output = {};
  uint64_t command_list = 0u;
  uint64_t command_list_epoch = 0u;
  uint64_t execute_serial = 0u;
  uint64_t bound_pipeline = 0u;
  uint64_t replacement_pipeline = 0u;
  bool replacement_bound = false;
};

struct UpscalerColorPathAuditState {
  std::array<UpscalerColorPathEntry, 64> entries = {};
  uint64_t capture_id = 0u;
  uint64_t start_generation = 0u;
  uint32_t count = 0u;
  uint32_t presents = 0u;
  uint32_t sequence = 0u;
  bool active = false;
};

struct UpscalerColorWriterObservation {
  uint32_t shader_hash = 0u;
  uint64_t resource = 0u;
  uint64_t command_list = 0u;
  uint64_t command_list_epoch = 0u;
  uint64_t execute_serial = 0u;
  GammaAuditResource input = {};
  GammaAuditResource output = {};
};

struct UpscalerCurveAudit {
  uint64_t resource = 0u;
  uint64_t offset = 0u;
  uint64_t size = 0u;
  std::array<uint32_t, 8> bits = {};
  std::array<float, 8> values = {};
  uint32_t cached_float_count = 0u;
  bool cache_available = false;
};

struct UpscalerMappedBuffer {
  uint8_t* data = nullptr;
  uint64_t offset = 0u;
  uint64_t size = 0u;
};

struct UpscalerInputAuditEntry {
  uint64_t generation = 0u;
  uint32_t api = 0u;
  uint32_t draw_count = 0u;
  uint32_t instance_count = 0u;
  uint32_t present_index = 0u;
  uint32_t t0_table = UINT_MAX;
  uint32_t t0_binding = UINT_MAX;
  uint32_t t1_table = UINT_MAX;
  uint32_t t1_binding = UINT_MAX;
  uint32_t cb0_table = UINT_MAX;
  uint32_t cb0_binding = UINT_MAX;
  GammaAuditResource source = {};
  GammaAuditResource exposure = {};
  UpscalerCurveAudit curve = {};
};

struct UpscalerInputAuditState {
  std::array<UpscalerInputAuditEntry, 16> entries = {};
  uint64_t capture_id = 0u;
  uint64_t start_generation = 0u;
  uint32_t count = 0u;
  uint32_t presents = 0u;
  bool active = false;
};

enum class UpscalerSourceWriterType : uint8_t {
  draw,
  dispatch,
  copy_resource,
  copy_texture_region,
  resolve_texture_region,
};

struct UpscalerSourceWriter {
  UpscalerSourceWriterType type = UpscalerSourceWriterType::draw;
  uint32_t shader_hash = 0u;
  uint32_t present_index = 0u;
  uint64_t command_buffer = 0u;
  uint64_t source = 0u;
  uint64_t target = 0u;
};

struct UpscalerSourceWriterAuditState {
  std::array<UpscalerSourceWriter, 64> writers = {};
  std::array<GammaAuditResource, 128> compute_candidates = {};
  uint64_t capture_id = 0u;
  uint64_t source = 0u;
  uint64_t effective_source = 0u;
  uint64_t exposure = 0u;
  uint64_t effective_exposure = 0u;
  GammaAuditResource source_info = {};
  std::array<uint64_t, 8> recent_sources = {};
  std::array<uint64_t, 8> recent_effective_sources = {};
  uint32_t recent_source_count = 0u;
  uint32_t count = 0u;
  uint32_t compute_candidate_count = 0u;
  uint32_t presents = 0u;
  bool target_final_fg_output = false;
  bool active = false;
};

DownstreamDrawCaptureState downstream_draw_capture_state = {};
GammaDrawAuditState gamma_draw_audit_state = {};
GammaNativeInputAuditState gamma_native_input_audit_state = {};
DlssFgProducerAuditState dlss_fg_producer_audit_state = {};
DlssFgComputeWriterAuditState dlss_fg_compute_writer_audit_state = {};
DlssFgTagTransferAuditState dlss_fg_tag_transfer_audit_state = {};
UpscalerColorPathAuditState upscaler_color_path_audit_state = {};
struct UpscalerColorWriterKey {
  uint64_t resource = 0u;
  uint64_t command_list = 0u;
  uint64_t epoch = 0u;
  bool operator==(const UpscalerColorWriterKey&) const = default;
};
struct UpscalerColorWriterKeyHash {
  size_t operator()(const UpscalerColorWriterKey& key) const noexcept {
    size_t hash = std::hash<uint64_t>{}(key.resource);
    hash ^= std::hash<uint64_t>{}(key.command_list) + 0x9E3779B97F4A7C15ull + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<uint64_t>{}(key.epoch) + 0x9E3779B97F4A7C15ull + (hash << 6u) + (hash >> 2u);
    return hash;
  }
};
std::unordered_map<UpscalerColorWriterKey, uint32_t, UpscalerColorWriterKeyHash> upscaler_color_last_writers;
std::unordered_map<UpscalerColorWriterKey, std::vector<uint32_t>, UpscalerColorWriterKeyHash> upscaler_color_writer_chains;
uint64_t upscaler_color_path_capture_serial = 0u;
std::unordered_map<uint64_t, uint64_t> upscaler_color_command_epochs;
std::vector<UpscalerColorWriterObservation> upscaler_color_writer_observations;
uint64_t upscaler_color_execute_serial = 0u;
UpscalerInputAuditState upscaler_input_audit_state = {};
uint64_t upscaler_input_audit_capture_serial = 0u;
UpscalerSourceWriterAuditState upscaler_source_writer_audit_state = {};
uint64_t upscaler_source_writer_audit_capture_serial = 0u;
std::mutex downstream_draw_capture_mutex;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> downstream_capture_rtvs;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> downstream_capture_t0_views;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> gamma_audit_t0_views;
std::unordered_map<reshade::api::command_list*, reshade::api::resource_view> dlss_fg_compute_uav_views;
std::unordered_map<reshade::api::command_list*, std::vector<reshade::api::resource_view>>
    upscaler_source_compute_uav_views;
std::unordered_map<reshade::api::command_list*, std::vector<reshade::api::resource_view>>
    upscaler_source_compute_srv_views;
std::unordered_map<reshade::api::command_list*, reshade::api::buffer_range> upscaler_input_cb0_ranges;
std::unordered_map<uint64_t, UpscalerMappedBuffer> upscaler_mapped_buffers;
std::mutex upscaler_mapped_buffers_mutex;
std::mutex typeless_creation_audit_mutex;
std::unordered_map<uint64_t, int32_t> typeless_creation_indices;
uint32_t typeless_creation_width = 0u;
uint32_t typeless_creation_height = 0u;
int32_t typeless_creation_next_index = 0;

void OnTypelessAuditInitSwapchain(reshade::api::swapchain* swapchain, bool) {
  if (swapchain == nullptr) return;
  auto* device = swapchain->get_device();
  if (device == nullptr) return;
  // Streamline can load after init_device but before its first DLSS-G buffer
  // allocation. Retry here so the native-interface contract is active before
  // the first presentation rather than waiting for a downstream draw.
  TryInstallStreamlineHook();
  if (device->get_api() == reshade::api::device_api::d3d12) {
    dlss_fg_active_swapchain.store(reinterpret_cast<uintptr_t>(swapchain), std::memory_order_release);
  }
  const auto desc = device->get_resource_desc(swapchain->get_current_back_buffer());
  std::scoped_lock lock(typeless_creation_audit_mutex);
  typeless_creation_indices.clear();
  typeless_creation_width = desc.texture.width;
  typeless_creation_height = desc.texture.height;
  typeless_creation_next_index = 0;
}

void OnTypelessAuditInitResource(
    reshade::api::device*,
    const reshade::api::resource_desc& desc,
    const reshade::api::subresource_data*,
    reshade::api::resource_usage,
    reshade::api::resource resource) {
  if (resource.handle == 0u
      || desc.texture.format != reshade::api::format::r8g8b8a8_typeless
      || (desc.usage & reshade::api::resource_usage::render_target) == 0) {
    return;
  }
  std::scoped_lock lock(typeless_creation_audit_mutex);
  if (typeless_creation_width == 0u || typeless_creation_height == 0u
      || desc.texture.width != typeless_creation_width
      || desc.texture.height != typeless_creation_height) {
    return;
  }
  typeless_creation_indices[resource.handle] = typeless_creation_next_index++;
}

void OnTypelessAuditDestroyResource(reshade::api::device*, reshade::api::resource resource) {
  {
    std::scoped_lock lock(typeless_creation_audit_mutex);
    typeless_creation_indices.erase(resource.handle);
  }
  {
    std::scoped_lock lock(dlss_fg_bridge_mutex);
    dlss_fg_ad_effective_targets.erase(resource.handle);
    std::erase_if(dlss_fg_ad_effective_targets, [&](const auto& entry) {
      return entry.second == resource.handle;
    });
    dlss_fg_bridged_trays.erase(resource.handle);
    dlss_fg_ad_clone_states.erase(resource.handle);
    const auto prepared_iterator = dlss_fg_ad_prepared_sources.find(resource.handle);
    if (prepared_iterator != dlss_fg_ad_prepared_sources.end()) {
      dlss_fg_retired_prepared_sources.insert(
          dlss_fg_retired_prepared_sources.end(),
          prepared_iterator->second.begin(), prepared_iterator->second.end());
      dlss_fg_ad_prepared_sources.erase(prepared_iterator);
    }
  }
}

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
  result.view_format = device->get_resource_view_desc(view).format;
  result.effective_view_format = result.view_format;
  {
    std::scoped_lock lock(typeless_creation_audit_mutex);
    const auto index = typeless_creation_indices.find(resource.handle);
    if (index != typeless_creation_indices.end()) result.creation_index = index->second;
  }
  renodx::utils::resource::GetResourceInfo(resource, [&result](const renodx::utils::resource::ResourceInfo& info) {
    result.clone = info.clone.handle;
    result.clone_format = info.clone_desc.texture.format;
    result.clone_enabled = info.clone_enabled;
    if (info.clone_target != nullptr) result.upgrade_index = info.clone_target->index;
  });
  renodx::utils::resource::GetResourceViewInfo(view, [&result, device](const renodx::utils::resource::ResourceViewInfo& info) {
    result.view_clone_enabled = info.clone_enabled;
    if (!info.clone_enabled || info.clone.handle == 0u) return;
    const auto effective_resource = device->get_resource_from_view(info.clone);
    if (effective_resource.handle == 0u) return;
    const auto effective_desc = device->get_resource_desc(effective_resource);
    result.effective = effective_resource.handle;
    result.effective_format = effective_desc.texture.format;
    result.effective_view_format = device->get_resource_view_desc(info.clone).format;
  });
  return result;
}

constexpr bool IsDl2PopupUiShader(uint32_t hash) {
  return hash == 0x54F3F767u || hash == 0xF34DDC49u || hash == 0x43B22618u;
}

// Record the actual blend contract for the three popup/UI shaders identified
// from the first post-Gamma capture. This is read-only and runs only while
// their pipelines are created, so the eventual UI fix can preserve straight
// versus premultiplied alpha rather than guessing from shader output alone.
bool OnCreateDl2UiPipeline(
    reshade::api::device*,
    reshade::api::pipeline_layout,
    uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects) {
  uint32_t pixel_hash = 0u;
  const reshade::api::blend_desc* blend = nullptr;
  for (uint32_t index = 0u; index < subobject_count; ++index) {
    const auto& subobject = subobjects[index];
    if (subobject.type == reshade::api::pipeline_subobject_type::pixel_shader
        && subobject.count != 0u && subobject.data != nullptr) {
      const auto& desc = static_cast<const reshade::api::shader_desc*>(subobject.data)[0];
      if (desc.code != nullptr && desc.code_size != 0u) {
        pixel_hash = renodx::utils::hash::ComputeCRC32(
            static_cast<const uint8_t*>(desc.code), desc.code_size);
      }
    } else if (subobject.type == reshade::api::pipeline_subobject_type::blend_state
               && subobject.count != 0u && subobject.data != nullptr) {
      blend = &static_cast<const reshade::api::blend_desc*>(subobject.data)[0];
    }
  }
  if (!IsDl2PopupUiShader(pixel_hash)) return false;

  std::ostringstream stream;
  stream << "DL2 popup UI pipeline: ps=0x" << std::hex << std::uppercase << pixel_hash;
  if (blend == nullptr) {
    stream << " blend=unavailable";
  } else {
    stream << std::dec
           << " enabled=" << (blend->blend_enable[0] ? "yes" : "no")
           << " src_color=" << static_cast<uint32_t>(blend->source_color_blend_factor[0])
           << " dst_color=" << static_cast<uint32_t>(blend->dest_color_blend_factor[0])
           << " color_op=" << static_cast<uint32_t>(blend->color_blend_op[0])
           << " src_alpha=" << static_cast<uint32_t>(blend->source_alpha_blend_factor[0])
           << " dst_alpha=" << static_cast<uint32_t>(blend->dest_alpha_blend_factor[0])
           << " alpha_op=" << static_cast<uint32_t>(blend->alpha_blend_op[0])
           << " write_mask=0x" << std::hex << std::uppercase
           << static_cast<uint32_t>(blend->render_target_write_mask[0]);
  }
  renodx::utils::log::i(stream.str().c_str());
  return false;
}

struct DescriptorBindingAudit {
  uint32_t table = UINT_MAX;
  uint32_t binding = UINT_MAX;
  renodx::utils::descriptor::DescriptorHeapSlot slot = {};
  bool found = false;
};

bool FindGraphicsDescriptorBinding(
    reshade::api::device* device,
    const renodx::utils::state::CommandListState* command_state,
    uint32_t dx_register_index,
    reshade::api::descriptor_type expected_type,
    DescriptorBindingAudit* result) {
  if (device == nullptr || command_state == nullptr || result == nullptr) return false;
  auto* descriptor_data = renodx::utils::data::Get<renodx::utils::descriptor::DeviceData>(device);
  if (descriptor_data == nullptr) return false;

  bool found = false;
  renodx::utils::pipeline_layout::GetPipelineLayoutData(
      command_state->graphics_pipeline_layout,
      [&](const renodx::utils::pipeline_layout::PipelineLayoutData* layout_data) {
        for (uint32_t table_index = 0u;
             !found && table_index < layout_data->params.size()
             && table_index < command_state->graphics_descriptor_tables.size();
             ++table_index) {
          const auto& param = layout_data->params[table_index];
          if (param.type != reshade::api::pipeline_layout_param_type::descriptor_table) continue;
          const auto table = command_state->graphics_descriptor_tables[table_index];
          if (table.handle == 0u) continue;
          for (uint32_t range_index = 0u;
               !found && range_index < param.descriptor_table.count; ++range_index) {
            const auto& range = param.descriptor_table.ranges[range_index];
            if (range.type != expected_type || dx_register_index < range.dx_register_index) continue;
            const uint32_t register_offset = dx_register_index - range.dx_register_index;
            if (range.count != UINT_MAX && register_offset >= range.count) continue;
            const uint32_t binding = range.binding + register_offset;
            reshade::api::descriptor_heap heap = {};
            uint32_t heap_offset = 0u;
            device->get_descriptor_heap_offset(table, binding, 0u, &heap, &heap_offset);
            const std::shared_lock descriptor_lock(descriptor_data->mutex);
            const auto heap_it = descriptor_data->heaps.find(heap.handle);
            if (heap_it == descriptor_data->heaps.end() || heap_offset >= heap_it->second.size()) continue;
            const auto& slot = heap_it->second[heap_offset];
            if (slot.type != expected_type) continue;
            result->table = table_index;
            result->binding = binding;
            result->slot = slot;
            result->found = true;
            found = true;
          }
        }
      });
  return found;
}

UpscalerCurveAudit DescribeUpscalerCurve(
    reshade::api::device* device,
    const reshade::api::buffer_range& buffer_range) {
  UpscalerCurveAudit result = {
      .resource = buffer_range.buffer.handle,
      .offset = buffer_range.offset,
      .size = buffer_range.size,
  };
  if (device == nullptr || buffer_range.buffer.handle == 0u) return result;
  const auto cache = renodx::utils::constants::GetResourceCache(device, buffer_range.buffer);
  const auto capture_floats = [&result](const uint8_t* source, uint32_t count) {
    result.cache_available = true;
    result.cached_float_count = count;
    for (uint32_t index = 0u; index < count; ++index) {
      std::memcpy(&result.bits[index], source + index * sizeof(float), sizeof(uint32_t));
      std::memcpy(&result.values[index], source + index * sizeof(float), sizeof(float));
    }
  };
  if (buffer_range.offset < cache.size()) {
    const size_t available = std::min<size_t>(
        cache.size() - static_cast<size_t>(buffer_range.offset),
        buffer_range.size == UINT64_MAX ? kDL2TonemapperCurveFloatCount * sizeof(float) : static_cast<size_t>(buffer_range.size));
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(kDL2TonemapperCurveFloatCount, available / sizeof(float)));
    if (count != 0u) {
      capture_floats(cache.data() + buffer_range.offset, count);
      return result;
    }
  }
  // DL2 binds b0 from a persistently mapped D3D12 upload buffer. It is not
  // created with the ReShade constant-buffer usage bit, so use the CPU map
  // already exposed by the runtime instead of issuing a GPU readback.
  std::scoped_lock lock(upscaler_mapped_buffers_mutex);
  const auto mapped_it = upscaler_mapped_buffers.find(buffer_range.buffer.handle);
  if (mapped_it == upscaler_mapped_buffers.end()) return result;
  const auto& mapped = mapped_it->second;
  if (mapped.data == nullptr || buffer_range.offset < mapped.offset) return result;
  const uint64_t relative_offset = buffer_range.offset - mapped.offset;
  if (relative_offset > mapped.size || mapped.size - relative_offset < kDL2TonemapperCurveFloatCount * sizeof(float)) return result;
  capture_floats(mapped.data + relative_offset, kDL2TonemapperCurveFloatCount);
  return result;
}

void OnUpscalerMapBufferRegion(
    reshade::api::device* device,
    reshade::api::resource resource,
    uint64_t offset,
    uint64_t size,
    reshade::api::map_access,
    void** mapped_data) {
  if (device == nullptr || resource.handle == 0u || mapped_data == nullptr || *mapped_data == nullptr) return;
  const auto desc = device->get_resource_desc(resource);
  if (desc.type != reshade::api::resource_type::buffer) return;
  const uint64_t mapped_size = size == UINT64_MAX ? desc.buffer.size : size;
  if (mapped_size == 0u) return;
  std::scoped_lock lock(upscaler_mapped_buffers_mutex);
  upscaler_mapped_buffers[resource.handle] = {
      .data = static_cast<uint8_t*>(*mapped_data),
      .offset = offset,
      .size = mapped_size,
  };
}

void OnUpscalerUnmapBufferRegion(
    reshade::api::device*,
    reshade::api::resource resource) {
  std::scoped_lock lock(upscaler_mapped_buffers_mutex);
  upscaler_mapped_buffers.erase(resource.handle);
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
  const bool capture_upscaler_color_path = upscaler_color_path_audit_state.active;
  const bool capture_upscaler_inputs = upscaler_input_audit_state.active;
  const bool capture_upscaler_source_writers = upscaler_source_writer_audit_state.active;
  if ((!capture_gamma_input && !capture_downstream_inputs && !capture_fg_compute_writer
       && !capture_upscaler_color_path && !capture_upscaler_inputs
       && !capture_upscaler_source_writers)
      || update.count == 0u) {
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

  if (capture_upscaler_source_writers
      && renodx::utils::bitwise::HasFlag(stages, reshade::api::shader_stage::compute)
      && (update.type == reshade::api::descriptor_type::texture_unordered_access_view
          || update.type == reshade::api::descriptor_type::buffer_unordered_access_view)) {
    std::scoped_lock lock(downstream_draw_capture_mutex);
    if (upscaler_source_writer_audit_state.active) {
      auto& views = upscaler_source_compute_uav_views[cmd_list];
      for (uint32_t index = 0u; index < update.count && views.size() < 64u; ++index) {
        const auto view = renodx::utils::descriptor::GetResourceViewFromDescriptorUpdate(update, index);
        if (view.handle == 0u) continue;
        const bool known = std::any_of(views.begin(), views.end(), [&](const auto candidate) {
          return candidate.handle == view.handle;
        });
        if (!known) views.push_back(view);
        if (upscaler_source_writer_audit_state.compute_candidate_count
            < upscaler_source_writer_audit_state.compute_candidates.size()) {
          auto* device = cmd_list->get_device();
          const auto candidate = DescribeGammaAuditView(device, view);
          const bool known_candidate = std::any_of(
              upscaler_source_writer_audit_state.compute_candidates.begin(),
              upscaler_source_writer_audit_state.compute_candidates.begin()
                  + upscaler_source_writer_audit_state.compute_candidate_count,
              [&](const auto& existing) {
                return existing.resource == candidate.resource && existing.view_format == candidate.view_format;
              });
          if (!known_candidate && candidate.resource != 0u) {
            upscaler_source_writer_audit_state.compute_candidates[upscaler_source_writer_audit_state.compute_candidate_count++] = candidate;
          }
        }
      }
    }
  }

  if (capture_upscaler_source_writers
      && renodx::utils::bitwise::HasFlag(stages, reshade::api::shader_stage::compute)
      && (update.type == reshade::api::descriptor_type::sampler_with_resource_view
          || update.type == reshade::api::descriptor_type::shader_resource_view
          || update.type == reshade::api::descriptor_type::buffer_shader_resource_view)) {
    std::scoped_lock lock(downstream_draw_capture_mutex);
    auto& views = upscaler_source_compute_srv_views[cmd_list];
    for (uint32_t index = 0u; index < update.count && views.size() < 32u; ++index) {
      const auto view = renodx::utils::descriptor::GetResourceViewFromDescriptorUpdate(update, index);
      if (view.handle == 0u) continue;
      const bool known = std::any_of(views.begin(), views.end(), [&](const auto candidate) {
        return candidate.handle == view.handle;
      });
      if (!known) views.push_back(view);
    }
  }

  if (capture_upscaler_inputs
      && renodx::utils::bitwise::HasFlag(stages, reshade::api::shader_stage::pixel)
      && update.type == reshade::api::descriptor_type::constant_buffer) {
    uint32_t register_index = 0u;
    bool found_register_index = false;
    renodx::utils::pipeline_layout::GetPipelineLayoutData(layout, [&](const auto* layout_data) {
      if (layout_param >= layout_data->params.size()) return;
      const auto& param = layout_data->params[layout_param];
      if (param.type == reshade::api::pipeline_layout_param_type::push_descriptors) {
        register_index = param.push_descriptors.dx_register_index;
        found_register_index = true;
      }
    });
    if (found_register_index) {
      const auto* ranges = static_cast<const reshade::api::buffer_range*>(update.descriptors);
      for (uint32_t index = 0u; index < update.count; ++index) {
        if (register_index + update.binding + index != 0u) continue;
        std::scoped_lock lock(downstream_draw_capture_mutex);
        upscaler_input_cb0_ranges[cmd_list] = ranges[index];
        break;
      }
    }
  }

  if ((!capture_gamma_input && !capture_downstream_inputs && !capture_upscaler_color_path
       && !capture_upscaler_source_writers)
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
    if (capture_downstream_inputs || capture_upscaler_color_path || capture_upscaler_source_writers) {
      const auto existing = downstream_capture_t0_views.find(cmd_list);
      if (existing != downstream_capture_t0_views.end()
          || downstream_capture_t0_views.size() < kDownstreamInputViewLimit) {
        downstream_capture_t0_views[cmd_list] = view;
      }
    }
    return;
  }
}

void MarkDlssFgCommandListSwapchainWrite(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource resource,
    bool bound_rtv,
    bool copy_dest,
    bool preserved_native_copy = false) {
  if (!dlss_fg_mode_active.load(std::memory_order_acquire)
      || cmd_list == nullptr || resource.handle == 0u) {
    return;
  }
  bool is_backbuffer = false;
  renodx::utils::resource::GetResourceInfo(resource, [&](const renodx::utils::resource::ResourceInfo& info) {
    is_backbuffer = info.is_swap_chain;
  });
  if (!is_backbuffer) return;

  std::scoped_lock lock(dlss_fg_command_list_candidate_mutex);
  auto& candidate = dlss_fg_command_list_candidates[reinterpret_cast<uintptr_t>(cmd_list)];
  candidate.swapchain_generation = dlss_fg_swapchain_generation.load(std::memory_order_acquire);
  candidate.back_buffer = resource.handle;
  if (copy_dest) candidate.copy_source = source.handle;
  candidate.bound_swapchain_rtv |= bound_rtv;
  candidate.copied_to_swapchain |= copy_dest;
  candidate.preserved_native_copy |= preserved_native_copy;
}

void OnDownstreamBindRenderTargets(
    reshade::api::command_list* cmd_list,
    uint32_t count,
    const reshade::api::resource_view* rtvs,
    reshade::api::resource_view) {
  if (count != 0u && rtvs != nullptr && cmd_list != nullptr) {
    auto* device = cmd_list->get_device();
    if (device != nullptr) {
      MarkDlssFgCommandListSwapchainWrite(
          cmd_list,
          {},
          device->get_resource_from_view(rtvs[0]),
          true,
          false);
    }
  }
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
                                   || upscaler_color_path_audit_state.active
                                   || upscaler_source_writer_audit_state.active
                                   || (downstream_draw_capture_state.active && !downstream_draw_capture_state.consumed);
  if (!keep_target_binding || count == 0u || rtvs == nullptr) {
    downstream_capture_rtvs.erase(cmd_list);
    return;
  }
  downstream_capture_rtvs[cmd_list] = rtvs[0];
}

void MarkDlssFgAdCommandList(
    reshade::api::command_list* cmd_list,
    uint32_t draw_count,
    uint32_t instance_count) {
  if (!dlss_fg_mode_active.load(std::memory_order_relaxed)
      || cmd_list == nullptr
      || draw_count < 3u || draw_count > 6u
      || instance_count < 1u || instance_count > 4u) {
    return;
  }

  GammaAuditResource output = {};
  if (const auto* command_state = renodx::utils::state::GetCurrentState(cmd_list);
      command_state != nullptr && !command_state->render_targets.empty()) {
    output = DescribeGammaAuditView(cmd_list->get_device(), command_state->render_targets[0]);
  }

  std::scoped_lock lock(downstream_draw_capture_mutex);
  dlss_fg_ad_command_list_markers[reinterpret_cast<uintptr_t>(cmd_list)] = {
      .epoch = upscaler_color_command_epochs[reinterpret_cast<uintptr_t>(cmd_list)],
      .target = output.resource,
      .effective_target = output.effective,
  };
  if (output.resource != 0u && output.effective != 0u && output.resource != output.effective) {
    std::scoped_lock bridge_lock(dlss_fg_bridge_mutex);
    dlss_fg_ad_effective_targets[output.resource] = output.effective;
    // This callback is associated with the 0xAD draw that writes the target.
    // The clone's actual rest state for the later copy is therefore RTV.
    dlss_fg_ad_clone_states[output.effective] = reshade::api::resource_usage::render_target;
  }
}

bool PrepareDlssFgComputeSource(reshade::api::command_list* cmd_list) {
  const int32_t final_color_mode = std::clamp(
      static_cast<int32_t>(dlss_fg_final_color_mode + 0.5f), 0, 11);
  if (final_color_mode != 11
      || !dlss_fg_mode_active.load(std::memory_order_acquire)
      || swap_chain_use_hdr10 < 0.5f
      || cmd_list == nullptr || cmd_list->get_device() == nullptr) {
    return false;
  }
  auto* device = cmd_list->get_device();
  if (device->get_api() != reshade::api::device_api::d3d12) return false;

  GammaAuditResource output = {};
  if (const auto* command_state = renodx::utils::state::GetCurrentState(cmd_list);
      command_state != nullptr && !command_state->render_targets.empty()) {
    output = DescribeGammaAuditView(device, command_state->render_targets[0]);
  }
  if (output.resource == 0u || output.effective == 0u || output.resource == output.effective) {
    return false;
  }

  const auto effective_source = reshade::api::resource{output.effective};
  const auto source_desc = device->get_resource_desc(effective_source);
  if (source_desc.type != reshade::api::resource_type::texture_2d
      || source_desc.texture.format != reshade::api::format::r16g16b16a16_float) {
    return false;
  }

  // Streamline can queue several producer frames before its matching Compute
  // copy is submitted. Keep enough immutable trays to absorb that latency;
  // falling back to the native tray while a replacement is pending causes a
  // visible frame-to-frame color/brightness toggle.
  constexpr uint32_t kMaxPreparedSlotsPerSource = 8u;
  const uint64_t handoff_epoch = dlss_fg_handoff_epoch.load(std::memory_order_acquire);
  std::shared_ptr<DlssFgBridgePass> prepared;
  uint64_t record_serial = 0u;
  for (uint32_t attempt = 0u;
       attempt <= kMaxPreparedSlotsPerSource && prepared == nullptr;
       ++attempt) {
    std::vector<std::shared_ptr<DlssFgBridgePass>> slots;
    {
      std::scoped_lock lock(dlss_fg_bridge_mutex);
      const auto iterator = dlss_fg_ad_prepared_sources.find(output.effective);
      if (iterator != dlss_fg_ad_prepared_sources.end()) slots = iterator->second;
    }
    {
      std::scoped_lock lock(dlss_fg_prepared_command_mutex);
      for (const auto& slot : slots) {
        const uint64_t consumer_completed = slot != nullptr && slot->consumer_fence != nullptr
                                                ? slot->consumer_fence->GetCompletedValue()
                                                : 0u;
        if (slot != nullptr && slot->handoff_epoch != handoff_epoch
            && !slot->producer_pending && !slot->consumer_assigned) {
          slot->consumer_reserved = false;
        }
        if (slot == nullptr || slot->producer_pending || slot->consumer_reserved
            || (slot->consumer_fence_value != 0u
                && consumer_completed < slot->consumer_fence_value)) {
          continue;
        }
        prepared = slot;
        prepared->producer_pending = true;
        prepared->consumer_reserved = true;
        prepared->consumer_assigned = false;
        prepared->handoff_epoch = handoff_epoch;
        prepared->producer_fence_value = 0u;
        record_serial = ++prepared->producer_serial;
        dlss_fg_prepared_resource_record_serials[prepared->resource.handle] = record_serial;
        break;
      }
    }
    if (prepared != nullptr) break;

    std::scoped_lock lock(dlss_fg_bridge_mutex);
    auto& slots_for_source = dlss_fg_ad_prepared_sources[output.effective];
    if (slots_for_source.size() >= kMaxPreparedSlotsPerSource) break;
    auto prepared_desc = source_desc;
    prepared_desc.texture.format = reshade::api::format::r10g10b10a2_unorm;
    prepared_desc.heap = reshade::api::memory_heap::gpu_only;
    prepared_desc.usage = reshade::api::resource_usage::unordered_access
                          | reshade::api::resource_usage::copy_source;
    prepared_desc.flags = reshade::api::resource_flags::none;
    auto created = std::make_shared<DlssFgBridgePass>();
    created->slot = static_cast<uint32_t>(slots_for_source.size());
    if (!device->create_resource(
            prepared_desc, nullptr, reshade::api::resource_usage::copy_source,
            &created->resource)) {
      renodx::utils::log::i("DL2 DLSS FG Direct prepare: RGB10 create failed");
      return false;
    }
    auto* native_device = reinterpret_cast<ID3D12Device*>(
        static_cast<uintptr_t>(device->get_native()));
    const HRESULT producer_fence_hr = native_device != nullptr
                                          ? native_device->CreateFence(
                                                0u,
                                                D3D12_FENCE_FLAG_NONE,
                                                IID_PPV_ARGS(&created->producer_fence))
                                          : E_POINTER;
    const HRESULT consumer_fence_hr = native_device != nullptr
                                          ? native_device->CreateFence(
                                                0u,
                                                D3D12_FENCE_FLAG_NONE,
                                                IID_PPV_ARGS(&created->consumer_fence))
                                          : E_POINTER;
    if (FAILED(producer_fence_hr) || FAILED(consumer_fence_hr)) {
      device->destroy_resource(created->resource);
      std::ostringstream message;
      message << "DL2 DLSS FG Direct prepare: slot fence create failed producer_hr=0x"
              << std::hex << static_cast<uint32_t>(producer_fence_hr)
              << " consumer_hr=0x" << static_cast<uint32_t>(consumer_fence_hr);
      renodx::utils::log::w(message.str().c_str());
      return false;
    }
    created->pass = std::make_unique<renodx::utils::render::RenderPass>();
    created->pass->shader_resource_slots.resources = {effective_source};
    created->pass->unordered_access_slots.resources = {created->resource};
    created->pass->pipeline_subobjects.compute_shader = __dlss_fg_bridge_compute_shader_dx12;
    created->pass->dispatch_group_counts = {
        (source_desc.texture.width + 7u) / 8u,
        (source_desc.texture.height + 7u) / 8u,
        1u};
    created->pass->revert_state_after_render = true;
    created->pass->use_render_pass = false;
    created->pass->push_constants[{
        .slot = 13,
        .space = 50,
    }] = std::span<const float>(reinterpret_cast<const float*>(&shader_injection),
                                sizeof(shader_injection) / sizeof(float));
    {
      std::scoped_lock prepared_lock(dlss_fg_prepared_command_mutex);
      dlss_fg_prepared_slots[created->resource.handle] = created;
    }
    slots_for_source.push_back(std::move(created));
  }
  if (prepared == nullptr || prepared->resource.handle == 0u) {
    uint64_t highest_consumer_completed = 0u;
    {
      std::scoped_lock lock(dlss_fg_bridge_mutex);
      const auto iterator = dlss_fg_ad_prepared_sources.find(output.effective);
      if (iterator != dlss_fg_ad_prepared_sources.end()) {
        for (const auto& slot : iterator->second) {
          if (slot != nullptr && slot->consumer_fence != nullptr) {
            highest_consumer_completed =
                std::max(highest_consumer_completed, slot->consumer_fence->GetCompletedValue());
          }
        }
      }
    }
    const uint32_t diagnostic =
        dlss_fg_bridge_candidate_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
    if (diagnostic < 32u) {
      std::ostringstream message;
      message << "DL2 DLSS FG Direct prepare: source=0x" << std::hex << output.effective
              << std::dec << " pool_exhausted=1 highest_consumer_completed="
              << highest_consumer_completed;
      renodx::utils::log::w(message.str().c_str());
    }
    return false;
  }

  bool rendered = false;
  {
    std::scoped_lock recording_lock(prepared->recording_mutex);
    cmd_list->barrier(
        effective_source,
        reshade::api::resource_usage::render_target,
        reshade::api::resource_usage::shader_resource);
    cmd_list->barrier(
        prepared->resource,
        reshade::api::resource_usage::copy_source,
        reshade::api::resource_usage::unordered_access);
    rendered = prepared->pass->Render(cmd_list);
    cmd_list->barrier(
        prepared->resource,
        reshade::api::resource_usage::unordered_access,
        reshade::api::resource_usage::copy_source);
    cmd_list->barrier(
        effective_source,
        reshade::api::resource_usage::shader_resource,
        reshade::api::resource_usage::render_target);
  }
  if (rendered) {
    std::scoped_lock lock(dlss_fg_prepared_command_mutex);
    const auto command_handle = reinterpret_cast<uintptr_t>(cmd_list->get_native());
    dlss_fg_prepared_command_resources[command_handle][prepared->resource.handle] = record_serial;
  } else {
    std::scoped_lock lock(dlss_fg_prepared_command_mutex);
    if (prepared->producer_serial == record_serial) {
      prepared->producer_pending = false;
      prepared->consumer_reserved = false;
      prepared->consumer_assigned = false;
    }
  }
  const uint32_t diagnostic = dlss_fg_bridge_candidate_diagnostic_count.fetch_add(1u);
  if (diagnostic < 16u || !rendered) {
    std::ostringstream message;
    message << "DL2 DLSS FG Direct prepare: original=0x" << std::hex << std::uppercase
            << output.resource << " source=0x" << output.effective
            << " prepared=0x" << prepared->resource.handle << std::dec
            << " slot=" << prepared->slot
            << " producer_serial=" << record_serial
            << " rendered=" << (rendered ? 1 : 0)
            << " consumer_completed=" << prepared->consumer_fence->GetCompletedValue()
            << " state=copy_source contract=pq_bt2100";
    renodx::utils::log::i(message.str().c_str());
  }
  return rendered;
}

void DlssFgAdPostDraw(renodx::utils::command_action::CommandContext<
                          renodx::utils::command_action::DrawArguments>& context,
                      const void*) {
  PrepareDlssFgComputeSource(context.cmd_list);
}

void DlssFgAdPostDrawIndexed(renodx::utils::command_action::CommandContext<
                                 renodx::utils::command_action::DrawIndexedArguments>& context,
                             const void*) {
  PrepareDlssFgComputeSource(context.cmd_list);
}

inline constexpr auto OnGammaDrawAudit = []<typename Context>(Context& context)
    -> renodx::utils::command_action::CallbackResult<Context> {
  if (context.IsDispatch()) return {};

  uint32_t draw_count = 0u;
  uint32_t instance_count = 0u;
  if constexpr (requires { context.arguments.vertex_count; context.arguments.instance_count; }) {
    draw_count = context.arguments.vertex_count;
    instance_count = context.arguments.instance_count;
  } else if constexpr (requires { context.arguments.index_count; context.arguments.instance_count; }) {
    draw_count = context.arguments.index_count;
    instance_count = context.arguments.instance_count;
  }
  MarkDlssFgAdCommandList(context.cmd_list, draw_count, instance_count);

  auto* shader_state = renodx::utils::command_action::GetShaderState(&context);
  const uint32_t shader_hash = shader_state != nullptr
                                   ? renodx::utils::shader::GetCurrentShaderHash(
                                         shader_state, renodx::utils::shader::PIXEL_INDEX)
                                   : 0u;
  if (shader_hash == 0xAD085E81u) {
    if constexpr (std::is_same_v<Context, renodx::utils::command_action::CommandContext<
                                              renodx::utils::command_action::DrawArguments>>) {
      return {.post_callback = DlssFgAdPostDraw, .replay = true};
    } else if constexpr (std::is_same_v<Context, renodx::utils::command_action::CommandContext<
                                                     renodx::utils::command_action::DrawIndexedArguments>>) {
      return {.post_callback = DlssFgAdPostDrawIndexed, .replay = true};
    }
  }

  if (!gamma_draw_audit_capture && !gamma_native_input_audit_capture) return {};

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
  auto& upscaler_audit = upscaler_color_path_audit_state;
  const bool capture_upscaler_color_path = upscaler_audit.active;
  const bool capture_upscaler_inputs = upscaler_input_audit_state.active;
  const bool capture_upscaler_source_writers = upscaler_source_writer_audit_state.active;
  const bool capture_exact_ad_ordering =
      dlss_fg_exact_ad_ordering_remaining.load(std::memory_order_relaxed) != 0u;
  if (!capture_commands && !capture_transfers && !capture_fg_producer && !capture_fg_compute_writer
      && !capture_upscaler_color_path && !capture_upscaler_inputs && !capture_upscaler_source_writers
      && !capture_exact_ad_ordering) {
    capture = {};
    downstream_capture_t0_views.clear();
    return {};
  }
  if (capture.consumed && !capture_fg_producer && !capture_fg_compute_writer
      && !capture_upscaler_color_path && !capture_upscaler_inputs && !capture_upscaler_source_writers
      && !capture_exact_ad_ordering) return {};

  auto* shader_state = renodx::utils::command_action::GetShaderState(&context);
  if (shader_state == nullptr) return {};
  const bool is_compute = context.IsDispatch();
  const uint32_t shader_hash = renodx::utils::shader::GetCurrentShaderHash(
      shader_state, is_compute ? renodx::utils::shader::COMPUTE_INDEX
                               : renodx::utils::shader::PIXEL_INDEX);

  uint32_t draw_count = 0u;
  uint32_t instance_count = 0u;
  if constexpr (requires { context.arguments.vertex_count; context.arguments.instance_count; }) {
    draw_count = context.arguments.vertex_count;
    instance_count = context.arguments.instance_count;
  } else if constexpr (requires { context.arguments.index_count; context.arguments.instance_count; }) {
    draw_count = context.arguments.index_count;
    instance_count = context.arguments.instance_count;
  }
  const bool likely_fullscreen_draw = draw_count >= 3u && draw_count <= 6u
                                      && instance_count >= 1u && instance_count <= 4u;
  const bool targeted_color_shader = shader_hash == 0x3E36DA5Bu
                                     || shader_hash == 0x268BAB6Du
                                     || shader_hash == 0xAD085E81u;

  const bool capture_tonemapper_inputs = upscaler_input_audit_state.active
                                         || (upscaler_source_writer_audit_state.active
                                             && !upscaler_source_writer_audit_state.target_final_fg_output);
  if (capture_tonemapper_inputs && !is_compute && shader_hash == 0x3E36DA5Bu
      && likely_fullscreen_draw
      && (upscaler_input_audit_state.count < 4u || upscaler_source_writer_audit_state.active)) {
    auto* device = context.cmd_list->get_device();
    const auto* command_state = renodx::utils::state::GetCurrentState(context.cmd_list);
    DescriptorBindingAudit t0_binding = {};
    DescriptorBindingAudit t1_binding = {};
    DescriptorBindingAudit cb0_binding = {};
    FindGraphicsDescriptorBinding(
        device, command_state, 0u,
        reshade::api::descriptor_type::texture_shader_resource_view, &t0_binding);
    FindGraphicsDescriptorBinding(
        device, command_state, 1u,
        reshade::api::descriptor_type::texture_shader_resource_view, &t1_binding);
    FindGraphicsDescriptorBinding(
        device, command_state, 0u,
        reshade::api::descriptor_type::constant_buffer, &cb0_binding);

    auto source = t0_binding.found
                      ? DescribeGammaAuditView(device, t0_binding.slot.resource_view)
                      : GammaAuditResource{};
    // D3D11 may not expose the layout metadata, but its push-descriptor event
    // has already captured t0 by the time this draw callback executes.
    if (source.resource == 0u) {
      const auto input_it = downstream_capture_t0_views.find(context.cmd_list);
      if (input_it != downstream_capture_t0_views.end()) {
        source = DescribeGammaAuditView(device, input_it->second);
      }
    }
    const auto exposure = t1_binding.found
                              ? DescribeGammaAuditView(device, t1_binding.slot.resource_view)
                              : GammaAuditResource{};
    auto& source_writer_audit = upscaler_source_writer_audit_state;
    if (source_writer_audit.active && source.resource != 0u) {
      if (source_writer_audit.source == 0u) {
        source_writer_audit.source = source.resource;
        source_writer_audit.effective_source = source.effective;
        source_writer_audit.source_info = source;
      }
      const bool known_source = std::any_of(
          source_writer_audit.recent_sources.begin(),
          source_writer_audit.recent_sources.begin() + source_writer_audit.recent_source_count,
          [&](uint64_t value) { return value == source.resource; });
      if (!known_source && source_writer_audit.recent_source_count < source_writer_audit.recent_sources.size()) {
        const uint32_t index = source_writer_audit.recent_source_count++;
        source_writer_audit.recent_sources[index] = source.resource;
        source_writer_audit.recent_effective_sources[index] = source.effective;
      }
      source_writer_audit.exposure = exposure.resource;
      source_writer_audit.effective_exposure = exposure.effective;
    }
    auto curve = cb0_binding.found
                     ? DescribeUpscalerCurve(device, cb0_binding.slot.buffer_range)
                     : UpscalerCurveAudit{};
    if (curve.resource == 0u) {
      const auto curve_it = upscaler_input_cb0_ranges.find(context.cmd_list);
      if (curve_it != upscaler_input_cb0_ranges.end()) {
        curve = DescribeUpscalerCurve(device, curve_it->second);
      }
    }
    if (upscaler_input_audit_state.active && upscaler_input_audit_state.count < 4u) {
      const uint64_t generation = dlss_fg_swapchain_generation.load(std::memory_order_acquire);
      upscaler_input_audit_state.entries[upscaler_input_audit_state.count++] = {
          .generation = generation,
          .api = static_cast<uint32_t>(device->get_api()),
          .draw_count = draw_count,
          .instance_count = instance_count,
          .present_index = upscaler_input_audit_state.presents + 1u,
          .t0_table = t0_binding.table,
          .t0_binding = t0_binding.binding,
          .t1_table = t1_binding.table,
          .t1_binding = t1_binding.binding,
          .cb0_table = cb0_binding.table,
          .cb0_binding = cb0_binding.binding,
          .source = source,
          .exposure = exposure,
          .curve = curve,
      };
    }
  }

  if (upscaler_source_writer_audit_state.active && !is_compute
      && upscaler_source_writer_audit_state.source != 0u
      && upscaler_source_writer_audit_state.count < upscaler_source_writer_audit_state.writers.size()) {
    std::vector<reshade::api::resource_view> writer_targets;
    if (const auto target_it = downstream_capture_rtvs.find(context.cmd_list);
        target_it != downstream_capture_rtvs.end()) {
      writer_targets.push_back(target_it->second);
    } else if (const auto* command_state = renodx::utils::state::GetCurrentState(context.cmd_list);
               command_state != nullptr) {
      writer_targets = command_state->render_targets;
    }
    for (const auto target_view : writer_targets) {
      const auto output = DescribeGammaAuditView(context.cmd_list->get_device(), target_view);
      auto& audit = upscaler_source_writer_audit_state;
      const bool matched = std::any_of(
          audit.recent_sources.begin(), audit.recent_sources.begin() + audit.recent_source_count,
          [&](uint64_t value) { return output.resource == value || output.effective == value; });
      if (matched) {
        const bool known = std::any_of(
            audit.writers.begin(), audit.writers.begin() + audit.count,
            [&](const auto& writer) {
              return writer.type == UpscalerSourceWriterType::draw
                     && writer.shader_hash == shader_hash && writer.target == output.resource;
            });
        if (!known) {
          uint64_t writer_source = 0u;
          const auto input_it = downstream_capture_t0_views.find(context.cmd_list);
          if (input_it != downstream_capture_t0_views.end()) {
            writer_source = DescribeGammaAuditView(
                                context.cmd_list->get_device(), input_it->second)
                                .resource;
          }
          audit.writers[audit.count++] = {
              .type = UpscalerSourceWriterType::draw,
              .shader_hash = shader_hash,
              .present_index = audit.presents + 1u,
              .command_buffer = reinterpret_cast<uint64_t>(context.cmd_list->get_native()),
              .source = writer_source,
              .target = output.resource,
          };
        }
      }
    }
  }

  if (capture_upscaler_source_writers && is_compute && shader_hash != 0u
      && upscaler_source_writer_audit_state.source != 0u) {
    const auto views_it = upscaler_source_compute_uav_views.find(context.cmd_list);
    if (views_it != upscaler_source_compute_uav_views.end()) {
      auto& audit = upscaler_source_writer_audit_state;
      auto* device = context.cmd_list->get_device();
      for (const auto view : views_it->second) {
        const auto output = DescribeGammaAuditView(device, view);
        const bool matched = std::any_of(
            audit.recent_sources.begin(), audit.recent_sources.begin() + audit.recent_source_count,
            [&](uint64_t value) { return output.resource == value || output.effective == value; });
        if (!matched) continue;
        const bool known = std::any_of(
            audit.writers.begin(), audit.writers.begin() + audit.count,
            [&](const auto& writer) {
              return writer.type == UpscalerSourceWriterType::dispatch
                     && writer.shader_hash == shader_hash && writer.target == output.resource;
            });
        if (!known && audit.count < audit.writers.size()) {
          uint64_t writer_source = 0u;
          const auto inputs_it = upscaler_source_compute_srv_views.find(context.cmd_list);
          if (inputs_it != upscaler_source_compute_srv_views.end() && !inputs_it->second.empty()) {
            writer_source = DescribeGammaAuditView(device, inputs_it->second.front()).resource;
          }
          audit.writers[audit.count++] = {
              .type = UpscalerSourceWriterType::dispatch,
              .shader_hash = shader_hash,
              .present_index = audit.presents + 1u,
              .command_buffer = reinterpret_cast<uint64_t>(context.cmd_list->get_native()),
              .source = writer_source,
              .target = output.resource,
          };
        }
      }
      views_it->second.clear();
      upscaler_source_compute_srv_views[context.cmd_list].clear();
    }
  }

  if (capture_upscaler_color_path && !is_compute && targeted_color_shader && likely_fullscreen_draw) {
    const uint32_t sequence = ++upscaler_audit.sequence;
    const auto target = downstream_capture_rtvs.find(context.cmd_list);
    if (target != downstream_capture_rtvs.end()) {
      auto* device = context.cmd_list->get_device();
      const auto output = DescribeGammaAuditView(device, target->second);
      if (output.width >= 128u && output.height >= 128u) {
        const auto input_it = downstream_capture_t0_views.find(context.cmd_list);
        auto input = input_it != downstream_capture_t0_views.end()
                         ? DescribeGammaAuditView(device, input_it->second)
                         : GammaAuditResource{};
        uint32_t input_table = UINT_MAX;
        uint32_t input_binding = UINT_MAX;
        uint32_t viewport_width = 0u;
        uint32_t viewport_height = 0u;
        const auto* command_state = renodx::utils::state::GetCurrentState(context.cmd_list);
        if (command_state != nullptr) {
          if (!command_state->viewports.empty()) {
            viewport_width = static_cast<uint32_t>(command_state->viewports[0].width);
            viewport_height = static_cast<uint32_t>(command_state->viewports[0].height);
          }
          uint64_t best_area = static_cast<uint64_t>(input.width) * input.height;
          auto* descriptor_data = renodx::utils::data::Get<renodx::utils::descriptor::DeviceData>(device);
          std::vector<std::pair<uint32_t, uint32_t>> descriptor_locations;
          renodx::utils::pipeline_layout::GetPipelineLayoutData(
              command_state->graphics_pipeline_layout,
              [&](const renodx::utils::pipeline_layout::PipelineLayoutData* layout_data) {
                for (uint32_t table_index = 0u;
                     table_index < layout_data->params.size()
                     && table_index < command_state->graphics_descriptor_tables.size();
                     ++table_index) {
                  const auto& param = layout_data->params[table_index];
                  if (param.type != reshade::api::pipeline_layout_param_type::descriptor_table) continue;
                  for (uint32_t range_index = 0u; range_index < param.descriptor_table.count; ++range_index) {
                    const auto& range = param.descriptor_table.ranges[range_index];
                    if (range.type != reshade::api::descriptor_type::sampler_with_resource_view
                        && range.type != reshade::api::descriptor_type::texture_shader_resource_view
                        && range.type != reshade::api::descriptor_type::buffer_shader_resource_view) {
                      continue;
                    }
                    const uint32_t count = std::min(range.count, 64u);
                    for (uint32_t offset = 0u; offset < count; ++offset) {
                      descriptor_locations.emplace_back(table_index, range.binding + offset);
                    }
                  }
                }
              });
          for (const auto& [table_index, binding] : descriptor_locations) {
            const auto table = command_state->graphics_descriptor_tables[table_index];
            if (table.handle == 0u) continue;
            renodx::utils::descriptor::DescriptorHeapSlot slot = {};
            uint32_t heap_offset = 0u;
            reshade::api::descriptor_heap heap = {};
            device->get_descriptor_heap_offset(table, binding, 0u, &heap, &heap_offset);
            bool found_slot = false;
            if (descriptor_data != nullptr) {
              const std::shared_lock descriptor_lock(descriptor_data->mutex);
              const auto heap_it = descriptor_data->heaps.find(heap.handle);
              if (heap_it != descriptor_data->heaps.end() && heap_offset < heap_it->second.size()) {
                slot = heap_it->second[heap_offset];
                found_slot = true;
              }
            }
            if (!found_slot || !slot.HasResourceView() || slot.resource_view.handle == 0u) continue;
            const auto candidate_input = DescribeGammaAuditView(device, slot.resource_view);
            if (candidate_input.resource == 0u || candidate_input.resource == output.resource) continue;
            if (candidate_input.width < output.width / 2u || candidate_input.height < output.height / 2u
                || candidate_input.width == 0u || candidate_input.height == 0u
                || output.width == 0u || output.height == 0u) continue;
            const float output_aspect = static_cast<float>(output.width) / output.height;
            const float input_aspect = static_cast<float>(candidate_input.width) / candidate_input.height;
            if (std::abs(input_aspect - output_aspect) > output_aspect * 0.12f) continue;
            const uint64_t area = static_cast<uint64_t>(candidate_input.width) * candidate_input.height;
            if (area <= best_area || candidate_input.width < 128u || candidate_input.height < 128u) continue;
            input = candidate_input;
            best_area = area;
            input_table = table_index;
            input_binding = binding;
          }
        }
        const uint64_t generation = dlss_fg_swapchain_generation.load(std::memory_order_acquire);
        uint32_t input_writer_hash = 0u;
        std::array<uint32_t, 16> input_writer_hashes = {};
        uint32_t input_writer_count = 0u;
        const auto writer_key = UpscalerColorWriterKey{
            .resource = input.effective != 0u ? input.effective : input.resource,
            .command_list = reinterpret_cast<uintptr_t>(context.cmd_list),
            .epoch = upscaler_color_command_epochs[reinterpret_cast<uintptr_t>(context.cmd_list)],
        };
        const auto find_input_writer = [&](uint64_t resource) {
          auto key = writer_key;
          key.resource = resource;
          const auto writer = upscaler_color_last_writers.find(key);
          if (writer != upscaler_color_last_writers.end()) input_writer_hash = writer->second;
          const auto chain = upscaler_color_writer_chains.find(key);
          if (chain == upscaler_color_writer_chains.end()) return;
          input_writer_count = static_cast<uint32_t>(std::min(chain->second.size(), input_writer_hashes.size()));
          std::copy_n(chain->second.begin(), input_writer_count, input_writer_hashes.begin());
        };
        if (input.effective != 0u) find_input_writer(input.effective);
        if (input_writer_hash == 0u && input.resource != 0u) find_input_writer(input.resource);
        if (upscaler_audit.count < upscaler_audit.entries.size()) {
          uint64_t bound_pipeline = 0u;
          uint64_t replacement_pipeline = 0u;
          bool replacement_bound = false;
          if (auto* shader_state = renodx::utils::shader::GetCurrentState(context.cmd_list);
              shader_state != nullptr) {
            auto* pixel_state = renodx::utils::shader::GetCurrentPixelState(shader_state);
            renodx::utils::shader::PopulateStageState(pixel_state);
            if (pixel_state->pipeline_details != nullptr) {
              bound_pipeline = pixel_state->pipeline.handle;
              replacement_pipeline = pixel_state->pipeline_details->replacement_pipeline.handle;
              replacement_bound = pixel_state->pipeline_details->is_replacement
                                  || (replacement_pipeline != 0u && bound_pipeline == replacement_pipeline);
            }
          }
          upscaler_audit.entries[upscaler_audit.count++] = {
              .shader_hash = shader_hash,
              .input_writer_hash = input_writer_hash,
              .input_writer_hashes = input_writer_hashes,
              .input_writer_count = input_writer_count,
              .api = static_cast<uint32_t>(device->get_api()),
              .generation = generation,
              .draw_count = draw_count,
              .instance_count = instance_count,
              .input_table = input_table,
              .input_binding = input_binding,
              .viewport_width = viewport_width,
              .viewport_height = viewport_height,
              .sequence = sequence,
              .present_index = upscaler_audit.presents + 1u,
              .input = input,
              .output = output,
              .command_list = reinterpret_cast<uintptr_t>(context.cmd_list),
              .command_list_epoch = upscaler_color_command_epochs[reinterpret_cast<uintptr_t>(context.cmd_list)],
              .bound_pipeline = bound_pipeline,
              .replacement_pipeline = replacement_pipeline,
              .replacement_bound = replacement_bound,
          };
        }
      }
    }
  }

  if (capture_upscaler_color_path && !is_compute && shader_hash != 0u) {
    const auto target = downstream_capture_rtvs.find(context.cmd_list);
    if (target != downstream_capture_rtvs.end()) {
      const auto output = DescribeGammaAuditView(context.cmd_list->get_device(), target->second);
      if (output.width >= 128u && output.height >= 128u) {
        const auto record_writer = [&](uint64_t resource) {
          if (resource == 0u) return;
          const auto key = UpscalerColorWriterKey{
              .resource = resource,
              .command_list = reinterpret_cast<uintptr_t>(context.cmd_list),
              .epoch = upscaler_color_command_epochs[reinterpret_cast<uintptr_t>(context.cmd_list)],
          };
          upscaler_color_last_writers[key] = shader_hash;
          auto& chain = upscaler_color_writer_chains[key];
          if (chain.size() < 16u && (chain.empty() || chain.back() != shader_hash)) {
            chain.push_back(shader_hash);
          }
        };
        record_writer(output.resource);
        if (output.effective != output.resource) record_writer(output.effective);
      }
    }
  }

  if (capture_fg_producer && !is_compute && shader_hash != 0u) {
    const auto target = downstream_capture_rtvs.find(context.cmd_list);
    if (target != downstream_capture_rtvs.end()) {
      const auto output = DescribeGammaAuditView(context.cmd_list->get_device(), target->second);
      if (output.resource == fg_producer_audit.original_resource
          || output.effective == fg_producer_audit.clone_resource) {
        bool known_shader = false;
        for (uint32_t index = 0u; index < fg_producer_audit.count; ++index) {
          if (fg_producer_audit.pixel_shader_hashes[index] == shader_hash) {
            known_shader = true;
            break;
          }
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
    if (!is_compute && shader_hash == 0x268BAB6Du) {
      capture.count = 0u;
      capture.transfer_count = 0u;
      capture.targets.fill({});
      capture.inputs.fill({});
      // Keep t0 bindings collected while the one-shot capture was armed.
      capture.active = true;
      capture.capture_commands = capture_commands;
      capture.capture_transfers = capture_transfers;
      capture.anchor_shader_hash = shader_hash;
      if (capture_commands || capture_transfers) {
        const auto target = downstream_capture_rtvs.find(context.cmd_list);
        if (target != downstream_capture_rtvs.end()) {
          auto* device = context.cmd_list->get_device();
          const auto resource = device->get_resource_from_view(target->second);
          const auto desc = device->get_resource_desc(resource);
          capture.gamma_target = resource.handle;
          capture.gamma_target_format = desc.texture.format;
          capture.gamma_target_width = desc.texture.width;
          capture.gamma_target_height = desc.texture.height;
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

  // The main menu records many low-resolution material/compute passes after
  // Gamma and used to exhaust all 16 slots before its popup UI was drawn.
  // Keep the three proven UI shaders regardless of target, otherwise retain
  // only full-size pixel draws that can actually composite the final menu.
  if (is_compute) return {};
  const bool known_popup_ui = IsDl2PopupUiShader(shader_hash);
  DownstreamTarget candidate_target = {};
  const auto candidate_target_it = downstream_capture_rtvs.find(context.cmd_list);
  if (candidate_target_it != downstream_capture_rtvs.end()) {
    const auto resource = DescribeGammaAuditView(context.cmd_list->get_device(), candidate_target_it->second);
    candidate_target = {
        .resource = resource.resource,
        .effective = resource.effective,
        .format = resource.format,
        .effective_format = resource.effective_format,
        .view_format = resource.view_format,
        .effective_view_format = resource.effective_view_format,
        .width = resource.width,
        .height = resource.height,
        .clone_enabled = resource.view_clone_enabled,
    };
  }
  const bool full_size_target = candidate_target.width != 0u && candidate_target.height != 0u
                                && (capture.gamma_target == 0u
                                    || (candidate_target.width * 10u >= capture.gamma_target_width * 9u
                                        && candidate_target.height * 10u >= capture.gamma_target_height * 9u));
  if (!known_popup_ui && !full_size_target) return {};

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
      capture.targets[capture.count] = candidate_target;
      reshade::api::resource_view input_view = {0u};
      const auto input = downstream_capture_t0_views.find(context.cmd_list);
      if (input != downstream_capture_t0_views.end()) input_view = input->second;
      if (input_view.handle == 0u) {
        DescriptorBindingAudit t0_binding = {};
        FindGraphicsDescriptorBinding(
            context.cmd_list->get_device(),
            renodx::utils::state::GetCurrentState(context.cmd_list),
            0u,
            reshade::api::descriptor_type::texture_shader_resource_view,
            &t0_binding);
        if (t0_binding.found) input_view = t0_binding.slot.resource_view;
      }
      if (input_view.handle != 0u) {
        const auto resource = DescribeGammaAuditView(context.cmd_list->get_device(), input_view);
        capture.inputs[capture.count] = {
            .resource = resource.resource,
            .effective = resource.effective,
            .format = resource.format,
            .effective_format = resource.effective_format,
            .view_format = resource.view_format,
            .effective_view_format = resource.effective_view_format,
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

bool IsDlssFgFinalHdr10Copy(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest) {
  if (!dlss_fg_mode_active.load(std::memory_order_acquire)
      || swap_chain_use_hdr10 < 0.5f
      || cmd_list == nullptr || source.handle == 0u || dest.handle == 0u) {
    return false;
  }

  bool is_swapchain_back_buffer = false;
  uint64_t source_clone = 0u;
  uint64_t source_fallback = 0u;
  uint64_t dest_clone = 0u;
  uint64_t dest_fallback = 0u;
  bool source_clone_enabled = false;
  bool dest_clone_enabled = false;
  bool source_is_clone = false;
  bool dest_is_clone = false;
  renodx::utils::resource::GetResourceInfo(source, [&](const renodx::utils::resource::ResourceInfo& info) {
    source_clone = info.clone.handle;
    source_fallback = info.fallback.handle;
    source_clone_enabled = info.clone_enabled;
    source_is_clone = info.is_clone;
  });
  renodx::utils::resource::GetResourceInfo(dest, [&](const renodx::utils::resource::ResourceInfo& info) {
    is_swapchain_back_buffer = info.is_swap_chain;
    dest_clone = info.clone.handle;
    dest_fallback = info.fallback.handle;
    dest_clone_enabled = info.clone_enabled;
    dest_is_clone = info.is_clone;
  });
  if (!is_swapchain_back_buffer) return false;

  auto* device = cmd_list->get_device();
  if (device == nullptr) return false;
  const auto source_desc = device->get_resource_desc(source);
  const auto dest_desc = device->get_resource_desc(dest);
  const bool compatible = source_desc.type == reshade::api::resource_type::texture_2d
                          && dest_desc.type == reshade::api::resource_type::texture_2d
                          && source_desc.texture.format == reshade::api::format::r10g10b10a2_unorm
                          && dest_desc.texture.format == reshade::api::format::r10g10b10a2_unorm
                          && source_desc.texture.width == dest_desc.texture.width
                          && source_desc.texture.height == dest_desc.texture.height;

  const uint32_t diagnostic = dlss_fg_final_copy_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
  const uint32_t match_diagnostic = compatible
                                        ? dlss_fg_final_copy_match_diagnostic_count.fetch_add(1u, std::memory_order_relaxed)
                                        : UINT32_MAX;
  if (diagnostic < 16u || match_diagnostic < 8u) {
    std::ostringstream message;
    message << "DL2 DLSS FG final copy: source=0x" << std::hex << std::uppercase << source.handle
            << " dest=0x" << dest.handle
            << " source_clone=0x" << source_clone
            << " source_fallback=0x" << source_fallback
            << " dest_clone=0x" << dest_clone
            << " dest_fallback=0x" << dest_fallback
            << std::dec << " source_format=" << static_cast<uint32_t>(source_desc.texture.format)
            << " dest_format=" << static_cast<uint32_t>(dest_desc.texture.format)
            << " size=" << source_desc.texture.width << "x" << source_desc.texture.height
            << " source_clone_enabled=" << (source_clone_enabled ? 1 : 0)
            << " dest_clone_enabled=" << (dest_clone_enabled ? 1 : 0)
            << " source_is_clone=" << (source_is_clone ? 1 : 0)
            << " dest_is_clone=" << (dest_is_clone ? 1 : 0)
            << " preserve=" << (compatible ? 1 : 0);
    renodx::utils::log::i(message.str().c_str());
  }
  if (compatible) {
    std::scoped_lock lock(downstream_draw_capture_mutex);
    auto& audit = upscaler_source_writer_audit_state;
    if (audit.active && audit.target_final_fg_output) {
      if (audit.source == 0u) {
        audit.source = source.handle;
        audit.effective_source = source.handle;
        audit.source_info = {
            .resource = source.handle,
            .effective = source.handle,
            .format = source_desc.texture.format,
            .effective_format = source_desc.texture.format,
            .width = source_desc.texture.width,
            .height = source_desc.texture.height,
        };
      }
      const bool known_source = std::find(
                                    audit.recent_sources.begin(),
                                    audit.recent_sources.begin() + audit.recent_source_count,
                                    source.handle)
                                != audit.recent_sources.begin() + audit.recent_source_count;
      if (!known_source && audit.recent_source_count < audit.recent_sources.size()) {
        const uint32_t index = audit.recent_source_count++;
        audit.recent_sources[index] = source.handle;
        audit.recent_effective_sources[index] = source.handle;
      }
    }
  }
  return compatible;
}

bool RenderDlssFgAdBridge(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest) {
  if (cmd_list == nullptr || source.handle == 0u || dest.handle == 0u) return false;
  const int32_t final_color_mode = std::clamp(
      static_cast<int32_t>(dlss_fg_final_color_mode + 0.5f), 0, 11);
  if (final_color_mode != 11
      || !dlss_fg_mode_active.load(std::memory_order_acquire)
      || swap_chain_use_hdr10 < 0.5f) {
    return false;
  }

  auto* device = cmd_list->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) return false;

  const auto native_command_list = std::bit_cast<ID3D12CommandList*>(
      static_cast<uintptr_t>(cmd_list->get_native()));
  const auto command_list_type = native_command_list != nullptr
                                     ? native_command_list->GetType()
                                     : D3D12_COMMAND_LIST_TYPE_DIRECT;
  const uint32_t probe_diagnostic =
      dlss_fg_bridge_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
  if (probe_diagnostic < 16u) {
    std::ostringstream message;
    message << "DL2 DLSS FG AD bridge context probe: command_list_type="
            << static_cast<uint32_t>(command_list_type)
            << " compute_uav_supported="
            << (dlss_fg_rgb10_uav_supported.load(std::memory_order_relaxed) ? 1 : 0);
    renodx::utils::log::i(message.str().c_str());
  }
  // The callback is observed on both the final DIRECT copy list and the
  // Streamline COMPUTE list. Only the latter is a valid context for this
  // bridge; never record graphics work into a compute command list.
  if (command_list_type != D3D12_COMMAND_LIST_TYPE_COMPUTE
      || !dlss_fg_rgb10_uav_supported.load(std::memory_order_acquire)) {
    return false;
  }

  uint64_t effective_source_handle = 0u;
  reshade::api::resource_usage effective_source_state = reshade::api::resource_usage::undefined;
  {
    std::scoped_lock lock(dlss_fg_bridge_mutex);
    const auto iterator = dlss_fg_ad_effective_targets.find(source.handle);
    if (iterator == dlss_fg_ad_effective_targets.end()) return false;
    effective_source_handle = iterator->second;
    // Keep the last submitted tray valid until its replacement is actually
    // published. Clearing it here creates a window where the final copy falls
    // back to the native path between the Streamline copy and our bridge copy.
    const auto state_iterator = dlss_fg_ad_clone_states.find(effective_source_handle);
    if (state_iterator != dlss_fg_ad_clone_states.end()) {
      effective_source_state = state_iterator->second;
    }
  }
  if (effective_source_handle == 0u || effective_source_handle == source.handle) return false;

  const reshade::api::resource effective_clone{effective_source_handle};
  bool effective_source_is_clone = false;
  const bool found_effective_source = renodx::utils::resource::GetResourceInfo(
      effective_clone,
      [&](const renodx::utils::resource::ResourceInfo& info) {
        effective_source_is_clone = info.is_clone;
      });
  if (!found_effective_source || !effective_source_is_clone
      || effective_source_state == reshade::api::resource_usage::undefined) {
    return false;
  }
  // The clone state is established by the 0xAD producer draw above. Do not
  // substitute ResourceInfo::initial_state here: it is the creation state,
  // and the observed log showed it as GENERAL while the clone was an RTV.

  std::shared_ptr<DlssFgBridgePass> prepared;
  uint64_t prepared_serial = 0u;
  uint64_t required_fence_value = 0u;
  const uint64_t handoff_epoch = dlss_fg_handoff_epoch.load(std::memory_order_acquire);
  {
    std::scoped_lock lock(dlss_fg_bridge_mutex);
    const auto iterator = dlss_fg_ad_prepared_sources.find(effective_source_handle);
    if (iterator != dlss_fg_ad_prepared_sources.end()) {
      std::scoped_lock prepared_lock(dlss_fg_prepared_command_mutex);
      for (const auto& candidate : iterator->second) {
        if (candidate == nullptr || candidate->producer_pending
            || !candidate->consumer_reserved || candidate->consumer_assigned
            || candidate->producer_fence_value == 0u
            || candidate->handoff_epoch != handoff_epoch) {
          continue;
        }
        if (prepared == nullptr || candidate->producer_serial < prepared->producer_serial) {
          prepared = candidate;
        }
      }
      if (prepared != nullptr) {
        prepared->consumer_assigned = true;
        prepared_serial = prepared->producer_serial;
        required_fence_value = prepared->producer_fence_value;
      }
    }
  }
  if (prepared == nullptr || prepared->resource.handle == 0u) {
    const uint32_t diagnostic =
        dlss_fg_bridge_candidate_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
    if (diagnostic < 32u) {
      std::ostringstream message;
      message << "DL2 DLSS FG PQ handoff skipped: source=0x" << std::hex
              << effective_source_handle << " dest=0x" << dest.handle
              << " reason=no_submitted_slot";
      renodx::utils::log::i(message.str().c_str());
    }
    return false;
  }
  const reshade::api::resource effective_source = prepared->resource;
  const auto release_prepared_assignment = [&]() {
    std::scoped_lock lock(dlss_fg_prepared_command_mutex);
    if (prepared->producer_serial == prepared_serial) {
      prepared->consumer_assigned = false;
    }
  };

  bool dest_is_swapchain = false;
  bool dest_has_clone = false;
  renodx::utils::resource::GetResourceInfo(dest, [&](const renodx::utils::resource::ResourceInfo& info) {
    dest_is_swapchain = info.is_swap_chain;
    dest_has_clone = info.clone.handle != 0u || info.clone_enabled;
  });
  const auto source_desc = device->get_resource_desc(source);
  const auto effective_desc = device->get_resource_desc(effective_source);
  const auto dest_desc = device->get_resource_desc(dest);
  const char* rejection = nullptr;
  if (dest_is_swapchain) {
    rejection = "dest_is_swapchain";
  } else if (dest_has_clone) {
    rejection = "dest_has_clone";
  } else if (source_desc.type != reshade::api::resource_type::texture_2d
             || effective_desc.type != reshade::api::resource_type::texture_2d
             || dest_desc.type != reshade::api::resource_type::texture_2d) {
    rejection = "not_texture_2d";
  } else if (source_desc.texture.format != reshade::api::format::r8g8b8a8_unorm) {
    rejection = "source_not_rgba8_unorm";
  } else if (effective_desc.texture.format != reshade::api::format::r10g10b10a2_unorm) {
    rejection = "prepared_not_rgb10";
  } else if (dest_desc.texture.format != reshade::api::format::r10g10b10a2_unorm) {
    rejection = "dest_not_rgb10";
  } else if (source_desc.texture.width != dest_desc.texture.width
             || source_desc.texture.height != dest_desc.texture.height
             || effective_desc.texture.width != dest_desc.texture.width
             || effective_desc.texture.height != dest_desc.texture.height) {
    rejection = "size_mismatch";
  }
  if (rejection != nullptr) {
    release_prepared_assignment();
    const uint32_t diagnostic =
        dlss_fg_bridge_candidate_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
    if (diagnostic < 24u) {
      std::ostringstream message;
      message << "DL2 DLSS FG AD bridge candidate: source=0x" << std::hex << std::uppercase
              << source.handle << "=>0x" << effective_source.handle
              << " dest=0x" << dest.handle
              << std::dec << " source_format=" << static_cast<uint32_t>(source_desc.texture.format)
              << " effective_format=" << static_cast<uint32_t>(effective_desc.texture.format)
              << " dest_format=" << static_cast<uint32_t>(dest_desc.texture.format)
              << " dest_swapchain=" << (dest_is_swapchain ? 1 : 0)
              << " dest_clone=" << (dest_has_clone ? 1 : 0)
              << " accepted=0 reason=" << rejection;
      renodx::utils::log::i(message.str().c_str());
    }
    return false;
  }

  if (required_fence_value == 0u) {
    release_prepared_assignment();
    const uint32_t diagnostic =
        dlss_fg_bridge_candidate_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
    if (diagnostic < 24u) {
      std::ostringstream message;
      message << "DL2 DLSS FG PQ handoff skipped: prepared=0x" << std::hex
              << prepared->resource.handle << " dest=0x" << dest.handle
              << " reason=prepared_not_submitted";
      renodx::utils::log::i(message.str().c_str());
    }
    return false;
  }
  static thread_local bool bridge_recording = false;
  if (bridge_recording) {
    release_prepared_assignment();
    return false;
  }
  bridge_recording = true;
  {
    std::scoped_lock recording_lock(prepared->recording_mutex);
    cmd_list->copy_resource(prepared->resource, dest);
  }
  // The prepared PQ/RGB10 resource was produced on the Direct queue. Associate
  // this exact version with only the Streamline Compute command list that
  // copies it, so unrelated Compute submissions are never stalled.
  {
    std::scoped_lock lock(dlss_fg_prepared_command_mutex);
    const auto command_handle = reinterpret_cast<uintptr_t>(cmd_list->get_native());
    dlss_fg_bridge_compute_uses[command_handle].push_back({
        .prepared = prepared,
        .handoff_epoch = handoff_epoch,
        .producer_serial = prepared_serial,
        .producer_fence_value = required_fence_value,
        .tray = dest.handle,
        .generation = dlss_fg_swapchain_generation.load(std::memory_order_acquire),
    });
  }
  bridge_recording = false;

  const uint32_t diagnostic = dlss_fg_bridge_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
  if (diagnostic < 24u) {
    std::ostringstream message;
    message << "DL2 DLSS FG AD bridge: source=0x" << std::hex << std::uppercase
            << source.handle << "=>0x" << effective_source.handle
            << " dest=0x" << dest.handle
            << std::dec << " copied=1"
            << " formats=" << static_cast<uint32_t>(source_desc.texture.format)
            << "=>" << static_cast<uint32_t>(effective_desc.texture.format)
            << "=>" << static_cast<uint32_t>(dest_desc.texture.format)
            << " peak=" << shader_injection.peak_white_nits
            << " prepared_state=copy_source contract=pq_bt2100"
            << " slot=" << prepared->slot
            << " producer_serial=" << prepared_serial
            << " producer_fence=" << required_fence_value
            << " consumer_reserved=1";
    renodx::utils::log::i(message.str().c_str());
  }
  return true;
}

bool IsDlssFgBridgedTray(uint64_t resource) {
  if (resource == 0u) return false;
  const auto generation = dlss_fg_swapchain_generation.load(std::memory_order_acquire);
  const auto handoff_epoch = dlss_fg_handoff_epoch.load(std::memory_order_acquire);
  std::scoped_lock lock(dlss_fg_bridge_mutex);
  const auto iterator = dlss_fg_bridged_trays.find(resource);
  return iterator != dlss_fg_bridged_trays.end()
         && iterator->second.swapchain_generation == generation
         && iterator->second.handoff_epoch == handoff_epoch
         && iterator->second.consumer_fence_value != 0u;
}

void RecordUpscalerSourceTransfer(
    UpscalerSourceWriterType type,
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest) {
  std::scoped_lock lock(downstream_draw_capture_mutex);
  auto& audit = upscaler_source_writer_audit_state;
  if (!audit.active || audit.recent_source_count == 0u || audit.count >= audit.writers.size()) return;
  const bool matches_recent = std::any_of(
      audit.recent_sources.begin(), audit.recent_sources.begin() + audit.recent_source_count,
      [&](uint64_t value) { return dest.handle == value; });
  if (!matches_recent && dest.handle != audit.exposure && dest.handle != audit.effective_exposure) return;
  audit.writers[audit.count++] = {
      .type = type,
      .present_index = audit.presents + 1u,
      .command_buffer = cmd_list != nullptr
                            ? reinterpret_cast<uint64_t>(cmd_list->get_native())
                            : 0u,
      .source = source.handle,
      .target = dest.handle,
  };
}

bool OnDownstreamCopyResource(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest) {
  static thread_local uint32_t callback_depth = 0u;
  ++callback_depth;
  const auto current_depth = callback_depth;
  const bool preserve_native_copy = IsDlssFgFinalHdr10Copy(cmd_list, source, dest);
  if (preserve_native_copy) {
    const uint32_t diagnostic = dlss_fg_final_copy_match_diagnostic_count.load(std::memory_order_relaxed);
    if (diagnostic <= 8u) {
      std::ostringstream message;
      message << "DL2 DLSS FG final copy callback: depth=" << current_depth;
      renodx::utils::log::i(message.str().c_str());
    }
  }
  MarkDlssFgCommandListSwapchainWrite(cmd_list, source, dest, false, true, preserve_native_copy);
  RecordDlssFgTagTransfer(DownstreamTransferType::copy_resource, cmd_list, source, dest);
  RecordDownstreamTransfer(DownstreamTransferType::copy_resource, cmd_list, source, dest);
  RecordUpscalerSourceTransfer(UpscalerSourceWriterType::copy_resource, cmd_list, source, dest);
  if (current_depth == 1u
      && !preserve_native_copy
      && RenderDlssFgAdBridge(cmd_list, source, dest)) {
    --callback_depth;
    return true;
  }
  if (preserve_native_copy) {
    // Streamline has already produced the final RGB10 frame. Keep this copy on
    // the real backbuffer instead of letting resource upgrading redirect it to
    // a per-backbuffer FP16 clone that may contain an older rendered frame.
    cmd_list->copy_resource(source, dest);
    --callback_depth;
    return true;
  }
  --callback_depth;
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
  MarkDlssFgCommandListSwapchainWrite(cmd_list, source, dest, false, true);
  RecordDlssFgTagTransfer(DownstreamTransferType::copy_texture_region, cmd_list, source, dest);
  RecordDownstreamTransfer(DownstreamTransferType::copy_texture_region, cmd_list, source, dest);
  RecordUpscalerSourceTransfer(
      UpscalerSourceWriterType::copy_texture_region, cmd_list, source, dest);
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
  MarkDlssFgCommandListSwapchainWrite(cmd_list, source, dest, false, true);
  RecordDlssFgTagTransfer(DownstreamTransferType::resolve_texture_region, cmd_list, source, dest);
  RecordDownstreamTransfer(DownstreamTransferType::resolve_texture_region, cmd_list, source, dest);
  RecordUpscalerSourceTransfer(
      UpscalerSourceWriterType::resolve_texture_region, cmd_list, source, dest);
  return false;
}

void OnDlssFgBackbufferBarrier(
    reshade::api::command_list* cmd_list,
    uint32_t count,
    const reshade::api::resource* resources,
    const reshade::api::resource_usage* old_states,
    const reshade::api::resource_usage* new_states) {
  if (resources == nullptr || old_states == nullptr || new_states == nullptr) {
    return;
  }
  const bool submission_audit_active = dlss_fg_mode_active.load(std::memory_order_acquire)
                                       && dlss_fg_execute_candidate_remaining.load(std::memory_order_relaxed) != 0u;
  const bool manual_capture_active = dlss_fg_backbuffer_barrier_capture.load(std::memory_order_relaxed) != 0u;
  if (!submission_audit_active && !manual_capture_active) return;

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

    if (submission_audit_active) {
      std::scoped_lock lock(dlss_fg_command_list_candidate_mutex);
      auto& candidate = dlss_fg_command_list_candidates[reinterpret_cast<uintptr_t>(cmd_list)];
      candidate.swapchain_generation = dlss_fg_swapchain_generation.load(std::memory_order_acquire);
      candidate.back_buffer = resource.handle;
      if (candidate.transition_count == 0u) {
        candidate.first_old_state = static_cast<uint32_t>(old_states[index]);
      }
      candidate.last_new_state = static_cast<uint32_t>(new_states[index]);
      ++candidate.transition_count;
      if (new_states[index] == reshade::api::resource_usage::render_target) {
        candidate.entered_render_target = true;
      }
      if (old_states[index] == reshade::api::resource_usage::render_target
          && new_states[index] == reshade::api::resource_usage::present) {
        candidate.returned_to_present = true;
      }
    }

    if (!manual_capture_active) continue;

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

void OnDlssFgResetCommandList(reshade::api::command_list* cmd_list) {
  {
    std::scoped_lock lock(dlss_fg_prepared_command_mutex);
    const auto command_handle = reinterpret_cast<uintptr_t>(cmd_list->get_native());
    const auto producer_iterator = dlss_fg_prepared_command_resources.find(command_handle);
    if (producer_iterator != dlss_fg_prepared_command_resources.end()) {
      for (const auto& [resource, record_serial] : producer_iterator->second) {
        const auto slot_iterator = dlss_fg_prepared_slots.find(resource);
        if (slot_iterator == dlss_fg_prepared_slots.end()) continue;
        const auto prepared = slot_iterator->second.lock();
        if (prepared != nullptr && prepared->producer_serial == record_serial) {
          prepared->producer_pending = false;
          prepared->consumer_reserved = false;
          prepared->consumer_assigned = false;
        }
      }
    }
    const auto consumer_iterator = dlss_fg_bridge_compute_uses.find(command_handle);
    if (consumer_iterator != dlss_fg_bridge_compute_uses.end()) {
      for (const auto& use : consumer_iterator->second) {
        if (use.prepared != nullptr
            && use.prepared->producer_serial == use.producer_serial) {
          use.prepared->consumer_reserved = false;
          use.prepared->consumer_assigned = false;
        }
      }
      dlss_fg_bridge_compute_uses.erase(consumer_iterator);
    }
    dlss_fg_prepared_command_resources.erase(command_handle);
  }
  {
    std::scoped_lock lock(dlss_fg_command_list_candidate_mutex);
    dlss_fg_command_list_candidates.erase(reinterpret_cast<uintptr_t>(cmd_list));
  }
  {
    std::scoped_lock audit_lock(downstream_draw_capture_mutex);
    dlss_fg_ad_command_list_markers.erase(reinterpret_cast<uintptr_t>(cmd_list));
    ++upscaler_color_command_epochs[reinterpret_cast<uintptr_t>(cmd_list)];
    upscaler_source_compute_uav_views.erase(cmd_list);
    upscaler_source_compute_srv_views.erase(cmd_list);
  }
  {
    std::scoped_lock lock(dlss_fg_post_execute_mutex);
    dlss_fg_post_execute_markers.erase(static_cast<uintptr_t>(cmd_list->get_native()));
  }
}

void OnDlssFgExecuteCommandList(
    reshade::api::command_queue* queue,
    reshade::api::command_list* cmd_list) {
  const uint64_t classification_generation =
      dlss_fg_frame_classification_generation.load(std::memory_order_acquire);
  DlssFgAdCommandListMarker ad_marker = {};
  bool contains_target_ad = false;
  {
    std::scoped_lock lock(downstream_draw_capture_mutex);
    if (upscaler_color_path_audit_state.active) {
      const uint64_t command_list = reinterpret_cast<uintptr_t>(cmd_list);
      const uint64_t epoch = upscaler_color_command_epochs[command_list];
      const uint64_t serial = ++upscaler_color_execute_serial;
      for (uint32_t index = 0u; index < upscaler_color_path_audit_state.count; ++index) {
        auto& entry = upscaler_color_path_audit_state.entries[index];
        if (entry.command_list == command_list && entry.command_list_epoch == epoch) {
          entry.execute_serial = serial;
        }
      }
      for (auto& observation : upscaler_color_writer_observations) {
        if (observation.command_list == command_list && observation.command_list_epoch == epoch) {
          observation.execute_serial = serial;
        }
      }
    }
    const auto marker = dlss_fg_ad_command_list_markers.find(reinterpret_cast<uintptr_t>(cmd_list));
    if (marker != dlss_fg_ad_command_list_markers.end()) {
      ad_marker = marker->second;
      contains_target_ad = true;
      dlss_fg_ad_command_list_markers.erase(marker);
    }
  }
  if (!dlss_fg_mode_active.load(std::memory_order_acquire)) return;

  DlssFgCommandListCandidate candidate = {};
  bool contains_backbuffer_candidate = false;
  {
    std::scoped_lock lock(dlss_fg_command_list_candidate_mutex);
    const auto iterator = dlss_fg_command_list_candidates.find(reinterpret_cast<uintptr_t>(cmd_list));
    if (iterator != dlss_fg_command_list_candidates.end()) {
      candidate = iterator->second;
      contains_backbuffer_candidate = true;
      dlss_fg_command_list_candidates.erase(iterator);
    }
  }
  if (contains_target_ad) {
    dlss_fg_ad_submission_serial.fetch_add(1u, std::memory_order_acq_rel);
    uint32_t exact_remaining = dlss_fg_exact_ad_ordering_remaining.load(std::memory_order_relaxed);
    while (exact_remaining != 0u
           && !dlss_fg_exact_ad_ordering_remaining.compare_exchange_weak(
               exact_remaining, exact_remaining - 1u,
               std::memory_order_acq_rel, std::memory_order_relaxed)) {
    }
    if (exact_remaining != 0u) {
      uint64_t exact_event = 0u;
      {
        std::scoped_lock timing_lock(dlss_fg_timing_mutex);
        exact_event = dlss_fg_identity_event_serial.fetch_add(1u, std::memory_order_relaxed) + 1u;
        dlss_fg_timing.last_ad_execute_event = exact_event;
        dlss_fg_timing.last_ad_command_list = reinterpret_cast<uintptr_t>(cmd_list);
        dlss_fg_timing.last_ad_epoch = ad_marker.epoch;
        dlss_fg_timing.last_ad_target = ad_marker.target;
        dlss_fg_timing.last_ad_effective_target = ad_marker.effective_target;
      }
      std::ostringstream exact_message;
      exact_message << "DL2 DLSS FG exact AD submission: event=" << exact_event
                    << " thread=" << GetCurrentThreadId()
                    << " queue=0x" << std::hex << std::uppercase
                    << reinterpret_cast<uintptr_t>(queue)
                    << " cmd=0x" << reinterpret_cast<uintptr_t>(cmd_list)
                    << " epoch=" << std::dec << ad_marker.epoch
                    << " target=0x" << std::hex << ad_marker.target
                    << "=>0x" << ad_marker.effective_target
                    << " backbuffer=0x" << candidate.back_buffer
                    << " remaining=" << std::dec << (exact_remaining - 1u);
      renodx::utils::log::i(exact_message.str().c_str());
    }

    {
      std::scoped_lock lock(dlss_fg_post_execute_mutex);
      dlss_fg_post_execute_markers[static_cast<uintptr_t>(cmd_list->get_native())] = ad_marker;
    }
  }
  if (!contains_backbuffer_candidate) return;
  if (candidate.swapchain_generation
      != dlss_fg_swapchain_generation.load(std::memory_order_acquire)) {
    return;
  }
  if (!candidate.entered_render_target && !candidate.returned_to_present
      && !candidate.bound_swapchain_rtv && !candidate.copied_to_swapchain) return;

  if (candidate.preserved_native_copy) {
    const int32_t final_color_mode = std::clamp(
        static_cast<int32_t>(dlss_fg_final_color_mode + 0.5f), 0, 11);
    if (final_color_mode == 0 || final_color_mode == 11) {
      renodx::mods::swapchain::ConsumeProxySourceForBackBuffer({candidate.back_buffer});
      renodx::mods::swapchain::SkipProxyDrawForBackBuffer({candidate.back_buffer});
    } else {
      {
        const std::scoped_lock lock(renodx::utils::mutex::global_mutex);
        shader_injection.luminance_stage_padding_1 = static_cast<float>(final_color_mode);
      }
      renodx::mods::swapchain::ConsumeProxyDrawSkipForBackBuffer({candidate.back_buffer});
      renodx::mods::swapchain::SetProxySourceForBackBuffer(
          {candidate.back_buffer}, {candidate.copy_source});
    }
  }

  uint32_t remaining = dlss_fg_execute_candidate_remaining.load(std::memory_order_relaxed);
  while (remaining != 0u
         && !dlss_fg_execute_candidate_remaining.compare_exchange_weak(
             remaining, remaining - 1u, std::memory_order_acq_rel, std::memory_order_relaxed)) {
  }
  if (remaining == 0u) return;

  if (candidate.preserved_native_copy
      && dlss_fg_frame_classification_remaining.load(std::memory_order_acquire) != 0u) {
    const uint64_t native_command_buffer = reinterpret_cast<uint64_t>(cmd_list->get_native());
    std::scoped_lock input_lock(dlss_fg_input_snapshot_mutex);
    DlssFgInputSnapshot* matched_input = nullptr;
    for (auto& input : dlss_fg_input_snapshots) {
      if (input.generation != classification_generation
          || !input.color_committed || input.final_copy_consumed
          || input.frame_index == UINT_MAX) {
        continue;
      }
      const bool context_matches = std::find(
                                       input.command_buffers.begin(),
                                       input.command_buffers.begin() + input.command_buffer_count,
                                       native_command_buffer)
                                   != input.command_buffers.begin() + input.command_buffer_count;
      if (!context_matches) continue;
      if (matched_input == nullptr || input.tag_serial < matched_input->tag_serial) {
        matched_input = &input;
      }
    }
    if (matched_input != nullptr) {
      candidate.tag_serial = matched_input->tag_serial;
      candidate.frame_index = matched_input->frame_index;
      candidate.tag_context_match = true;
      matched_input->final_copy_consumed = true;
    }
  }

  const int32_t final_color_mode = std::clamp(
      static_cast<int32_t>(dlss_fg_final_color_mode + 0.5f), 0, 11);
  const bool bridge_ready = IsDlssFgBridgedTray(candidate.copy_source);
  const bool roundtrip_final_color = final_color_mode != 0 && final_color_mode != 11;
  uint32_t source_format = 0u;
  const bool classification_active =
      dlss_fg_frame_classification_remaining.load(std::memory_order_acquire) != 0u;
  if (classification_active && candidate.preserved_native_copy
      && queue != nullptr && queue->get_device() != nullptr && candidate.copy_source != 0u) {
    const auto source_resource = reshade::api::resource{
        static_cast<uintptr_t>(candidate.copy_source)};
    source_format = static_cast<uint32_t>(
        queue->get_device()->get_resource_desc(source_resource).texture.format);
  }
  uint64_t event = 0u;
  {
    std::scoped_lock audit_lock(
        dlss_fg_timing_mutex,
        dlss_fg_frame_classification_mutex);
    event = dlss_fg_identity_event_serial.fetch_add(1u, std::memory_order_relaxed) + 1u;
    dlss_fg_timing.last_execute_event = event;
    dlss_fg_timing.last_execute_backbuffer = candidate.back_buffer;
    if (classification_active && candidate.preserved_native_copy
        && dlss_fg_frame_classification.generation == classification_generation
        && dlss_fg_frame_classification.remaining != 0u) {
      auto& audit = dlss_fg_frame_classification;
      if (audit.pending_count < audit.pending_copies.size()) {
        audit.pending_copies[audit.pending_count++] = {
            .event = event,
            .back_buffer = candidate.back_buffer,
            .copy_source = candidate.copy_source,
            .command_buffer = reinterpret_cast<uint64_t>(cmd_list->get_native()),
            .tag_serial = candidate.tag_serial,
            .frame_index = candidate.frame_index,
            .source_format = source_format,
            .tag_context_match = candidate.tag_context_match,
            .roundtrip = roundtrip_final_color,
        };
      } else {
        ++audit.dropped_count;
      }
    }
  }
  auto describe_submission_resource = [queue](uint64_t handle) {
    std::ostringstream details;
    if (handle == 0u || queue == nullptr || queue->get_device() == nullptr) {
      details << "format=unknown";
      return details.str();
    }
    auto* device = queue->get_device();
    const auto resource = reshade::api::resource{static_cast<uintptr_t>(handle)};
    const auto desc = device->get_resource_desc(resource);
    details << "format=" << static_cast<uint32_t>(desc.texture.format);
    renodx::utils::resource::GetResourceInfo(resource, [&](const renodx::utils::resource::ResourceInfo& info) {
      details << " clone=0x" << std::hex << info.clone.handle << std::dec
              << " clone_enabled=" << (info.clone_enabled ? 1 : 0)
              << " clone_format=" << static_cast<uint32_t>(info.clone_desc.texture.format)
              << " is_swapchain=" << (info.is_swap_chain ? 1 : 0)
              << " is_clone=" << (info.is_clone ? 1 : 0)
              << " views=" << info.resource_view_handles.size();
    });
    return details.str();
  };
  std::ostringstream message;
  message << "DL2 DLSS FG backbuffer submission: event=" << event
          << " thread=" << GetCurrentThreadId()
          << " queue=0x" << std::hex << std::uppercase << reinterpret_cast<uintptr_t>(queue)
          << " cmd=0x" << reinterpret_cast<uintptr_t>(cmd_list)
          << " backbuffer=0x" << candidate.back_buffer
          << " backbuffer_" << describe_submission_resource(candidate.back_buffer)
          << " copy_source=0x" << candidate.copy_source
          << " copy_source_" << describe_submission_resource(candidate.copy_source)
          << " final_color_mode=" << final_color_mode
          << " bridge_ready=" << (bridge_ready ? 1 : 0)
          << " proxy_action=" << (roundtrip_final_color ? "force_proxy_source" : "skip_generated_proxy")
          << " output_hdr10=" << (swap_chain_use_hdr10 >= 0.5f ? 1 : 0)
          << " entered_rt=" << std::dec << (candidate.entered_render_target ? 1 : 0)
          << " returned_present=" << (candidate.returned_to_present ? 1 : 0)
          << " bound_rtv=" << (candidate.bound_swapchain_rtv ? 1 : 0)
          << " copy_dest=" << (candidate.copied_to_swapchain ? 1 : 0)
          << " preserve_copy=" << (candidate.preserved_native_copy ? 1 : 0)
          << " transitions=" << candidate.transition_count
          << " states=0x" << std::hex << candidate.first_old_state
          << "=>0x" << candidate.last_new_state
          << std::dec
          << " tag=" << candidate.tag_serial
          << " frame=" << candidate.frame_index
          << " tag_context_match=" << (candidate.tag_context_match ? 1 : 0)
          << " remaining=" << (remaining - 1u);
  renodx::utils::log::i(message.str().c_str());
}

void ProcessDlssFgNativePostExecute(
    reshade::api::command_queue* queue,
    ID3D12CommandList* command_list) {
  if (!dlss_fg_mode_active.load(std::memory_order_acquire)
      || queue == nullptr || command_list == nullptr) {
    return;
  }

  DlssFgAdCommandListMarker marker = {};
  {
    std::scoped_lock lock(dlss_fg_post_execute_mutex);
    const auto iterator = dlss_fg_post_execute_markers.find(reinterpret_cast<uintptr_t>(command_list));
    if (iterator == dlss_fg_post_execute_markers.end()) return;
    marker = iterator->second;
    dlss_fg_post_execute_markers.erase(iterator);
  }

  const auto swapchain_handle = dlss_fg_active_swapchain.load(std::memory_order_acquire);
  auto* swapchain = reinterpret_cast<reshade::api::swapchain*>(swapchain_handle);
  if (swapchain == nullptr || swapchain->get_device() != queue->get_device()) {
    renodx::utils::log::w("DL2 DLSS FG post-Execute: active swapchain unavailable or queue mismatch.");
    return;
  }

  const auto back_buffer = swapchain->get_current_back_buffer();
  if (back_buffer.handle == 0u) {
    renodx::utils::log::w("DL2 DLSS FG post-Execute: current backbuffer unavailable.");
    return;
  }

  // This native queue callback runs outside ReShade's immediate-command-list
  // lifetime. Rendering or flushing here corrupts the D3D12 submission order.
  // Keep the timing marker read-only; the normal Present callback owns proxy rendering.
  const bool rendered = false;

  const uint32_t diagnostic = dlss_fg_post_execute_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
  if (diagnostic < 32u) {
    std::ostringstream message;
    message << "DL2 DLSS FG post-Execute proxy: cmd=0x" << std::hex << std::uppercase
            << reinterpret_cast<uintptr_t>(command_list)
            << " target=0x" << marker.target
            << "=>0x" << marker.effective_target
            << " backbuffer=0x" << back_buffer.handle
            << " rendered=" << std::dec << (rendered ? 1 : 0)
            << " deferred_to_present=1";
    renodx::utils::log::i(message.str().c_str());
  }
}

void STDMETHODCALLTYPE HookedDlssFgNativeExecuteCommandLists(
    ID3D12CommandQueue* queue,
    UINT count,
    ID3D12CommandList* const* command_lists) {
  std::unordered_map<uint64_t, uint64_t> prepared_resources;
  std::vector<DlssFgBridgeComputeUse> compute_uses;
  if (command_lists != nullptr) {
    std::scoped_lock lock(dlss_fg_prepared_command_mutex);
    for (UINT index = 0u; index < count; ++index) {
      if (command_lists[index] == nullptr) continue;
      const auto command_handle = reinterpret_cast<uintptr_t>(command_lists[index]);
      const auto compute_iterator = dlss_fg_bridge_compute_uses.find(command_handle);
      if (compute_iterator != dlss_fg_bridge_compute_uses.end()) {
        compute_uses.insert(
            compute_uses.end(),
            compute_iterator->second.begin(), compute_iterator->second.end());
        dlss_fg_bridge_compute_uses.erase(compute_iterator);
      }
      const auto resources_iterator = dlss_fg_prepared_command_resources.find(command_handle);
      if (resources_iterator != dlss_fg_prepared_command_resources.end()) {
        for (const auto& [resource, record_serial] : resources_iterator->second) {
          auto& submitted_serial = prepared_resources[resource];
          submitted_serial = std::max(submitted_serial, record_serial);
        }
        dlss_fg_prepared_command_resources.erase(resources_iterator);
      }
    }
  }
  bool producer_waits_succeeded = queue != nullptr || compute_uses.empty();
  // A failed queue wait indicates an invalid/device-removed queue. The command
  // list has already been recorded by Streamline, so keep the slot quarantined
  // and suppress publication rather than adding a CPU wait or queue flush here.
  for (const auto& use : compute_uses) {
    if (queue == nullptr || use.prepared == nullptr || use.prepared->producer_fence == nullptr
        || FAILED(queue->Wait(use.prepared->producer_fence.Get(), use.producer_fence_value))) {
      producer_waits_succeeded = false;
      std::ostringstream message;
      message << "DL2 DLSS FG bridge fence: slot wait failed slot="
              << (use.prepared != nullptr ? use.prepared->slot : UINT_MAX)
              << " producer_serial=" << use.producer_serial
              << " quarantined=1";
      renodx::utils::log::w(message.str().c_str());
      continue;
    }
    static std::atomic_uint32_t bridge_wait_diagnostic_count = 0u;
    const auto diagnostic = bridge_wait_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
    if (diagnostic < 8u) {
      std::ostringstream message;
      message << "DL2 DLSS FG bridge fence: wait queue=0x" << std::hex
              << reinterpret_cast<uintptr_t>(queue) << std::dec
              << " slot=" << use.prepared->slot
              << " value=" << use.producer_fence_value
              << " scope=prepared_resource";
      renodx::utils::log::i(message.str().c_str());
    }
  }
  real_dlss_fg_native_execute_command_lists(queue, count, command_lists);

  if (!prepared_resources.empty() && queue != nullptr) {
    uint32_t signaled_slots = 0u;
    for (const auto& [resource, record_serial] : prepared_resources) {
      std::shared_ptr<DlssFgBridgePass> prepared;
      {
        std::scoped_lock lock(dlss_fg_prepared_command_mutex);
        const auto serial_iterator = dlss_fg_prepared_resource_record_serials.find(resource);
        const auto slot_iterator = dlss_fg_prepared_slots.find(resource);
        if (serial_iterator != dlss_fg_prepared_resource_record_serials.end()
            && serial_iterator->second == record_serial
            && slot_iterator != dlss_fg_prepared_slots.end()) {
          prepared = slot_iterator->second.lock();
        }
      }
      if (prepared == nullptr || prepared->producer_serial != record_serial
          || prepared->producer_fence == nullptr) {
        continue;
      }
      const HRESULT signal_hr = queue->Signal(prepared->producer_fence.Get(), record_serial);
      if (FAILED(signal_hr)) {
        std::ostringstream message;
        message << "DL2 DLSS FG bridge fence: signal failed hr=0x" << std::hex
                << static_cast<uint32_t>(signal_hr) << std::dec
                << " slot=" << prepared->slot << " quarantined=1";
        renodx::utils::log::w(message.str().c_str());
        continue;
      }
      {
        std::scoped_lock lock(dlss_fg_prepared_command_mutex);
        if (prepared->producer_serial == record_serial) {
          prepared->producer_fence_value = record_serial;
          prepared->producer_pending = false;
        }
      }
      ++signaled_slots;
    }
    static std::atomic_uint32_t producer_signal_diagnostic_count = 0u;
    const auto diagnostic =
        producer_signal_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
    if (diagnostic < 8u) {
      std::ostringstream message;
      message << "DL2 DLSS FG bridge fence: signal queue=0x" << std::hex
              << reinterpret_cast<uintptr_t>(queue) << std::dec
              << " slots=" << signaled_slots;
      renodx::utils::log::i(message.str().c_str());
    }
  }

  std::vector<DlssFgBridgeComputeUse> completed_uses;
  if (!compute_uses.empty() && queue != nullptr && producer_waits_succeeded) {
    for (const auto& use : compute_uses) {
      if (use.prepared == nullptr || use.prepared->consumer_fence == nullptr) continue;
      const HRESULT signal_hr =
          queue->Signal(use.prepared->consumer_fence.Get(), use.producer_serial);
      if (FAILED(signal_hr)) {
        std::ostringstream message;
        message << "DL2 DLSS FG consumer fence: signal failed hr=0x" << std::hex
                << static_cast<uint32_t>(signal_hr) << std::dec
                << " slot=" << use.prepared->slot << " quarantined=1";
        renodx::utils::log::w(message.str().c_str());
        continue;
      }
      {
        std::scoped_lock lock(dlss_fg_prepared_command_mutex);
        if (use.prepared->producer_serial != use.producer_serial
            || !use.prepared->consumer_assigned) {
          continue;
        }
        use.prepared->consumer_fence_value = use.producer_serial;
        use.prepared->consumer_reserved = false;
        use.prepared->consumer_assigned = false;
      }
      completed_uses.push_back(use);
    }
  }
  if (!completed_uses.empty()) {
    const auto current_epoch = dlss_fg_handoff_epoch.load(std::memory_order_acquire);
    {
      std::scoped_lock lock(dlss_fg_bridge_mutex);
      for (const auto& use : completed_uses) {
        if (use.handoff_epoch != current_epoch) continue;
        dlss_fg_bridged_trays[use.tray] = {
            .swapchain_generation = use.generation,
            .handoff_epoch = use.handoff_epoch,
            .producer_serial = use.producer_serial,
            .consumer_fence_value = use.producer_serial,
        };
      }
    }
    static std::atomic_uint32_t consumer_signal_diagnostic_count = 0u;
    const auto diagnostic =
        consumer_signal_diagnostic_count.fetch_add(1u, std::memory_order_relaxed);
    if (diagnostic < 16u) {
      std::ostringstream message;
      message << "DL2 DLSS FG consumer fence: signal queue=0x" << std::hex
              << reinterpret_cast<uintptr_t>(queue) << std::dec
              << " uses=" << completed_uses.size()
              << " slot=" << completed_uses.front().prepared->slot
              << " producer_serial=" << completed_uses.front().producer_serial
              << " tray=0x" << std::hex << completed_uses.front().tray;
      renodx::utils::log::i(message.str().c_str());
    }
  }

  reshade::api::command_queue* reshade_queue = nullptr;
  {
    std::scoped_lock lock(dlss_fg_native_queue_mutex);
    const auto iterator = dlss_fg_native_queues.find(reinterpret_cast<uintptr_t>(queue));
    if (iterator != dlss_fg_native_queues.end()) reshade_queue = iterator->second;
  }
  if (reshade_queue == nullptr || command_lists == nullptr) return;

  for (UINT index = 0u; index < count; ++index) {
    if (command_lists[index] != nullptr) {
      ProcessDlssFgNativePostExecute(reshade_queue, command_lists[index]);
    }
  }
}

void RegisterDlssFgNativeQueue(reshade::api::command_queue* queue) {
  if (queue == nullptr || queue->get_device() == nullptr
      || queue->get_device()->get_api() != reshade::api::device_api::d3d12) {
    return;
  }

  auto* native_queue = reinterpret_cast<ID3D12CommandQueue*>(static_cast<uintptr_t>(queue->get_native()));
  if (native_queue == nullptr) return;

  std::scoped_lock lock(dlss_fg_native_queue_mutex);
  dlss_fg_native_queues[reinterpret_cast<uintptr_t>(native_queue)] = queue;
  if (dlss_fg_native_execute_hook_installed) return;

  void** vtable = *reinterpret_cast<void***>(native_queue);
  if (vtable == nullptr || vtable[10] == nullptr) return;
  real_dlss_fg_native_execute_command_lists =
      reinterpret_cast<DlssFgNativeExecuteCommandLists>(vtable[10]);

  if (DetourTransactionBegin() != NO_ERROR) {
    real_dlss_fg_native_execute_command_lists = nullptr;
    renodx::utils::log::w("DL2 DLSS FG: native ExecuteCommandLists hook transaction failed.");
    return;
  }
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
      || DetourAttach(
             reinterpret_cast<void**>(&real_dlss_fg_native_execute_command_lists),
             reinterpret_cast<void*>(&HookedDlssFgNativeExecuteCommandLists))
             != NO_ERROR
      || DetourTransactionCommit() != NO_ERROR) {
    DetourTransactionAbort();
    real_dlss_fg_native_execute_command_lists = nullptr;
    renodx::utils::log::w("DL2 DLSS FG: native ExecuteCommandLists hook installation failed.");
    return;
  }
  dlss_fg_native_execute_hook_installed = true;
  renodx::utils::log::i("DL2 DLSS FG: native ExecuteCommandLists post-submit hook installed.");
}

void UnregisterDlssFgNativeQueue(reshade::api::command_queue* queue) {
  if (queue == nullptr) return;
  const auto native_handle = static_cast<uintptr_t>(queue->get_native());
  std::scoped_lock lock(dlss_fg_native_queue_mutex);
  dlss_fg_native_queues.erase(native_handle);
}

void RemoveDlssFgNativeExecuteHook() {
  std::scoped_lock lock(dlss_fg_native_queue_mutex);
  if (dlss_fg_native_execute_hook_installed) {
    if (DetourTransactionBegin() == NO_ERROR
        && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
        && DetourDetach(
               reinterpret_cast<void**>(&real_dlss_fg_native_execute_command_lists),
               reinterpret_cast<void*>(&HookedDlssFgNativeExecuteCommandLists))
               == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR) {
      dlss_fg_native_execute_hook_installed = false;
      real_dlss_fg_native_execute_command_lists = nullptr;
    } else {
      DetourTransactionAbort();
    }
  }
  dlss_fg_native_queues.clear();
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
  if (upscaler_input_audit_state.active) {
    auto& audit = upscaler_input_audit_state;
    ++audit.presents;
    // DLSS-G can interleave generated Presents with real rendering. Complete
    // on four actual 0x3E draws, with a bounded timeout for a genuine miss.
    if (audit.count >= 4u || audit.presents >= 32u) {
      std::ostringstream stream;
      stream << "DL2 0x3E input/curve audit: capture=" << audit.capture_id
             << " generations=" << audit.start_generation << "=>"
             << dlss_fg_swapchain_generation.load(std::memory_order_relaxed)
             << " presents=" << audit.presents << " count=" << audit.count;
      for (uint32_t index = 0u; index < audit.count; ++index) {
        const auto& entry = audit.entries[index];
        stream << " #" << (index + 1u)
               << " present=" << entry.present_index
               << " api=" << entry.api
               << " gen=" << entry.generation
               << " draw=" << entry.draw_count << "x" << entry.instance_count
               << " t0@srv" << entry.t0_table << ":" << entry.t0_binding
               << "(0x" << std::hex << entry.source.resource << "," << std::dec
               << static_cast<uint32_t>(entry.source.format) << "=>0x" << std::hex
               << entry.source.effective << "," << std::dec
               << static_cast<uint32_t>(entry.source.effective_format) << ","
               << entry.source.width << "x" << entry.source.height
               << ",clone=" << (entry.source.view_clone_enabled ? 1 : 0) << ")"
               << " t1@srv" << entry.t1_table << ":" << entry.t1_binding
               << "(0x" << std::hex << entry.exposure.resource << "," << std::dec
               << static_cast<uint32_t>(entry.exposure.format) << "=>0x" << std::hex
               << entry.exposure.effective << "," << std::dec
               << static_cast<uint32_t>(entry.exposure.effective_format) << ","
               << entry.exposure.width << "x" << entry.exposure.height
               << ",clone=" << (entry.exposure.view_clone_enabled ? 1 : 0) << ")"
               << " cb0@cbv" << entry.cb0_table << ":" << entry.cb0_binding
               << "(0x" << std::hex << entry.curve.resource << "+0x" << entry.curve.offset
               << ",size=0x" << entry.curve.size << std::dec
               << ",cached=" << (entry.curve.cache_available ? entry.curve.cached_float_count : 0u) << ")";
        if (entry.curve.cache_available) {
          stream << " values=[";
          for (uint32_t value_index = 0u; value_index < entry.curve.cached_float_count; ++value_index) {
            if (value_index != 0u) stream << ",";
            stream << "0x" << std::hex << entry.curve.bits[value_index] << std::dec
                   << ":" << entry.curve.values[value_index];
          }
          stream << "]";
        }
      }
      renodx::utils::log::i(stream.str().c_str());
      audit = {};
      downstream_capture_t0_views.clear();
      upscaler_input_cb0_ranges.clear();
    }
  }
  if (upscaler_source_writer_audit_state.active) {
    auto& audit = upscaler_source_writer_audit_state;
    ++audit.presents;
    if (audit.source != 0u && audit.presents >= 16u) {
      std::ostringstream stream;
      stream << (audit.target_final_fg_output
                     ? "DL2 DLSS FG final RGB10 producer audit: capture="
                     : "DL2 0x3E source-writer audit: capture=")
             << audit.capture_id
             << " source=0x" << std::hex << audit.source
             << " effective=0x" << audit.effective_source
             << " exposure=0x" << audit.exposure
             << " effective_exposure=0x" << audit.effective_exposure << std::dec
             << " source_desc=format=" << static_cast<uint32_t>(audit.source_info.format)
             << "=>" << static_cast<uint32_t>(audit.source_info.effective_format)
             << " view=" << static_cast<uint32_t>(audit.source_info.view_format)
             << "=>" << static_cast<uint32_t>(audit.source_info.effective_view_format)
             << " size=" << audit.source_info.width << "x" << audit.source_info.height
             << " clone=" << (audit.source_info.view_clone_enabled ? 1 : 0)
             << " latest_tag_original=0x" << std::hex
             << dlss_fg_latest_color_original.load(std::memory_order_relaxed)
             << " latest_tag_clone=0x"
             << dlss_fg_latest_color_clone.load(std::memory_order_relaxed)
             << std::dec
             << " compute_candidates=" << audit.compute_candidate_count;
      stream << " tracked=" << audit.recent_source_count;
      for (uint32_t source_index = 0u; source_index < audit.recent_source_count; ++source_index) {
        stream << " target" << source_index << "=0x" << std::hex
               << audit.recent_sources[source_index] << std::dec;
      }
      for (uint32_t candidate_index = 0u; candidate_index < audit.compute_candidate_count; ++candidate_index) {
        const auto& candidate = audit.compute_candidates[candidate_index];
        const bool matches_target = std::any_of(
            audit.recent_sources.begin(), audit.recent_sources.begin() + audit.recent_source_count,
            [&](uint64_t target) {
              return candidate.resource == target || candidate.effective == target;
            });
        if (matches_target) {
          stream << " match=resource=0x" << std::hex << candidate.resource
                 << "=>0x" << candidate.effective << std::dec
                 << " format=" << static_cast<uint32_t>(candidate.format)
                 << "=>" << static_cast<uint32_t>(candidate.effective_format)
                 << " view=" << static_cast<uint32_t>(candidate.view_format)
                 << "=>" << static_cast<uint32_t>(candidate.effective_view_format);
        }
      }
      stream << " presents=" << audit.presents << " count=" << audit.count;
      for (uint32_t index = 0u; index < audit.count; ++index) {
        const auto& writer = audit.writers[index];
        const char* type = writer.type == UpscalerSourceWriterType::draw                  ? "draw"
                           : writer.type == UpscalerSourceWriterType::dispatch            ? "dispatch"
                           : writer.type == UpscalerSourceWriterType::copy_resource       ? "CopyResource"
                           : writer.type == UpscalerSourceWriterType::copy_texture_region ? "CopyTexture"
                                                                                          : "ResolveTexture";
        stream << " #" << (index + 1u) << " present=" << writer.present_index
               << " " << type;
        if (writer.shader_hash != 0u) {
          stream << " shader=0x" << std::hex << std::uppercase << writer.shader_hash << std::dec;
        }
        if (writer.command_buffer != 0u) {
          stream << " cmd=0x" << std::hex << writer.command_buffer << std::dec;
        }
        if (writer.source != 0u) stream << " source=0x" << std::hex << writer.source << std::dec;
        if (writer.target != 0u) stream << " target=0x" << std::hex << writer.target << std::dec;
      }
      renodx::utils::log::i(stream.str().c_str());
      audit = {};
      upscaler_source_compute_uav_views.clear();
      upscaler_source_compute_srv_views.clear();
    }
  }
  if (upscaler_color_path_audit_state.active) {
    auto& audit = upscaler_color_path_audit_state;
    ++audit.presents;
    if (audit.presents >= 4u) {
      std::ostringstream stream;
      stream << "DL2 upscaler color path audit: capture=" << audit.capture_id
             << " generations=" << audit.start_generation << "=>"
             << dlss_fg_swapchain_generation.load(std::memory_order_relaxed)
             << " presents=" << audit.presents << " count=" << audit.count;
      for (uint32_t index = 0u; index < audit.count; ++index) {
        const auto& entry = audit.entries[index];
        stream << " #" << (index + 1u)
               << " seq=" << entry.sequence
               << " present=" << entry.present_index
               << " api=" << entry.api
               << " gen=" << entry.generation
               << " ps=0x" << std::hex << std::uppercase << entry.shader_hash
               << " input_writer=0x" << entry.input_writer_hash
               << " input_writers=[";
        for (uint32_t writer_index = 0u; writer_index < entry.input_writer_count; ++writer_index) {
          if (writer_index != 0u) stream << ",";
          stream << "0x" << entry.input_writer_hashes[writer_index];
        }
        stream << "] draw=" << std::dec << entry.draw_count << "x" << entry.instance_count
               << " viewport=" << entry.viewport_width << "x" << entry.viewport_height
               << " srv=" << entry.input_table << ":" << entry.input_binding
               << " t0(0x" << entry.input.resource << "," << std::dec << static_cast<uint32_t>(entry.input.format)
               << "=>0x" << std::hex << entry.input.effective << "," << std::dec
               << static_cast<uint32_t>(entry.input.effective_format) << ","
               << entry.input.width << "x" << entry.input.height
               << ",view=" << static_cast<uint32_t>(entry.input.view_format)
               << "=>" << static_cast<uint32_t>(entry.input.effective_view_format)
               << ",clone=" << (entry.input.view_clone_enabled ? 1 : 0)
               << ",creation_index=" << entry.input.creation_index
               << ",upgrade_index=" << entry.input.upgrade_index << ")"
               << " rtv(0x" << std::hex << entry.output.resource << "," << std::dec
               << static_cast<uint32_t>(entry.output.format) << "=>0x" << std::hex
               << entry.output.effective << "," << std::dec
               << static_cast<uint32_t>(entry.output.effective_format) << ","
               << entry.output.width << "x" << entry.output.height
               << ",view=" << static_cast<uint32_t>(entry.output.view_format)
               << "=>" << static_cast<uint32_t>(entry.output.effective_view_format)
               << ",clone=" << (entry.output.view_clone_enabled ? 1 : 0)
               << ",creation_index=" << entry.output.creation_index
               << ",upgrade_index=" << entry.output.upgrade_index << ")"
               << " cmd=0x" << std::hex << entry.command_list
               << " epoch=" << std::dec << entry.command_list_epoch
               << " execute=" << entry.execute_serial
               << " pipeline=0x" << std::hex << entry.bound_pipeline
               << " replacement=0x" << entry.replacement_pipeline
               << " replacement_bound=" << std::dec << (entry.replacement_bound ? 1 : 0);
        if (entry.shader_hash == 0xAD085E81u) {
          const uint64_t input_resource = entry.input.effective != 0u ? entry.input.effective : entry.input.resource;
          for (const auto& observation : upscaler_color_writer_observations) {
            if (observation.resource == input_resource) {
              stream << " bffc_writer_cmd=0x" << std::hex << observation.command_list
                     << " epoch=" << std::dec << observation.command_list_epoch
                     << " execute=" << observation.execute_serial
                     << " bffc_t0=0x" << std::hex << observation.input.resource
                     << "=>0x" << observation.input.effective
                     << " bffc_rtv=0x" << observation.output.resource
                     << "=>0x" << observation.output.effective;
              break;
            }
          }
        }
      }
      renodx::utils::log::i(stream.str().c_str());
      audit = {};
      upscaler_color_last_writers.clear();
      upscaler_color_writer_chains.clear();
      upscaler_color_writer_observations.clear();
      downstream_capture_t0_views.clear();
    }
  }
  if (dlss_fg_tag_transfer_audit_state.active) {
    const auto& audit = dlss_fg_tag_transfer_audit_state;
    std::stringstream stream;
    stream << "DL2 DLSS FG tag transfers: original=0x"
           << std::hex << std::uppercase << audit.original_resource
           << " clone=0x" << audit.clone_resource
           << " count=" << std::dec << audit.count;
    for (uint32_t index = 0u; index < audit.count; ++index) {
      const auto& transfer = audit.transfers[index];
      const char* type = transfer.type == DownstreamTransferType::copy_resource         ? "CopyResource"
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
    stream << "DL2 same-Present command candidates after 0x"
           << std::hex << std::uppercase << capture.anchor_shader_hash
           << std::dec << std::nouppercase << " (" << capture.count << "):";
    for (uint32_t index = 0u; index < capture.count; ++index) {
      stream << " " << (capture.is_compute[index] ? "CS" : "PS") << ":0x"
             << std::hex << std::uppercase << capture.hashes[index];
      const auto& target = capture.targets[index];
      if (target.resource != 0u) {
        stream << "->rtv(0x" << target.resource << ", "
               << static_cast<uint32_t>(target.format) << "=>0x" << target.effective << ", "
               << static_cast<uint32_t>(target.effective_format)
               << ", view=" << static_cast<uint32_t>(target.view_format) << "=>"
               << static_cast<uint32_t>(target.effective_view_format) << ", clone="
               << (target.clone_enabled ? "on" : "off") << ", " << std::dec
               << target.width << "x" << target.height << ")";
      }
      const auto& input = capture.inputs[index];
      if (input.resource != 0u) {
        const bool reads_anchor = input.resource == capture.gamma_target
                                  || input.resource == capture.gamma_target_clone
                                  || input.resource == capture.gamma_target_effective
                                  || input.effective == capture.gamma_target
                                  || input.effective == capture.gamma_target_clone
                                  || input.effective == capture.gamma_target_effective;
        stream << "<-t0(0x" << input.resource << ", " << static_cast<uint32_t>(input.format)
               << "=>0x" << input.effective << ", " << static_cast<uint32_t>(input.effective_format)
               << ", view=" << static_cast<uint32_t>(input.view_format) << "=>"
               << static_cast<uint32_t>(input.effective_view_format)
               << ", anchor=" << (reads_anchor ? "yes" : "no") << ")";
      }
    }
    reshade::log::message(reshade::log::level::info, stream.str().c_str());
  }
  if (capture.capture_transfers) {
    std::stringstream stream;
    stream << "DL2 post-Gamma transfers (" << capture.transfer_count << "):";
    for (uint32_t index = 0u; index < capture.transfer_count; ++index) {
      const auto& transfer = capture.transfers[index];
      const char* type = transfer.type == DownstreamTransferType::copy_resource         ? "CopyResource"
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

bool ActivateDl2HdrTarget(reshade::api::command_list* cmd_list) {
  auto rtvs = renodx::utils::swapchain::GetRenderTargets(cmd_list);
  bool has_active_clone = false;
  for (auto& rtv : rtvs) {
    if (rtv.handle == 0u) continue;
    renodx::mods::swapchain::ActivateCloneHotSwap(cmd_list->get_device(), rtv);
    const auto clone_view = renodx::mods::swapchain::GetResourceViewClone(rtv);
    if (clone_view.handle != 0u) rtv = clone_view;
    renodx::utils::resource::GetLiveResourceViewInfo(
        rtv,
        [&](const renodx::utils::resource::ResourceViewInfo& info) {
          has_active_clone = has_active_clone
                             || info.is_clone
                             || (info.clone_enabled && info.clone.handle != 0u);
        });
  }
  if (has_active_clone) {
    renodx::mods::swapchain::FlushDescriptors(cmd_list);
    renodx::mods::swapchain::RewriteRenderTargets(cmd_list, rtvs.size(), rtvs.data(), {0});
  }
  if (upscaler_color_path_audit_state.active) {
    auto* shader_state = renodx::utils::shader::GetCurrentState(cmd_list);
    if (shader_state != nullptr) {
      auto* pixel_state = renodx::utils::shader::GetCurrentPixelState(shader_state);
      renodx::utils::shader::PopulateStageState(pixel_state);
      if (pixel_state->pipeline_details != nullptr) {
        renodx::utils::log::i(
            "DL2 targeted replacement post-bind: shader=",
            renodx::utils::log::AsHex(
                renodx::utils::shader::GetCurrentShaderHash(
                    shader_state, renodx::utils::shader::PIXEL_INDEX)),
            " bound=", renodx::utils::log::AsPtr(pixel_state->pipeline.handle),
            " replacement=", renodx::utils::log::AsPtr(pixel_state->pipeline_details->replacement_pipeline.handle),
            " is_replacement=", pixel_state->pipeline_details->is_replacement ? 1 : 0);
      }
    }
  }
  return true;
}

renodx::mods::shader::CustomShader CreateDl2HdrShader(
    uint32_t crc32,
    std::span<const uint8_t> dx11_code,
    std::span<const uint8_t> dx12_code) {
  auto shader = renodx::mods::shader::CreateDirectXShader(crc32, dx11_code, dx12_code);
  shader.on_draw = ActivateDl2HdrTarget;
  return shader;
}

bool OnDl2BffcProbeDraw(reshade::api::command_list* cmd_list) {
  ActivateDl2HdrTarget(cmd_list);
  {
    std::scoped_lock lock(downstream_draw_capture_mutex);
    if (!upscaler_color_path_audit_state.active
        || upscaler_color_writer_observations.size() >= 32u) return true;
  }

  auto* state = renodx::utils::shader::GetCurrentState(cmd_list);
  uint64_t native_pipeline = 0u;
  uint64_t replacement_pipeline = 0u;
  bool build_ok = false;
  if (state != nullptr) {
    auto* pixel_state = renodx::utils::shader::GetCurrentPixelState(state);
    renodx::utils::shader::PopulateStageState(pixel_state);
    if (pixel_state->pipeline_details != nullptr) {
      native_pipeline = pixel_state->pipeline.handle;
      build_ok = renodx::utils::shader::BuildReplacementPipeline(pixel_state->pipeline_details);
      replacement_pipeline = pixel_state->pipeline_details->replacement_pipeline.handle;
      if (replacement_pipeline != 0u) {
        cmd_list->bind_pipeline(pixel_state->applied_stage, {replacement_pipeline});
      }
    }
  }
  GammaAuditResource input = {};
  GammaAuditResource output = {};
  auto* device = cmd_list->get_device();
  if (state != nullptr) {
    auto* command_state = renodx::utils::state::GetCurrentState(cmd_list);
    if (command_state != nullptr && !command_state->render_targets.empty()) {
      output = DescribeGammaAuditView(device, command_state->render_targets[0]);
      auto* descriptor_data = renodx::utils::data::Get<renodx::utils::descriptor::DeviceData>(device);
      uint64_t best_area = 0u;
      if (descriptor_data != nullptr) {
        renodx::utils::pipeline_layout::GetPipelineLayoutData(
            command_state->graphics_pipeline_layout,
            [&](const renodx::utils::pipeline_layout::PipelineLayoutData* layout_data) {
              for (uint32_t table_index = 0u;
                   table_index < layout_data->params.size()
                   && table_index < command_state->graphics_descriptor_tables.size();
                   ++table_index) {
                const auto& param = layout_data->params[table_index];
                if (param.type != reshade::api::pipeline_layout_param_type::descriptor_table) continue;
                for (uint32_t range_index = 0u; range_index < param.descriptor_table.count; ++range_index) {
                  const auto& range = param.descriptor_table.ranges[range_index];
                  if (range.type != reshade::api::descriptor_type::sampler_with_resource_view
                      && range.type != reshade::api::descriptor_type::texture_shader_resource_view) continue;
                  const uint32_t count = std::min(range.count, 64u);
                  for (uint32_t offset = 0u; offset < count; ++offset) {
                    const auto table = command_state->graphics_descriptor_tables[table_index];
                    if (table.handle == 0u) continue;
                    reshade::api::descriptor_heap heap = {};
                    uint32_t heap_offset = 0u;
                    device->get_descriptor_heap_offset(table, range.binding + offset, 0u, &heap, &heap_offset);
                    renodx::utils::descriptor::DescriptorHeapSlot slot = {};
                    bool found_slot = false;
                    {
                      const std::shared_lock descriptor_lock(descriptor_data->mutex);
                      const auto heap_it = descriptor_data->heaps.find(heap.handle);
                      if (heap_it != descriptor_data->heaps.end() && heap_offset < heap_it->second.size()) {
                        slot = heap_it->second[heap_offset];
                        found_slot = true;
                      }
                    }
                    if (!found_slot || !slot.HasResourceView()) continue;
                    const auto candidate = DescribeGammaAuditView(device, slot.resource_view);
                    if (candidate.resource == 0u || candidate.resource == output.resource
                        || candidate.width < output.width / 2u || candidate.height < output.height / 2u
                        || candidate.width == 0u || candidate.height == 0u) continue;
                    const float output_aspect = static_cast<float>(output.width) / output.height;
                    const float input_aspect = static_cast<float>(candidate.width) / candidate.height;
                    if (std::abs(input_aspect - output_aspect) > output_aspect * 0.12f) continue;
                    const uint64_t area = static_cast<uint64_t>(candidate.width) * candidate.height;
                    if (area > best_area) {
                      best_area = area;
                      input = candidate;
                    }
                  }
                }
              }
            });
      }
    }
  }
  const uint64_t epoch = upscaler_color_command_epochs[reinterpret_cast<uintptr_t>(cmd_list)];
  std::ostringstream resources;
  resources << "DL2 BFFC replacement resources: cmd=0x" << std::hex
            << reinterpret_cast<uintptr_t>(cmd_list)
            << " epoch=" << std::dec << epoch
            << " t0=0x" << std::hex << input.resource << "=>0x" << input.effective
            << " fmt=" << std::dec << static_cast<uint32_t>(input.format)
            << "=>" << static_cast<uint32_t>(input.effective_format)
            << " view=" << static_cast<uint32_t>(input.view_format)
            << "=>" << static_cast<uint32_t>(input.effective_view_format)
            << " rtv=0x" << std::hex << output.resource << "=>0x" << output.effective
            << " fmt=" << std::dec << static_cast<uint32_t>(output.format)
            << "=>" << static_cast<uint32_t>(output.effective_format)
            << " view=" << static_cast<uint32_t>(output.view_format)
            << "=>" << static_cast<uint32_t>(output.effective_view_format);
  renodx::utils::log::i(resources.str().c_str());
  if (output.resource != 0u && upscaler_color_writer_observations.size() < 128u) {
    upscaler_color_writer_observations.push_back({
        .shader_hash = 0xBFFC45ACu,
        .resource = output.resource,
        .command_list = reinterpret_cast<uintptr_t>(cmd_list),
        .command_list_epoch = epoch,
        .input = input,
        .output = output,
    });
  }
  std::ostringstream stream;
  stream << "DL2 BFFC replacement probe: cmd=0x" << std::hex
         << reinterpret_cast<uintptr_t>(cmd_list)
         << " native=0x" << native_pipeline
         << " replacement=0x" << replacement_pipeline
         << " build=" << std::dec << (build_ok ? 1 : 0)
         << " capture=" << upscaler_color_path_audit_state.capture_id;
  renodx::utils::log::i(stream.str().c_str());
  return true;
}

void OnDl2BffcProbeDrawn(reshade::api::command_list* cmd_list) {
  if (!upscaler_color_path_audit_state.active
      && !upscaler_input_audit_state.active
      && !upscaler_source_writer_audit_state.active) {
    return;
  }
  auto* state = renodx::utils::shader::GetCurrentState(cmd_list);
  std::ostringstream stream;
  stream << "DL2 BFFC replacement probe post-draw: cmd=0x"
         << std::hex << reinterpret_cast<uintptr_t>(cmd_list);
  if (state != nullptr) {
    auto* pixel_state = renodx::utils::shader::GetCurrentPixelState(state);
    stream << " pipeline=0x" << pixel_state->pipeline.handle;
  }
  renodx::utils::log::i(stream.str().c_str());
}

renodx::mods::shader::CustomShader CreateDl2BffcProbeShader() {
  auto shader = renodx::mods::shader::CreateDirectXShader(
      0xBFFC45ACu, __0xBFFC45AC_dx11, __0xBFFC45AC_dx12);
  shader.on_draw = &OnDl2BffcProbeDraw;
  shader.on_drawn = &OnDl2BffcProbeDrawn;
  return shader;
}

renodx::mods::shader::CustomShader CreateDl2UiWriterProbeShader(
    uint32_t crc32,
    std::span<const uint8_t> dx11_code,
    std::span<const uint8_t> dx12_code) {
  auto shader = renodx::mods::shader::CreateDirectXShader(crc32, dx11_code, dx12_code);
  shader.on_draw = ActivateDl2HdrTarget;
  return shader;
}

#define Dl2UiWriterProbeShader(__crc32__)                     \
  {                                                           \
      __crc32__, CreateDl2UiWriterProbeShader(                \
                     __crc32__,                               \
                     RENODX_JOIN_MACRO(__##__crc32__, _dx11), \
                     RENODX_JOIN_MACRO(__##__crc32__, _dx12))}

#define TargetedDl2HdrShader(__crc32__)                       \
  {                                                           \
      __crc32__, CreateDl2HdrShader(                          \
                     __crc32__,                               \
                     RENODX_JOIN_MACRO(__##__crc32__, _dx11), \
                     RENODX_JOIN_MACRO(__##__crc32__, _dx12))}

renodx::mods::shader::CustomShaders custom_shaders = {
    // Primary HDR bridge. Its t0 input was proven to retain scene values above
    // 4.0 (and above 12.0 in outdoor highlights) before the vanilla curve and
    // saturate operation collapse them to SDR.
    TargetedDl2HdrShader(0x3E36DA5B),
    // The native SDR LUT clamps the bridge back to SDR. Its replacement keeps
    // the vanilla grade below SDR white and reconstructs the HDR magnitude in
    // the now-proven Linear BT.709 intermediate domain.
    TargetedDl2HdrShader(0x268BAB6D),
    // DL2 composites gamma-domain UI through an UNORM view of the same
    // typeless target whose scene pass uses an sRGB view. FP16 cloning removes
    // that view distinction, so decode matched UI to Linear BT.709 and apply
    // the dedicated UI-white scale before alpha blending into the HDR scene.
    Dl2UiWriterProbeShader(0x54F3F767),
    Dl2UiWriterProbeShader(0xF34DDC49),
    Dl2UiWriterProbeShader(0x43B22618),
    Dl2UiWriterProbeShader(0x61DBDE91),
    Dl2UiWriterProbeShader(0x2280559E),
    Dl2UiWriterProbeShader(0x7D1BA5D4),
    // Full-screen post-LUT blit. The normal branch is bytecode-equivalent in
    // intent; its debug branch isolates the boundary before UI composition.
    {0xBFFC45ACu, CreateDl2BffcProbeShader()},
    // Keep the original power operation in normal rendering, but register the
    // exact replacement so its bounded post-Gamma probes actually execute.
    // Its draw callback also keeps the FP16 output active at this final game
    // color boundary.
    TargetedDl2HdrShader(0xAD085E81),
    // Disabled: guessed hashes caused crashes because the copied tonemapper
    // template shader has mismatched inputs/outputs.
    // CustomDirectXShaders(0x4d2b3f4d),
    // CustomDirectXShaders(0x8a1c8855),
    // CustomDirectXShaders(0x79b3c079),
    // CustomDirectXShaders(0xa766966e),
};

float current_settings_mode = 0;
float resource_upgrade_test_setting = 0.f;

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
        .key = "ResourceUpgradeTest",
        .binding = &resource_upgrade_test_setting,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Typeless Resource Candidate",
        .section = "Debug",
        .tooltip = "Requires a full game restart. Diagnostic only: selects individual or grouped full-size typeless resource creation indices while retaining the UNORM and sRGB upgrades.",
        .labels = {
            "Exact HDR chain 4 + 5 + 7",
            "Typeless candidate 0 + UNORM/sRGB",
            "Typeless candidate 1 + UNORM/sRGB",
            "Typeless candidate 2 + UNORM/sRGB",
            "Typeless candidate 3 + UNORM/sRGB",
            "Typeless candidate 4 + UNORM/sRGB",
            "Typeless candidate 5 + UNORM/sRGB",
            "Typeless candidate 6 + UNORM/sRGB",
            "Typeless candidate 7 + UNORM/sRGB",
            "UNORM + sRGB only",
            "None (native formats)",
            "Typeless 2 + 3 + UNORM/sRGB",
            "Typeless 2 + 4 + UNORM/sRGB",
            "Typeless 3 + 4 + UNORM/sRGB",
            "Typeless 2 + 3 + 4 + UNORM/sRGB",
            "Typeless 2 + 4 + 6 + UNORM/sRGB",
            "Typeless 2 + 3 + 4 + 6 + UNORM/sRGB",
            "Typeless range 8-15 + UNORM/sRGB",
            "Typeless range 16-23 + UNORM/sRGB",
            "Typeless range 24-31 + UNORM/sRGB",
            "Typeless range 32-47 + UNORM/sRGB",
            "Typeless range 48-63 + UNORM/sRGB",
            "Typeless cumulative 0-7 + UNORM/sRGB",
            "Typeless cumulative 0-15 + UNORM/sRGB",
            "Typeless cumulative 0-31 + UNORM/sRGB",
            "Typeless cumulative 0-63 + UNORM/sRGB",
            "Typeless 4 + 5 + UNORM/sRGB",
            "Typeless 4 + 7 + UNORM/sRGB",
            "Typeless 5 + 7 + UNORM/sRGB",
            "Typeless 4 + 5 + 7 + UNORM/sRGB",
            "All typeless (diagnostic) + UNORM/sRGB",
            "Semantic hot-swap (experimental)",
            "Exact Balanced chain 0 + 1",
            "Exact Balanced + FG chain 2 + 3",
        },
        .is_global = true,
        .is_visible = []() { return current_settings_mode >= 2; },
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
        .key = "DLSSFGColorBufferFormat",
        .binding = &dlss_fg_color_buffer_format_mode,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = false,
        .label = "DLSS FG Declared Final Color Format",
        .section = "Compatibility",
        .tooltip = "Requires a full game restart. Game default preserves DL2's optional zero format; RGB10 retains RenoDX's previous forced HDR10 declaration; RGBA8 is a negative control matching the host swapchain description rather than the upgraded backbuffer.",
        .labels = {"Game default (0)", "HDR10 RGB10 (24)", "Host RGBA8 (28, negative control)"},
        .is_global = true,
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "DLSSFGAuxiliaryColorTags",
        .binding = &dlss_fg_aux_tag_mode,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = false,
        .label = "DLSS FG Auxiliary Color Tags",
        .section = "Compatibility",
        .tooltip = "Live A/B for Streamline's auxiliary UI and HUD-less inputs. Disabled inputs are submitted as explicit null tags while preserving tag order and lifecycle semantics.",
        .labels = {"Original", "UI only", "HUDLess only", "None (Final Color only)"},
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
        .key = "DLSSFGFinalColorMode",
        .binding = &dlss_fg_final_color_mode,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = false,
        .label = "DLSS FG Final Color",
        .section = "Compatibility",
        .tooltip = "Live diagnostics for Streamline's focused-FG RGB10 output. The final mode encodes the exact 0xAD FP16 clone as BT.2100 PQ/RGB10 before Streamline and preserves the native RGB10 final copy; Off/Balanced and the stable resource masks are unchanged.",
        .labels = {
            "Direct PQ",
            "PQ / BT.2020",
            "Linear / BT.709 (203 nit)",
            "Linear / BT.709 (400 nit)",
            "Linear / BT.709 (600 nit)",
            "Linear / BT.709 (800 nit)",
            "Linear / BT.709 (1000 nit)",
            "Linear / BT.709 (1200 nit)",
            "Linear / BT.709 (1600 nit)",
            "Linear / BT.709 (Peak setting)",
            "Linear / BT.709 (4000 nit control)",
            "AD FP16 -> PQ RGB10 Handoff",
        },
        .on_change_value = [](float previous, float current) {
          const int32_t previous_mode = std::clamp(static_cast<int32_t>(previous + 0.5f), 0, 11);
          const int32_t current_mode = std::clamp(static_cast<int32_t>(current + 0.5f), 0, 11);
          if (previous_mode != current_mode) {
            dlss_fg_handoff_epoch.fetch_add(1u, std::memory_order_acq_rel);
            std::scoped_lock lock(dlss_fg_bridge_mutex);
            dlss_fg_bridged_trays.clear();
          }
          dlss_fg_execute_candidate_remaining.store(16u, std::memory_order_release);
          std::ostringstream stream;
          stream << "DL2 DLSS FG final color mode changed: "
                 << previous_mode << "=>" << current_mode
                 << " submission_logs=16";
          renodx::utils::log::i(stream.str().c_str()); },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "DLSSFGCreationFormatFix",
        .binding = &dlss_fg_creation_format_fix,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 1.f,
        .can_reset = false,
        .label = "DLSS FG RGB10 Native Contract (Restart)",
        .section = "Compatibility",
        .tooltip = "DL2-specific Streamline interface fix. When the swapchain reports RGBA8 but its actual RenoDX HDR10 backbuffer is RGB10, GetDesc/GetDesc1 report RGB10 to Streamline before DLSS-G allocates its generated-frame buffers. Requires a game restart.",
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
          return false; },
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
          return false; },
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
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DLSS FG Final Color + Producers (16)",
        .section = "Debug",
        .tooltip = "One-shot read-only capture. Correlates 16 rendered/generated Presents and traces the two final RGB10 resources backward to visible draw, dispatch/UAV, copy, or resolve producers. No resource mutation, readback, or Present timing change.",
        .on_click = []() {
          dlss_fg_identity_event_serial.store(0u, std::memory_order_release);
          dlss_fg_present_identity_count.store(0u, std::memory_order_release);
          dlss_fg_reshade_identity_count.store(0u, std::memory_order_release);
          dlss_fg_exact_ad_ordering_remaining.store(32u, std::memory_order_release);
          dlss_fg_execute_candidate_remaining.store(128u, std::memory_order_release);
          dlss_fg_frame_classification_remaining.store(0u, std::memory_order_release);
          const uint64_t generation =
              dlss_fg_frame_classification_generation.fetch_add(
                  1u, std::memory_order_acq_rel)
              + 1u;
          {
            std::scoped_lock audit_lock(
                dlss_fg_timing_mutex,
                dlss_fg_frame_classification_mutex);
            dlss_fg_timing = {};
            dlss_fg_frame_classification = {
                .generation = generation,
                .last_ad_submission_serial =
                    dlss_fg_ad_submission_serial.load(std::memory_order_acquire),
                .remaining = 16u,
            };
          }
          {
            std::scoped_lock input_lock(dlss_fg_input_snapshot_mutex);
            dlss_fg_input_snapshots.fill({});
          }
          {
            std::scoped_lock draw_lock(downstream_draw_capture_mutex);
            upscaler_source_writer_audit_state = {
                .capture_id = ++upscaler_source_writer_audit_capture_serial,
                .target_final_fg_output = true,
                .active = true,
            };
            upscaler_source_compute_uav_views.clear();
            upscaler_source_compute_srv_views.clear();
          }
          dlss_fg_input_snapshot_evictions.store(0u, std::memory_order_release);
          dlss_fg_frame_classification_remaining.store(16u, std::memory_order_release);
          std::ostringstream armed;
          armed << "DL2 DLSS FG final color correlation armed: generation=" << generation
                << " presents=16 submissions=128 inputs=tagged producers=rgb10";
          renodx::utils::log::i(armed.str().c_str());
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture Exact AD to Streamline Ordering",
        .section = "Debug",
        .tooltip = "One-shot read-only timing capture. Tracks 16 command-list submissions containing the final 0xAD085E81 draw and correlates them with Streamline Present1 and RenoDX post-proxy Present events.",
        .on_click = []() {
          dlss_fg_identity_event_serial.store(0u, std::memory_order_release);
          dlss_fg_present_identity_count.store(0u, std::memory_order_release);
          dlss_fg_reshade_identity_count.store(0u, std::memory_order_release);
          dlss_fg_exact_ad_ordering_remaining.store(16u, std::memory_order_release);
          {
            std::scoped_lock timing_lock(dlss_fg_timing_mutex);
            dlss_fg_timing = {};
          }
          renodx::utils::log::i("DL2 DLSS FG exact AD ordering audit armed: submissions=16");
          return false; },
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
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DLSS FG Streamline Tags",
        .section = "Debug",
        .tooltip = "One-shot: logs the next Streamline slSetTag/slSetTagForFrame resource tags, including native handles and RenoDX clone state. No dumping, readback, or resource interception.",
        .on_click = []() {
          dlss_fg_tag_capture = true;
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Arm Streamline Present1 Trace (256)",
        .section = "Debug",
        .tooltip = "Creates the one-shot marker consumed by the diagnostic Streamline interposer. It records the next 256 Present1 calls in sl.log and does not change rendering, colors, or queue timing. sl.interposer.json must already enable the log path.",
        .on_click = []() {
          ArmStreamlinePresentTrace();
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DLSS FG Present Cadence (16)",
        .section = "Debug",
        .tooltip = "One-shot: records 16 Present events as color-tag serial plus current backbuffer handle. It does not capture draws, descriptors, resources, or GPU data.",
        .on_click = []() {
          dlss_fg_present_cadence_capture = true;
          return false; },
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
        .tooltip = "Legacy diagnostic. Explicitly clears DL2's pre-PQ HUDLessColor and UIColorAndAlpha tags so DLSS-G uses the automatically intercepted final HDR10/PQ color. This overrides the auxiliary color tag selector with None.",
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture DLSS FG Backbuffer Barriers (128)",
        .section = "Debug",
        .tooltip = "One-shot: records up to 128 barriers for full-size swapchain backbuffers and clones, including old/new states and the current Streamline tag serial. No mutation or readback.",
        .on_click = []() {
          dlss_fg_backbuffer_barrier_capture.store(128u, std::memory_order_release);
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture 268 Resource Lifecycle (4 Presents)",
        .section = "Debug",
        .tooltip = "One-shot read-only capture. Arms the 268 post-LUT target snapshot and the four-Present 3E/268/AD path audit together, including clone/effective RTV views, downstream readers, copies, formats, and rotating resources.",
        .on_click = []() {
          std::scoped_lock lock(downstream_draw_capture_mutex);
          downstream_draw_capture = 1.f;
          downstream_transfer_capture = 1.f;
          downstream_draw_capture_state = {};
          downstream_capture_t0_views.clear();

          const uint64_t capture_id = ++upscaler_color_path_capture_serial;
          const uint64_t generation = dlss_fg_swapchain_generation.load(std::memory_order_acquire);
          upscaler_color_path_audit_state = {
              .capture_id = capture_id,
              .start_generation = generation,
              .active = true,
          };
          upscaler_color_last_writers.clear();
          upscaler_color_writer_chains.clear();
          upscaler_color_writer_observations.clear();
          std::ostringstream stream;
          stream << "DL2 268 resource lifecycle audit armed: capture=" << capture_id
                 << " generation=" << generation;
          renodx::utils::log::i(stream.str().c_str());
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture Targeted DLSS Color Path (4 Presents)",
        .section = "Debug",
        .tooltip = "One-shot: records only DL2's known Tonemapper (0x3E36DA5B), LUT (0x268BAB6D), and Gamma (0xAD085E81) passes for four Presents, including execution order, t0/RTV resources, formats, dimensions, clone state, API, and swapchain generation. No readback or resource mutation.",
        .on_click = []() {
          std::scoped_lock lock(downstream_draw_capture_mutex);
          const uint64_t capture_id = ++upscaler_color_path_capture_serial;
          const uint64_t generation = dlss_fg_swapchain_generation.load(std::memory_order_acquire);
          upscaler_color_path_audit_state = {
              .capture_id = capture_id,
              .start_generation = generation,
              .active = true,
          };
          upscaler_color_last_writers.clear();
          upscaler_color_writer_chains.clear();
          upscaler_color_writer_observations.clear();
          downstream_capture_t0_views.clear();
          std::ostringstream stream;
          stream << "DL2 upscaler color path audit armed: capture=" << capture_id
                 << " generation=" << generation;
          renodx::utils::log::i(stream.str().c_str());
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture 0x3E Inputs and Curve (4 Draws)",
        .section = "Debug",
        .tooltip = "One-shot: records four Tonemapper draws (waiting up to 32 Presents), including t0 scene source, t1 auto-exposure source, and b0 SDR-curve binding. Where the game's constant upload is cached, it also prints the five curve floats referenced by the original shader. No texture readback, resource mutation, or timing change.",
        .on_click = []() {
          std::scoped_lock lock(downstream_draw_capture_mutex);
          const uint64_t capture_id = ++upscaler_input_audit_capture_serial;
          const uint64_t generation = dlss_fg_swapchain_generation.load(std::memory_order_acquire);
          upscaler_input_audit_state = {
              .capture_id = capture_id,
              .start_generation = generation,
              .active = true,
          };
          downstream_capture_t0_views.clear();
          upscaler_input_cb0_ranges.clear();
          std::ostringstream stream;
          stream << "DL2 0x3E input/curve audit armed: capture=" << capture_id
                 << " generation=" << generation;
          renodx::utils::log::i(stream.str().c_str());
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture 0x3E Source Writers (16 Presents)",
        .section = "Debug",
        .tooltip = "One-shot: identifies the scene and 1x1 exposure resources sampled by the Tonemapper, then records pixel, compute/UAV, copy, or resolve writers during the next 16 Presents. No resource mutation or readback.",
        .on_click = []() {
          std::scoped_lock lock(downstream_draw_capture_mutex);
          const uint64_t capture_id = ++upscaler_source_writer_audit_capture_serial;
          upscaler_source_writer_audit_state = {
              .capture_id = capture_id,
              .active = true,
          };
          upscaler_source_compute_uav_views.clear();
          upscaler_source_compute_srv_views.clear();
          std::ostringstream stream;
          stream << "DL2 0x3E source-writer audit armed: capture=" << capture_id;
          renodx::utils::log::i(stream.str().c_str());
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "LuminanceStageProbe",
        .binding = &shader_injection.luminance_stage_probe,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = false,
        .label = "Luminance Stage Probe",
        .section = "Debug",
        .tooltip = "Ordered HDR path probe. The selected stage writes the same 51/203/812/3248-nit ladder at the lower right and a unique stage marker at the upper left. Unselected shaders render normally.",
        .labels = {"Off", "1. 3E Scene Output", "2. 268 LUT Output", "3. AD Gamma Input (Bypass Gamma)", "4. AD Gamma Output", "5. BFFC Blit Output", "6. Final Proxy Output"},
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "DebugMode",
        .binding = &shader_injection.debug_mode,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = false,
        .label = "Legacy Debug Mode",
        .section = "Debug",
        .tooltip = "False-color visualization and output probes. Luminance Ladder places four known scene values in the lower-right corner.",
        .labels = {"Off", "HDR Input Range", "Neutral SDR", "Graded SDR", "RenoDRT Output", "Output Probe (500-nit red)", "Scene Probe (Peak white)", "Output Luminance Ladder", "Raw Output Ladder", "Late LUT Output Ladder", "Source t0 Range", "Auto Exposure t1", "Bypass Late Gamma (Test)", "LUT Output Constant (500-nit white)", "Gamma Output Constant (500-nit white)", "Final Proxy Constant (500-nit white)", "Gamma Input t0 Range", "Gamma Power cb0", "Stability Probe (4 stages; top bypass, bottom Gamma)", "Source t0 Chroma", "Vanilla SDR Chroma", "RenoDRT Output Chroma", "Vanilla SDR Direct", "RenoDRT Output Direct", "Proxy No Gamut Compression", "Proxy Decode Grid (709/2020 x Linear/sRGB/2.2)", "0x3E Legacy sRGB Output (A/B)", "Post-LUT Blit Constant (6.25x white)", "DLSS Off Grid at 0x3E", "DLSS Off Grid after LUT", "DLSS Off Grid after Gamma", "DLSS Off Grid at Proxy", "DLSS Off Input Semantics (TL linear, TR sRGB decode, BL gamma 2.2, BR half decode)", "DLSS Off 0x268 Partial Decode (TL 0%, TR 25%, BL 50%, BR 75%)", "DLSS Off 0x3E Bridge Grid (TL source, TR vanilla, BL scaled HDR, BR unscaled HDR)", "DLSS Off 0x268 LUT Grid (TL native, TR upgraded, BL stable, BR current)", "FG Final Proxy Source Range (4 quadrants)"},
        .on_change_value = [](float previous, float current) {
          const int32_t previous_mode = static_cast<int32_t>(previous + 0.5f);
          const int32_t current_mode = static_cast<int32_t>(current + 0.5f);
          if (previous_mode != 36 && current_mode != 36) return;
          dlss_fg_execute_candidate_remaining.store(32u, std::memory_order_release);
          std::ostringstream stream;
          stream << "DL2 FG final proxy source-range probe: "
                 << previous_mode << "=>" << current_mode
                 << " submission_logs=32";
          renodx::utils::log::i(stream.str().c_str()); },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture Post-LUT Candidates",
        .section = "Debug",
        .tooltip = "One-shot, records full-size pixel composites after the scene LUT (0x268BAB6D) until the next Present. Logs D3D12 descriptor-table t0 inputs and original/effective RTV formats. No mutation, readback, or dumping.",
        .on_click = []() {
          std::scoped_lock lock(downstream_draw_capture_mutex);
          downstream_draw_capture = 1.f;
          downstream_draw_capture_state = {};
          downstream_capture_t0_views.clear();
          renodx::utils::log::i("DL2 post-LUT candidate capture armed.");
          return false; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Capture Post-LUT Transfers",
        .section = "Debug",
        .tooltip = "One-shot, records up to 16 unique copy or resolve operations after the scene LUT (0x268BAB6D) until the next Present. It records only resource handles and formats, with no readback or interception.",
        .on_click = []() {
          std::scoped_lock lock(downstream_draw_capture_mutex);
          downstream_transfer_capture = 1.f;
          downstream_draw_capture_state = {};
          renodx::utils::log::i("DL2 post-LUT transfer capture armed.");
          return false; },
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
          return false; },
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
          return false; },
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
      {"DLSSFGColorBufferFormat", 0.f},
      {"DLSSFGCreationFormatFix", 1.f},
      {"DLSSFGAuxiliaryColorTags", 0.f},
      {"DLSSFGUseTaggedClone", 0.f},
      {"DLSSFGSuppressPrePQTags", 0.f},
      {"DLSSFGSkipGeneratedProxy", 0.f},
      {"DLSSFGBypassAllProxy", 0.f},
      {"DebugMode", 0.f},
      {"LuminanceStageProbe", 0.f},
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

    auto* native_device = reinterpret_cast<ID3D12Device*>(
        static_cast<uintptr_t>(device->get_native()));
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support = {};
    format_support.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    const HRESULT support_hr = native_device != nullptr
                                   ? native_device->CheckFeatureSupport(
                                         D3D12_FEATURE_FORMAT_SUPPORT,
                                         &format_support,
                                         sizeof(format_support))
                                   : E_POINTER;
    const bool typed_uav = SUCCEEDED(support_hr)
                           && (format_support.Support1
                               & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)
                                  != 0
                           && (format_support.Support2
                               & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)
                                  != 0;
    dlss_fg_rgb10_uav_supported.store(typed_uav, std::memory_order_release);
    std::ostringstream support_message;
    support_message << "DL2 DLSS FG RGB10 compute bridge: supported=" << (typed_uav ? 1 : 0)
                    << " hr=0x" << std::hex << static_cast<uint32_t>(support_hr)
                    << " support1=0x" << format_support.Support1
                    << " support2=0x" << format_support.Support2;
    renodx::utils::log::i(support_message.str().c_str());
  }

  TryInstallStreamlineHook();
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Dying Light 2";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH: {
      addon_module = h_module;
      if (!reshade::register_addon(h_module)) return FALSE;

      // This bounded diagnostic needs the D3D12 descriptor heap and command
      // state mirrors to identify fullscreen color inputs across DLSS modes.
      // The existing constant cache lets the 0x3E audit compare the original
      // two-float4 SDR curve without mapping or reading GPU textures.
      renodx::utils::descriptor::trace_descriptor_tables = true;
      renodx::utils::constants::capture_constant_buffers = true;
      renodx::utils::descriptor::Use(fdw_reason);
      renodx::utils::constants::Use(fdw_reason);
      renodx::utils::state::Use(fdw_reason);

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
      renodx::utils::command_action::Register(
          renodx::games::dyinglight2::descriptor_override::OnTargetOutputDraw,
          {.shader_hash = 0x3E36DA5Bu,
           .command_types = renodx::utils::command_action::COMMAND_TYPE_DIRECT_DRAW});
      renodx::utils::command_action::Register(
          renodx::games::dyinglight2::descriptor_override::OnTargetDraw,
          {.shader_hash = 0x268BAB6Du,
           .command_types = renodx::utils::command_action::COMMAND_TYPE_DIRECT_DRAW});
      renodx::utils::command_action::Register(
          renodx::games::dyinglight2::descriptor_override::OnTargetDraw,
          {.shader_hash = 0xAD085E81u,
           .command_types = renodx::utils::command_action::COMMAND_TYPE_DIRECT_DRAW});
      renodx::utils::log::i("DL2 build: ", renodx::build_info::kBuildVersion);
      renodx::utils::log::i("DL2 scoped clone diagnostic: output-audit-v1");
      reshade::register_event<reshade::addon_event::copy_resource>(OnDownstreamCopyResource);
      reshade::register_event<reshade::addon_event::create_pipeline>(OnCreateDl2UiPipeline);
      reshade::register_event<reshade::addon_event::init_command_queue>(RegisterDlssFgNativeQueue);
      reshade::register_event<reshade::addon_event::destroy_command_queue>(UnregisterDlssFgNativeQueue);
      reshade::register_event<reshade::addon_event::init_swapchain>(OnTypelessAuditInitSwapchain);
      reshade::register_event<reshade::addon_event::init_resource>(OnTypelessAuditInitResource);
      reshade::register_event<reshade::addon_event::destroy_resource>(OnTypelessAuditDestroyResource);
      reshade::register_event<reshade::addon_event::copy_texture_region>(OnDownstreamCopyTextureRegion);
      reshade::register_event<reshade::addon_event::resolve_texture_region>(OnDownstreamResolveTextureRegion);
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnDownstreamBindRenderTargets);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnGammaAuditPushDescriptors);
      reshade::register_event<reshade::addon_event::map_buffer_region>(OnUpscalerMapBufferRegion);
      reshade::register_event<reshade::addon_event::unmap_buffer_region>(OnUpscalerUnmapBufferRegion);
      reshade::register_event<reshade::addon_event::reset_command_list>(OnDlssFgResetCommandList);
      reshade::register_event<reshade::addon_event::execute_command_list>(OnDlssFgExecuteCommandList);
      // Registered before mods::swapchain::Use below so the DLSS-G input
      // fence is observed before proxy clones are released during resize.
      reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::register_event<reshade::addon_event::destroy_device>(
          renodx::games::dyinglight2::descriptor_override::OnDestroyDevice);

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
      renodx::mods::swapchain::proxy_source_is_hdr10_index =
          offsetof(ShaderInjectData, renodrt_padding_2) / sizeof(float);

      int32_t resource_upgrade_test = 0;
      reshade::get_config_value(
          nullptr,
          renodx::utils::settings::global_name.c_str(),
          "ResourceUpgradeTest",
          resource_upgrade_test);
      if (resource_upgrade_test < 0 || resource_upgrade_test > 33) {
        resource_upgrade_test = 0;
      }
      resource_upgrade_test_setting = static_cast<float>(resource_upgrade_test);
      const bool upgrade_all_typeless = resource_upgrade_test == 30;
      const bool semantic_typeless_hot_swap = resource_upgrade_test == 31;
      uint64_t typeless_candidate_mask = 0u;
      if (resource_upgrade_test == 0) {
        typeless_candidate_mask = (uint64_t{1} << 4) | (uint64_t{1} << 5) | (uint64_t{1} << 7);
      } else if (resource_upgrade_test >= 1 && resource_upgrade_test <= 8) {
        typeless_candidate_mask = uint64_t{1} << (resource_upgrade_test - 1);
      } else if (resource_upgrade_test == 11) {
        typeless_candidate_mask = (uint64_t{1} << 2) | (uint64_t{1} << 3);
      } else if (resource_upgrade_test == 12) {
        typeless_candidate_mask = (uint64_t{1} << 2) | (uint64_t{1} << 4);
      } else if (resource_upgrade_test == 13) {
        typeless_candidate_mask = (uint64_t{1} << 3) | (uint64_t{1} << 4);
      } else if (resource_upgrade_test == 14) {
        typeless_candidate_mask = (uint64_t{1} << 2) | (uint64_t{1} << 3) | (uint64_t{1} << 4);
      } else if (resource_upgrade_test == 15) {
        typeless_candidate_mask = (uint64_t{1} << 2) | (uint64_t{1} << 4) | (uint64_t{1} << 6);
      } else if (resource_upgrade_test == 16) {
        typeless_candidate_mask = (uint64_t{1} << 2) | (uint64_t{1} << 3) | (uint64_t{1} << 4) | (uint64_t{1} << 6);
      } else if (resource_upgrade_test >= 17 && resource_upgrade_test <= 21) {
        static constexpr uint32_t RANGE_STARTS[] = {8, 16, 24, 32, 48};
        static constexpr uint32_t RANGE_ENDS[] = {15, 23, 31, 47, 63};
        const uint32_t range = static_cast<uint32_t>(resource_upgrade_test - 17);
        for (uint32_t index = RANGE_STARTS[range]; index <= RANGE_ENDS[range]; ++index) {
          typeless_candidate_mask |= uint64_t{1} << index;
        }
      } else if (resource_upgrade_test >= 22 && resource_upgrade_test <= 25) {
        static constexpr uint32_t RANGE_ENDS[] = {7, 15, 31, 63};
        const uint32_t end = RANGE_ENDS[resource_upgrade_test - 22];
        typeless_candidate_mask = end == 63 ? ~uint64_t{0} : ((uint64_t{1} << (end + 1)) - 1);
      } else if (resource_upgrade_test == 26) {
        typeless_candidate_mask = (uint64_t{1} << 4) | (uint64_t{1} << 5);
      } else if (resource_upgrade_test == 27) {
        typeless_candidate_mask = (uint64_t{1} << 4) | (uint64_t{1} << 7);
      } else if (resource_upgrade_test == 28) {
        typeless_candidate_mask = (uint64_t{1} << 5) | (uint64_t{1} << 7);
      } else if (resource_upgrade_test == 29) {
        typeless_candidate_mask = (uint64_t{1} << 4) | (uint64_t{1} << 5) | (uint64_t{1} << 7);
      } else if (resource_upgrade_test == 32) {
        typeless_candidate_mask = (uint64_t{1} << 0) | (uint64_t{1} << 1);
      } else if (resource_upgrade_test == 33) {
        typeless_candidate_mask = (uint64_t{1} << 2) | (uint64_t{1} << 3);
      }
      const bool enable_typeless_upgrade = upgrade_all_typeless
                                           || semantic_typeless_hot_swap
                                           || typeless_candidate_mask != 0u;
      const bool enable_unorm_upgrades = resource_upgrade_test != 10;
      renodx::utils::log::i(
          "DL2 typeless candidate test: mode=",
          resource_upgrade_test,
          " mask=0x",
          std::hex,
          typeless_candidate_mask,
          std::dec,
          " all=",
          upgrade_all_typeless,
          " semantic_hot_swap=",
          semantic_typeless_hot_swap,
          " typeless=",
          enable_typeless_upgrade,
          " unorm_srgb=",
          enable_unorm_upgrades);

      // The 0x3E -> 0x268 -> 0xAD chain uses full-size R8G8B8A8_TYPELESS
      // resources. Without their FP16 clones, values above 1.0 are clipped
      // immediately after the HDR bridge and the final output is capped near
      // the 203-nit reference white.
      if (enable_typeless_upgrade) {
        if (upgrade_all_typeless || semantic_typeless_hot_swap) {
          renodx::mods::swapchain::resource_upgrade_infos.push_back({
              .old_format = reshade::api::format::r8g8b8a8_typeless,
              .new_format = semantic_typeless_hot_swap
                                ? reshade::api::format::r16g16b16a16_typeless
                                : reshade::api::format::r16g16b16a16_float,
              .ignore_size = false,
              .use_resource_view_cloning = true,
              .use_resource_view_hot_swap = semantic_typeless_hot_swap,
              .aspect_ratio = renodx::mods::swapchain::SwapChainUpgradeTarget::ANY,
              .usage_include = reshade::api::resource_usage::render_target,
          });
        } else {
          for (int32_t candidate_index = 0; candidate_index < 64; ++candidate_index) {
            if ((typeless_candidate_mask & (uint64_t{1} << candidate_index)) == 0u) continue;
            renodx::mods::swapchain::resource_upgrade_infos.push_back({
                .old_format = reshade::api::format::r8g8b8a8_typeless,
                .new_format = reshade::api::format::r16g16b16a16_float,
                .index = candidate_index,
                .ignore_size = false,
                .use_resource_view_cloning = true,
                .use_resource_view_hot_swap = false,
                .aspect_ratio = renodx::mods::swapchain::SwapChainUpgradeTarget::ANY,
                .usage_include = reshade::api::resource_usage::render_target,
            });
          }
        }
      }

      // Preserve range for the general UNORM scene targets. The typeless
      // and sRGB variants below carry the same HDR composite through later
      // native passes.
      if (enable_unorm_upgrades) {
        renodx::mods::swapchain::resource_upgrade_infos.push_back({
            .old_format = reshade::api::format::r8g8b8a8_unorm,
            .new_format = reshade::api::format::r16g16b16a16_float,
            .ignore_size = false,
            .use_resource_view_cloning = true,
            .aspect_ratio = renodx::mods::swapchain::SwapChainUpgradeTarget::ANY,
            .usage_include = reshade::api::resource_usage::render_target,
        });

        // Preserve the sRGB composite's HDR headroom as well. Removing this rule
        // caps the proxy input near 1.0 (about the 203-nit reference white). The
        // typeless rule above, not this sRGB rule, caused the DLSS mode split.
        renodx::mods::swapchain::resource_upgrade_infos.push_back({
            .old_format = reshade::api::format::r8g8b8a8_unorm_srgb,
            .new_format = reshade::api::format::r16g16b16a16_float,
            .ignore_size = false,
            .use_resource_view_cloning = true,
            .use_resource_view_hot_swap = true,
            .aspect_ratio = renodx::mods::swapchain::SwapChainUpgradeTarget::ANY,
            .usage_include = reshade::api::resource_usage::render_target,
        });
      }

      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);

      break;
    }
    case DLL_PROCESS_DETACH:
      renodx::utils::command_action::Unregister(OnDownstreamDrawCapture);
      renodx::utils::command_action::Unregister(OnGammaDrawAudit);
      renodx::utils::command_action::Unregister(
          renodx::games::dyinglight2::descriptor_override::OnTargetOutputDraw);
      renodx::utils::command_action::Unregister(
          renodx::games::dyinglight2::descriptor_override::OnTargetDraw);
      reshade::unregister_event<reshade::addon_event::present>(OnDownstreamDrawCapturePresent);
      reshade::unregister_event<reshade::addon_event::barrier>(OnDlssFgBackbufferBarrier);
      reshade::unregister_event<reshade::addon_event::copy_resource>(OnDownstreamCopyResource);
      reshade::unregister_event<reshade::addon_event::create_pipeline>(OnCreateDl2UiPipeline);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnTypelessAuditInitSwapchain);
      reshade::unregister_event<reshade::addon_event::init_resource>(OnTypelessAuditInitResource);
      reshade::unregister_event<reshade::addon_event::destroy_resource>(OnTypelessAuditDestroyResource);
      reshade::unregister_event<reshade::addon_event::copy_texture_region>(OnDownstreamCopyTextureRegion);
      reshade::unregister_event<reshade::addon_event::resolve_texture_region>(OnDownstreamResolveTextureRegion);
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnDownstreamBindRenderTargets);
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnGammaAuditPushDescriptors);
      reshade::unregister_event<reshade::addon_event::map_buffer_region>(OnUpscalerMapBufferRegion);
      reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(OnUpscalerUnmapBufferRegion);
      reshade::unregister_event<reshade::addon_event::reset_command_list>(OnDlssFgResetCommandList);
      reshade::unregister_event<reshade::addon_event::execute_command_list>(OnDlssFgExecuteCommandList);
      reshade::unregister_event<reshade::addon_event::init_command_queue>(RegisterDlssFgNativeQueue);
      reshade::unregister_event<reshade::addon_event::destroy_command_queue>(UnregisterDlssFgNativeQueue);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::unregister_event<reshade::addon_event::destroy_device>(
          renodx::games::dyinglight2::descriptor_override::OnDestroyDevice);
      RemoveDlssFgNativeExecuteHook();
      RemoveStreamlineHook();
      renodx::utils::state::Use(fdw_reason);
      renodx::utils::constants::Use(fdw_reason);
      renodx::utils::descriptor::Use(fdw_reason);
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
