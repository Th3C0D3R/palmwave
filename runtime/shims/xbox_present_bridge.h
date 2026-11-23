// The PresentX bridge: real presentation state for one Xbox device proxy over this layer's Vulkan device.
//
// It owns a top-level window titled "Palmwave Game Launcher" (created and pumped on its own thread), a WindowPresenter
// on the REAL device behind the proxy, and an XboxPresentation (xbox_present.h) whose FramePacer serves both the
// device's frame events (slots 78/79/80) and queue PresentX. It is installed on the proxy through the opaque
// presentation hooks of d3d12_xs_device.h, so the proxy (and the CPU-only xs_proxy_mock) never link Vulkan.
//
// What reaches the presenter is only the strict subset of xbox_present_wire.h (one SDR plane, one resource, no
// placement, no extended descriptors, null or all-zero parameters), plus a blank frame for zero planes. Every other
// value is refused before the presenter is touched, with a log line and a counted refusal; nothing is coerced.
// Before the blit the bridge waits, bounded, for the work already submitted to the calling queue (a temporary fence
// on that queue; no vkDeviceWaitIdle), because the presenter submits on the device's own presentation VkQueue.
// After the blit it waits for that submission's fence before S_OK, so the caller can reuse the source on another queue.
// A minimized window returns DXGI_STATUS_OCCLUDED without incrementing the frame count; a closed window is cancelled.
//
// Frame events accepted: origin (0) and indexed event 2, with a null ancillary list and flags 0; SetFrameIntervalX with
// flags 0. Anything else is refused (E_NOTIMPL), never answered with an empty S_OK. WaitFrameEventX follows the pacer:
// HRESULT_FROM_WIN32(ERROR_INVALID_STATE) until the interval and requested event are registered. Event 2 requires
// an existing, retained token and waits on its frame grid without issuing or consuming an origin token.
//
// Ownership: proxy -> bridge state -> {XboxPresentation -> WindowPresenter -> real device, window thread}. The state
// holds no reference to the proxy or to any queue/fence; temporary objects live only during one call. The proxy
// destroys the state on its last Release, before it releases the real device.
#pragma once

#include "d3d12_com.h"
#include "xbox_present.h"

#include <cstdint>
#include <vector>

namespace rt::d3d12 {

struct PresentBridgeOptions {
    palmwave::FrameClock* clock = nullptr;  // null: real time. Tests pass a ManualFrameClock that outlives the device.
    bool visible = true;                    // show the window (SW_SHOWNOACTIVATE)
    bool capture = false;                   // VulkanPresenter capture for read-back (tests); failing it fails install
    int client_width = 1280;
    int client_height = 720;
    DWORD queue_wait_ms = 5000;             // bound for each calling-queue and presentation-fence wait
};

// Installs the bridge on `device` (the Xbox device proxy, or its DXGI pointer, over a device of this layer).
// MUST run before rt::d3d12::set_device_outer for that device: otherwise the presenter would hold the proxy and form a
// cycle, so that case is refused (E_UNEXPECTED). Failures, each with a log line and nothing installed:
// E_INVALIDARG (not a proxy over this layer's device), DXGI_ERROR_UNSUPPORTED (the device was created without
// presentation, or no presenter for the window), E_FAIL (window or thread creation, capture), E_UNEXPECTED (already
// installed, or the outer already set).
HRESULT install_present_bridge(void* device, const PresentBridgeOptions& options = {});

// Test and diagnostic accessors (null / 0 when `device` has no bridge). No reference is added.
XboxPresentation* present_bridge_presentation(const void* device);
HWND present_bridge_window(const void* device);
struct PresentBridgeCounts {
    std::uint64_t presented = 0;   // completed, actually submitted presents (blank included)
    std::uint64_t blank = 0;       // of those, blank frames
    std::uint64_t refused = 0;     // refused by the wire check, resource/queue validation or a frame-event check
    std::uint64_t failed = 0;      // validated, but the queue wait or the presenter failed
};
PresentBridgeCounts present_bridge_counts(const void* device);

}  // namespace rt::d3d12
