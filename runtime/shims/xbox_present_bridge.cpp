// The PresentX bridge (xbox_present_bridge.h). Independently written for Palmwave; the plane/parameter contract it
// enforces is documented with its public sources in xbox_present_wire.h.
#include "xbox_present_bridge.h"

#include "d3d12_xs_device.h"
#include "xbox_present_wire.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <new>
#include <thread>

namespace rt::d3d12 {
namespace {

constexpr wchar_t kWindowClass[] = L"PalmwaveGameLauncherWindow";
constexpr wchar_t kWindowTitle[] = L"Palmwave Game Launcher";
constexpr wchar_t kClosedProperty[] = L"PalmwavePresentClosed";
constexpr UINT kStopMessage = WM_APP + 0x50;  // posted by WindowThread::stop: the only path that destroys the window
constexpr std::uint64_t kLoggedRefusals = 16;  // per bridge; the rest are only counted

template <class T>
struct Hold {  // one reference for the duration of a call
    T* p = nullptr;
    Hold() = default;
    explicit Hold(T* object) : p(object) {
        if (p) p->AddRef();
    }
    ~Hold() {
        if (p) p->Release();
    }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};

LRESULT CALLBACK bridge_window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CLOSE:
        // Hide immediately, cancel subsequent/blocked presents, and keep the HWND alive until the surface is gone.
        SetPropW(window, kClosedProperty, reinterpret_cast<HANDLE>(1));
        ShowWindow(window, SW_HIDE);
        return 0;
    case kStopMessage:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w, l);
    }
}

HINSTANCE this_module() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&bridge_window_proc), &module);
    return module ? module : GetModuleHandleW(nullptr);
}

// A top-level window created, owned and pumped by its own thread, so it keeps answering while the title's threads
// block in WaitFrameEventX or a present.
class WindowThread {
public:
    WindowThread() = default;
    WindowThread(const WindowThread&) = delete;
    WindowThread& operator=(const WindowThread&) = delete;
    ~WindowThread() { stop(); }

    bool start(int client_width, int client_height, bool visible) {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) return false;
        try {
            thread_ = std::thread([this, ready, client_width, client_height, visible] {
                run(ready, client_width, client_height, visible);
            });
        } catch (...) {
            CloseHandle(ready);
            return false;
        }
        const DWORD waited = WaitForSingleObject(ready, INFINITE);
        CloseHandle(ready);
        if (waited != WAIT_OBJECT_0 || !window_) {
            stop();
            return false;
        }
        return true;
    }

    // Destroys the window on its own thread and joins it. Safe to call twice.
    void stop() {
        if (!thread_.joinable()) return;
        if (!window_ || !PostMessageW(window_, kStopMessage, 0, 0))
            PostThreadMessageW(thread_id_.load(), WM_QUIT, 0, 0);
        thread_.join();
        window_ = nullptr;
    }

    HWND window() const { return window_; }
    bool closed() const { return window_ && GetPropW(window_, kClosedProperty) != nullptr; }

private:
    void run(HANDLE ready, int client_width, int client_height, bool visible) {
        thread_id_ = GetCurrentThreadId();
        const HINSTANCE instance = this_module();
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = bridge_window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kWindowClass;
        window_class.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            std::fprintf(stderr, "[rt] present bridge: RegisterClassExW failed (%lu)\n", GetLastError());
            SetEvent(ready);
            return;
        }
        RECT frame{0, 0, client_width, client_height};
        AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
        HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                      frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance,
                                      nullptr);
        if (!window) {
            std::fprintf(stderr, "[rt] present bridge: CreateWindowExW failed (%lu)\n", GetLastError());
            SetEvent(ready);
            return;
        }
        if (visible) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            UpdateWindow(window);
        }
        window_ = window;
        SetEvent(ready);  // `ready` belongs to start() from here on
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    std::thread thread_;
    std::atomic<DWORD> thread_id_{0};
    HWND window_ = nullptr;
};

struct BridgeState {
    // Declaration order is destruction order reversed: the presentation (presenter, surface, swapchain) goes first,
    // the window after it.
    WindowThread window;
    std::unique_ptr<XboxPresentation> presentation;
    ID3D12Device* real = nullptr;  // no reference: the proxy holds one and destroys this state before releasing it
    const void* proxy = nullptr;   // identity only, no reference (a reference would be a cycle)
    DWORD queue_wait_ms = 5000;
    std::mutex present_mutex;  // serialize the wait, actual present and frame-count comparison
    std::atomic<std::uint64_t> presented{0}, blank{0}, refused{0}, failed{0};
};

HRESULT refuse(BridgeState& state, const char* method, HRESULT hr, const char* reason) {
    if (state.refused.fetch_add(1, std::memory_order_relaxed) < kLoggedRefusals)
        std::fprintf(stderr, "[rt] present bridge: %s refused: %s (hr=0x%08lx, nothing presented)\n", method, reason,
                     static_cast<unsigned long>(hr));
    return hr;
}

HRESULT fail(BridgeState& state, const char* what, HRESULT hr) {
    if (state.failed.fetch_add(1, std::memory_order_relaxed) < kLoggedRefusals)
        std::fprintf(stderr, "[rt] present bridge: PresentX failed: %s (hr=0x%08lx)\n", what,
                     static_cast<unsigned long>(hr));
    return hr;
}

// `object`'s GetDevice answers this bridge's device: the real device, or the proxy once set_device_outer ran.
template <class T>
bool same_device(const BridgeState& state, T* object) {
    ID3D12Device* owner = nullptr;
    if (FAILED(object->GetDevice(IID_PPV_ARGS(&owner))) || !owner) return false;
    const bool same = owner == state.real || static_cast<const void*>(owner) == state.proxy;
    owner->Release();
    return same;
}

bool supported_sdr_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

// Waits, bounded, until everything submitted to `queue` before this call has executed: the presenter blits on the
// device's own VkQueue, which need not be the one behind `queue`. A temporary fence; nothing global is idled.
HRESULT wait_for_queue(BridgeState& state, ID3D12CommandQueue* queue) {
    ID3D12Fence* fence = nullptr;
    HRESULT hr = state.real->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr) || !fence) return fail(state, "CreateFence for the queue wait", FAILED(hr) ? hr : E_FAIL);
    Hold<ID3D12Fence> owned;
    owned.p = fence;
    if (hr = queue->Signal(fence, 1); FAILED(hr)) return fail(state, "Signal on the calling queue", hr);
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const UINT64 completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) return fail(state, "the calling queue's device was removed", DXGI_ERROR_DEVICE_REMOVED);
        if (completed >= 1) return S_OK;
        if (state.window.closed()) return fail(state, "the window was closed", HRESULT_FROM_WIN32(ERROR_CANCELLED));
        if (GetTickCount64() - start >= state.queue_wait_ms)
            return fail(state, "the calling queue's earlier work did not finish in time", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        // Bounded polling avoids registering a HANDLE that could still be signalled after a timeout. No handle leaks
        // or detached cleanup thread; this first-frame path trades up to one millisecond of host latency for it.
        Sleep(1);
    }
}

// ---- The hooks ----

void hook_destroy(void* opaque) {
    auto* state = static_cast<BridgeState*>(opaque);
    state->presentation.reset();  // presenter (and its real-device reference) first
    state->window.stop();         // then the window, on its own thread
    std::fprintf(stderr, "[rt] present bridge: destroyed (%llu presented, %llu refused, %llu failed)\n",
                 static_cast<unsigned long long>(state->presented.load()),
                 static_cast<unsigned long long>(state->refused.load()),
                 static_cast<unsigned long long>(state->failed.load()));
    delete state;
}

HRESULT hook_set_frame_interval(void* opaque, std::uint64_t, std::uint32_t length_us, std::uint32_t period_in_intervals,
                                std::uint32_t flags) {
    auto& state = *static_cast<BridgeState*>(opaque);
    constexpr auto method = "SetFrameIntervalX";
    if (flags) return refuse(state, method, E_NOTIMPL, "flags are not 0");
    // The output is not read: there is one window. The third word is `periodInIntervals` in the public Ebiten binding
    // and the frame latency in the samples' use; the pacer keeps its existing provisional latency reading.
    const HRESULT hr = state.presentation->SetFrameIntervalX(nullptr, length_us, period_in_intervals, 0);
    if (FAILED(hr)) refuse(state, method, hr, "rejected by the frame pacer");
    return hr;
}

HRESULT hook_schedule_frame_event(void* opaque, std::uint32_t event, std::uint32_t offset_us, std::uint64_t ancillary,
                                  std::uint32_t flags) {
    auto& state = *static_cast<BridgeState*>(opaque);
    constexpr auto method = "ScheduleFrameEventX";
    if (!palmwave::supported_frame_event(event))
        return refuse(state, method, E_NOTIMPL, "only origin (0) and scheduled event index 2 are supported");
    if (ancillary) return refuse(state, method, E_NOTIMPL, "an ancillary signal list is not supported");
    if (flags) return refuse(state, method, E_NOTIMPL, "flags are not 0");
    const HRESULT hr = state.presentation->ScheduleFrameEventX(event, offset_us, 0);
    if (FAILED(hr)) refuse(state, method, hr, "rejected by the frame pacer");
    return hr;
}

HRESULT hook_wait_frame_event(void* opaque, std::uint32_t event, std::uint32_t timeout_ms, std::uint64_t ancillary,
                              std::uint32_t flags, std::uint64_t* token) {
    auto& state = *static_cast<BridgeState*>(opaque);
    constexpr auto method = "WaitFrameEventX";
    if (!token) return refuse(state, method, E_POINTER, "null token pointer");
    if (!palmwave::supported_frame_event(event))
        return refuse(state, method, E_NOTIMPL, "only origin (0) and scheduled event index 2 are supported");
    if (ancillary) return refuse(state, method, E_NOTIMPL, "an ancillary wait list is not supported");
    if (flags) return refuse(state, method, E_NOTIMPL, "flags are not 0");
    // A timeout is an answer, not a refusal; nothing registered (ERROR_INVALID_STATE) is logged as one.
    const HRESULT hr = state.presentation->WaitFrameEventX(event, timeout_ms, 0, token);
    if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE))
        refuse(state, method, hr, "no frame interval or requested event registered (or suspended)");
    return hr;
}

HRESULT hook_present(void* opaque, ID3D12CommandQueue* queue, std::uint64_t plane_count, std::uint64_t planes,
                     std::uint64_t params) {
    auto& state = *static_cast<BridgeState*>(opaque);
    constexpr auto method = "PresentX";
    std::lock_guard serialise(state.present_mutex);
    if (state.window.closed()) return refuse(state, method, HRESULT_FROM_WIN32(ERROR_CANCELLED), "the window was closed");
    if (IsIconic(state.window.window())) return DXGI_STATUS_OCCLUDED;
    const XboxPresentWireCheck check = check_present_wire(plane_count, planes, params);
    if (FAILED(check.hr)) return refuse(state, method, check.hr, check.reason);
    if (!queue) return refuse(state, method, E_INVALIDARG, "no queue behind the proxy");
    if (!same_device(state, queue)) return refuse(state, method, E_INVALIDARG, "the queue belongs to another device");

    if (check.request.blank) {
        // Zero planes: a real black frame, ordered after this queue's earlier work too.
        if (const HRESULT waited = wait_for_queue(state, queue); FAILED(waited)) return waited;
        const auto before = state.presentation->presenter().presented();
        const HRESULT hr = state.presentation->PresentX(0, nullptr, nullptr);
        if (FAILED(hr)) return fail(state, "blank present", hr);
        if (const HRESULT done = state.presentation->presenter().wait_for_present(state.queue_wait_ms); FAILED(done))
            return fail(state, "the blank blit did not complete", done);
        if (state.presentation->presenter().presented() == before) return DXGI_STATUS_OCCLUDED;
        state.blank.fetch_add(1, std::memory_order_relaxed);
        state.presented.fetch_add(1, std::memory_order_relaxed);
        return hr;
    }

    Hold<ID3D12Resource> resource(check.request.resource);
    if (vulkan_image(resource.p) == VK_NULL_HANDLE)
        return refuse(state, method, E_NOTIMPL, "the plane resource is not a texture of this layer");
    if (!same_device(state, resource.p))
        return refuse(state, method, E_INVALIDARG, "the plane resource belongs to another device");
    const D3D12_RESOURCE_DESC desc = resource.p->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0)
        return refuse(state, method, E_NOTIMPL, "the plane resource is not a single-sample, single-mip 2D texture");
    if (!supported_sdr_format(desc.Format))
        return refuse(state, method, E_NOTIMPL, "the plane resource format is not an 8-bit or 10:10:10:2 UNORM format");
    const D3D12_RECT* rect = check.request.has_source_rect ? &check.request.source_rect : nullptr;
    if (rect && (UINT64(rect->right) > desc.Width || UINT(rect->bottom) > desc.Height))
        return refuse(state, method, E_INVALIDARG, "the source rectangle does not fit the texture");

    if (const HRESULT waited = wait_for_queue(state, queue); FAILED(waited)) return waited;

    ID3D12Resource* const resources[1] = {resource.p};
    XboxPresentPlaneParameters plane{};
    plane.Token = check.request.token;
    plane.ResourceCount = 1;
    plane.ppResources = resources;
    plane.pSrcViewRects = rect;
    // pDestPlacementBase null, ColorSpace 0, ScaleFilter 0, no extended descriptors, Flags 0: the plain blit path.
    const auto before = state.presentation->presenter().presented();
    const HRESULT hr = state.presentation->PresentX(1, &plane, nullptr);
    if (FAILED(hr)) {
        // E_INVALIDARG from the core: a token its pacer never handed out. Counted as a refusal, not a failure.
        if (hr == E_INVALIDARG) return refuse(state, method, hr, "the token was never handed out by WaitFrameEventX");
        return fail(state, "the presenter", hr);
    }
    // Complete the presenter's read before the caller can overwrite this source on another Vulkan queue.
    if (const HRESULT done = state.presentation->presenter().wait_for_present(state.queue_wait_ms); FAILED(done))
        return fail(state, "the source blit did not complete", done);
    if (state.presentation->presenter().presented() == before) return DXGI_STATUS_OCCLUDED;
    state.presented.fetch_add(1, std::memory_order_relaxed);
    return hr;
}

const d3d12_xs::PresentationHooks kHooks{hook_destroy, hook_set_frame_interval, hook_schedule_frame_event,
                                         hook_wait_frame_event, hook_present};

BridgeState* state_of(const void* device) {
    return static_cast<BridgeState*>(d3d12_xs::device_presentation_state(device, &kHooks));
}

}  // namespace

HRESULT install_present_bridge(void* device, const PresentBridgeOptions& options) {
    void* proxy = d3d12_xs::device_proxy_primary(device);
    ID3D12Device* real = proxy ? d3d12_xs::device_proxy_real_device(proxy) : nullptr;
    const VulkanDeviceInfo* info = real ? vulkan_device_info(real) : nullptr;
    if (!info) {
        std::fprintf(stderr, "[rt] present bridge: %p is not an Xbox device proxy over this layer's device\n", device);
        return E_INVALIDARG;
    }
    if (!info->presentation) {
        std::fprintf(stderr, "[rt] present bridge: the Vulkan device has no presentation support; no bridge\n");
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (device_has_outer(real)) {
        std::fprintf(stderr, "[rt] present bridge: set_device_outer already ran; the presenter would hold the proxy "
                             "(a reference cycle), so no bridge\n");
        return E_UNEXPECTED;
    }
    if (state_of(proxy)) return E_UNEXPECTED;
    try {
        auto state = std::make_unique<BridgeState>();
        state->real = real;
        state->proxy = proxy;
        state->queue_wait_ms = options.queue_wait_ms;
        if (!state->window.start(options.client_width, options.client_height, options.visible)) {
            std::fprintf(stderr, "[rt] present bridge: the window could not be created; no bridge\n");
            return E_FAIL;
        }
        // On the real device (no outer yet), so the presenter's reference is to the device, never to the proxy.
        auto presenter = create_window_presenter(real, state->window.window());
        if (!presenter) {
            std::fprintf(stderr, "[rt] present bridge: no presenter for the window; no bridge\n");
            return DXGI_ERROR_UNSUPPORTED;
        }
        if (options.capture && !presenter->enable_capture()) {
            std::fprintf(stderr, "[rt] present bridge: capture requested but not available; no bridge\n");
            return E_FAIL;
        }
        state->presentation = std::make_unique<XboxPresentation>(std::move(presenter), options.clock);
        BridgeState* owned = state.get();
        owned->presentation->presenter().set_abort([owned] {
            return owned->window.closed() || owned->presentation->pacer().suspended();
        });
        if (!d3d12_xs::set_device_presentation(proxy, &kHooks, state.get())) return E_UNEXPECTED;
        std::fprintf(stderr, "[rt] present bridge: installed on device proxy %p (window \"Palmwave Game Launcher\" "
                             "%p); strict single-plane SDR PresentX subset\n",
                     proxy, static_cast<void*>(state->window.window()));
        state.release();  // owned by the proxy now
        return S_OK;
    } catch (...) {
        std::fprintf(stderr, "[rt] present bridge: out of memory or thread creation failed; no bridge\n");
        return E_OUTOFMEMORY;
    }
}

XboxPresentation* present_bridge_presentation(const void* device) {
    BridgeState* state = state_of(device);
    return state ? state->presentation.get() : nullptr;
}

HWND present_bridge_window(const void* device) {
    BridgeState* state = state_of(device);
    return state ? state->window.window() : nullptr;
}

PresentBridgeCounts present_bridge_counts(const void* device) {
    PresentBridgeCounts counts;
    if (BridgeState* state = state_of(device)) {
        counts.presented = state->presented.load();
        counts.blank = state->blank.load();
        counts.refused = state->refused.load();
        counts.failed = state->failed.load();
    }
    return counts;
}

}  // namespace rt::d3d12
