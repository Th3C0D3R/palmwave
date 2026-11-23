// The PresentX bridge (shims/xbox_present_bridge.h) end to end on the Vulkan-backed device: the device proxy's frame-event
// slots 78/79/80, modern queue slot 35 submission and slot 32 PresentX, checked on the swapchain image read back from a real "Palmwave Game Launcher" window.
//
// Every source is a texture this test creates and clears with ClearRenderTargetView on a real D3D12 queue of the layer;
// the test does NOT wait for that work on the CPU before PresentX, so the bridge's wait on the calling queue is what
// orders the clear before the blit. No shaders, assets or game data. A frame shown here is a synthetic test frame, not a
// game frame.
//
// Cases: window title/visibility/thread; WaitFrameEventX before registration refused; unknown frame-event values
// refused; tokens from slot 80 accepted by PresentX (one shared pacer); full-frame colour; a source rectangle (each half
// of a two-colour texture); unknown plane/parameter values, two planes, a buffer and a never-issued token refused with
// the last frame and the present counts unchanged; a blank frame; the bridge outliving the caller's device reference
// through a queue, and the window going away with the last queue reference.
// Exit 77 (skipped) without a Vulkan device with presentation, or when the only validation error is the NVIDIA layer's
// known sRGB swapchain check. Any other validation error fails (PALMWAVE_VK_VALIDATION=1).
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../shims/xbox_present_bridge.h"
#include "../shims/xbox_present_wire.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "xbox_present_bridge line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

constexpr UINT kWidth = 96, kHeight = 64;

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
};

using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: the 8-byte handle in RAX
using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

HRESULT device_slot(void* device, unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                    std::uint64_t a4, std::uint64_t a5 = 0) {
    const auto fn = reinterpret_cast<SlotFn>((*reinterpret_cast<void***>(device))[slot]);
    return static_cast<HRESULT>(static_cast<std::uint32_t>(fn(device, a1, a2, a3, a4, a5)));
}

std::uint64_t word(const void* p) { return reinterpret_cast<std::uint64_t>(p); }

struct Frame {
    std::vector<std::uint8_t> bytes;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
};

// Every pixel of [x0, x1) x [y0, y1) is (r, g, b) within 1. The swapchain must be an 8-bit UNORM format, so a UNORM
// source is copied without an sRGB conversion.
bool region_is(const Frame& f, unsigned x0, unsigned y0, unsigned x1, unsigned y1, int r, int g, int b) {
    const bool bgr = f.format == VK_FORMAT_B8G8R8A8_UNORM;
    if (!bgr && f.format != VK_FORMAT_R8G8B8A8_UNORM) {
        std::fprintf(stderr, "swapchain format %d is not an 8-bit UNORM format\n", int(f.format));
        return false;
    }
    for (unsigned y = y0; y < y1; ++y)
        for (unsigned x = x0; x < x1; ++x) {
            const std::uint8_t* p = &f.bytes[(std::size_t(y) * f.extent.width + x) * 4];
            const int pr = bgr ? p[2] : p[0], pg = p[1], pb = bgr ? p[0] : p[2];
            if (std::abs(pr - r) > 1 || std::abs(pg - g) > 1 || std::abs(pb - b) > 1) {
                std::fprintf(stderr, "pixel (%u, %u) = %d %d %d, expected %d %d %d\n", x, y, pr, pg, pb, r, g, b);
                return false;
            }
        }
    return true;
}

}  // namespace

int main() {
    vktest::Device d;
    d.queue_count_wanted = 2;  // a second VkQueue where the family has one: D3D12 queues need not share the presenter's
    if (!d.create(true)) {
        std::printf("no Vulkan device with presentation: skipped\n");
        return 77;
    }
    palmwave::ManualFrameClock clock;  // outlives the device and its bridge
    const UINT period = rt::d3d12::xbox_frame_interval_60hz_us;
    HWND window = nullptr;
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        if (d.queues.size() > 1) info.command_queues = d.queues;
        info.presentation = true;
        info.swapchain_maintenance1 = d.swapchain_maintenance1;
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        if (FAILED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device.p), 0x00020004u))) {
            inner->Release();
            EXPECT(false);
        }

        // Install as D3D12XboxCreateDevice does: bridge first, outer second.
        rt::d3d12::PresentBridgeOptions options;
        options.clock = &clock;
        options.capture = true;
        options.client_width = int(kWidth);
        options.client_height = int(kHeight);
        const HRESULT installed = rt::d3d12::install_present_bridge(device.p, options);
        if (installed == DXGI_ERROR_UNSUPPORTED) {
            std::printf("no presenter for a window on this device: skipped\n");
            return 77;
        }
        EXPECT(installed == S_OK);
        rt::d3d12::set_device_outer(inner, device.p);
        EXPECT(rt::d3d12::install_present_bridge(device.p, options) == E_UNEXPECTED);  // outer set, already installed

        window = rt::d3d12::present_bridge_window(device.p);
        EXPECT(window && IsWindow(window) && IsWindowVisible(window));
        wchar_t title[64] = {};
        EXPECT(GetWindowTextW(window, title, 64) > 0 && std::wcscmp(title, L"Palmwave Game Launcher") == 0);
        EXPECT(GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId());  // the bridge's own pumping thread
        rt::d3d12::XboxPresentation* presentation = rt::d3d12::present_bridge_presentation(device.p);
        EXPECT(presentation);

        // Frame events through the device proxy (slots 78/79/80, public-HLE indices).
        std::uint64_t token = 0;
        EXPECT(device_slot(device.p, 80, 0, 0, 0, 0, word(&token)) == HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        EXPECT(token == 0);
        EXPECT(device_slot(device.p, 78, 0, period, 1, 1) == E_NOTIMPL);          // unknown interval flag
        EXPECT(device_slot(device.p, 78, 0, period, 1, 0) == S_OK);
        EXPECT(device_slot(device.p, 79, 1, 0, 0, 0) == E_NOTIMPL);               // not the origin event
        EXPECT(device_slot(device.p, 79, 0, 0, word(&token), 0) == E_NOTIMPL);    // ancillary list
        EXPECT(device_slot(device.p, 79, 0, 0, 0, 4) == E_NOTIMPL);               // flags
        EXPECT(device_slot(device.p, 79, 0, 0, 0, 0) == S_OK);
        // Measured registration sequence: origin 0 followed by index 2, both zero-offset, no ancillary/flags.
        EXPECT(device_slot(device.p, 79, 2, 0, 0, 0) == S_OK);
        EXPECT(device_slot(device.p, 80, 0, 0, word(&token), 0, word(&token)) == E_NOTIMPL);
        EXPECT(device_slot(device.p, 80, 0, 0, 0, 0, 0) == E_POINTER);
        std::uint64_t last_token = 0;
        const auto next_token = [&](std::uint64_t& out) -> bool {
            clock.advance_us(period);
            return device_slot(device.p, 80, 0, 0, 0, 0, word(&out)) == S_OK && out > last_token &&
                   (last_token = out, true);
        };

        // Queues: a first one that may take the presenter's VkQueue, then the one this test submits on.
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> first_queue, queue;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&first_queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(rt::d3d12_xs::is_queue_proxy(queue.p));
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12DescriptorHeap> rtv_heap;
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(list->Close()));
        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{SIZE_T(reinterpret_cast<XboxHeapHandleFn>(
            (*reinterpret_cast<void***>(rtv_heap.p))[9])(rtv_heap.p))};

        Ref<ID3D12Resource> source;
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &desc,
                                                         D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                                         IID_PPV_ARGS(&source.p))));
        device->CreateRenderTargetView(source.p, nullptr, rtv);

        // Clears `source` on `queue` and returns without waiting on the CPU: the bridge orders the blit after it.
        const auto clear = [&](const FLOAT colour[4], UINT rect_count = 0, const D3D12_RECT* rects = nullptr,
                               const FLOAT* second_colour = nullptr, const D3D12_RECT* second_rect = nullptr) -> bool {
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.p, nullptr))) return false;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = source.p;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            list->ResourceBarrier(1, &barrier);
            list->ClearRenderTargetView(rtv, colour, rect_count, rects);
            if (second_colour) list->ClearRenderTargetView(rtv, second_colour, 1, second_rect);
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            list->ResourceBarrier(1, &barrier);
            if (FAILED(list->Close())) return false;
            ID3D12CommandList* lists[] = {list.p};
            if (device_slot(queue.p, 35, 1, word(lists), 0, 0) != S_OK) return false;
            return rt::d3d12::last_execute_result(queue.p) == palmwave::ReplayResult::success;
        };
        ID3D12Resource* resources[1] = {source.p};
        const auto plane_for = [&](std::uint64_t t, const D3D12_RECT* rect = nullptr) {
            rt::d3d12::XboxPresentPlaneWire plane{};
            plane.Token = t;
            plane.ResourceCount = 1;
            plane.ppResources = word(resources);
            plane.pSrcViewRects = word(rect);
            return plane;
        };
        const auto present = [&](const rt::d3d12::XboxPresentPlaneWire* planes, std::uint64_t count = 1,
                                 const void* params = nullptr) {
            return device_slot(queue.p, 32, count, word(planes), word(params), 0);
        };
        const auto grab = [&](Frame& f) -> bool {
            const bool captured = presentation->presenter().read_back(f.bytes, &f.extent, &f.format);
            RECT client{};
            GetClientRect(window, &client);
            std::printf("readback: captured %u, %ux%u, format %d, bytes %zu; client %ldx%ld; source %ux%u\n",
                        unsigned(captured), f.extent.width, f.extent.height, int(f.format), f.bytes.size(),
                        client.right - client.left, client.bottom - client.top, kWidth, kHeight);
            // Native caption/minimum-window and DPI constraints may change the requested client dimensions. Check
            // the entire captured output, not just a 96x64 corner, while the source remains exactly 96x64.
            return captured && f.extent.width > 0 && f.extent.height > 0 &&
                   f.bytes.size() == std::size_t(f.extent.width) * f.extent.height * 4;
        };

        // 1. A full frame of one colour (exact UNORM values).
        const FLOAT teal[4] = {51 / 255.0f, 153 / 255.0f, 204 / 255.0f, 1.0f};
        EXPECT(clear(teal));
        EXPECT(next_token(token) && token == 1);  // the first origin of the shared pacer
        EXPECT(device_slot(device.p, 80, 2, 0, 0, 0, word(&token)) == S_OK && token == 1);
        EXPECT(presentation->pacer().tokens_issued() == 1);  // index 2 observes the same token, never creates another
        auto plane = plane_for(token);
        EXPECT(present(&plane) == S_OK);
        Frame frame;
        EXPECT(grab(frame));
        EXPECT(region_is(frame, 0, 0, frame.extent.width, frame.extent.height, 51, 153, 204));
        EXPECT(rt::d3d12::present_bridge_counts(device.p).presented == 1);
        EXPECT(presentation->presents() == 1);

        // 2. A source rectangle: left half red, right half green; each half alone fills the window.
        const FLOAT red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
        const D3D12_RECT left{0, 0, LONG(kWidth / 2), LONG(kHeight)}, right{LONG(kWidth / 2), 0, LONG(kWidth), LONG(kHeight)};
        EXPECT(clear(red, 1, &left, green, &right));  // one recording; no allocator reset while earlier work is in flight
        EXPECT(next_token(token));
        plane = plane_for(token, &right);
        EXPECT(present(&plane) == S_OK);
        EXPECT(grab(frame));
        // The centre: a linear blit may blend across the seam at the window's left edge column.
        EXPECT(region_is(frame, frame.extent.width / 4, frame.extent.height / 4, frame.extent.width * 3 / 4, frame.extent.height * 3 / 4, 0, 255, 0));
        EXPECT(next_token(token));
        plane = plane_for(token, &left);
        EXPECT(present(&plane) == S_OK);
        // No readback/wait here: the following blue clear reuses the source on another queue immediately after
        // PresentX returns. Its own completion fence must make this safe; capture cannot provide the ordering.
        const D3D12_RECT too_wide{0, 0, LONG(kWidth + 1), LONG(kHeight)};
        plane = plane_for(token, &too_wide);
        EXPECT(present(&plane) == E_INVALIDARG);

        // 3. Refusals leave the last frame and every count unchanged. The texture is cleared blue first, so a refused
        // request that still presented would change the read-back.
        const FLOAT blue[4] = {0, 0, 1, 1};
        EXPECT(clear(blue));
        Frame last;
        EXPECT(grab(last));
        const auto counts_before = rt::d3d12::present_bridge_counts(device.p);
        const std::uint64_t shown_before = presentation->presenter().presented();
        EXPECT(next_token(token));
        auto bad = plane_for(token);
        bad.ColorSpace = 12;  // HDR10
        EXPECT(present(&bad) == E_NOTIMPL);
        bad = plane_for(token);
        bad.ScaleFilter = 1;
        EXPECT(present(&bad) == E_NOTIMPL);
        bad = plane_for(token);
        bad.Flags = 1;
        EXPECT(present(&bad) == E_NOTIMPL);
        const float whole[4] = {0, 0, 1, 1};
        bad = plane_for(token);
        bad.pDestPlacementBase = word(whole);
        EXPECT(present(&bad) == E_NOTIMPL);
        rt::d3d12::XboxPresentParametersWire params{};
        params.ImmediateThresholdPercent = 50.0f;
        plane = plane_for(token);
        EXPECT(present(&plane, 1, &params) == E_NOTIMPL);
        const rt::d3d12::XboxPresentPlaneWire pair[2] = {plane_for(token), plane_for(token)};
        EXPECT(present(pair, 2) == E_NOTIMPL);
        plane = plane_for(token + 1000);  // never handed out by WaitFrameEventX
        EXPECT(present(&plane) == E_INVALIDARG);
        Ref<ID3D12Resource> buffer;
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = 256;
        buffer_desc.Height = 1;
        buffer_desc.DepthOrArraySize = 1;
        buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer.p))));
        ID3D12Resource* buffers[1] = {buffer.p};
        bad = plane_for(token);
        bad.ppResources = word(buffers);
        EXPECT(present(&bad) == E_NOTIMPL);
        const auto counts_after = rt::d3d12::present_bridge_counts(device.p);
        EXPECT(counts_after.presented == counts_before.presented && counts_after.failed == counts_before.failed);
        EXPECT(counts_after.refused == counts_before.refused + 8);
        EXPECT(presentation->presenter().presented() == shown_before);
        Frame after;
        EXPECT(grab(after) && after.bytes == last.bytes);
        EXPECT(region_is(after, after.extent.width / 4, after.extent.height / 4, after.extent.width * 3 / 4, after.extent.height * 3 / 4, 255, 0, 0));
        // The same token is still good: the refusals consumed nothing. Now the blue clear shows.
        plane = plane_for(token);
        EXPECT(present(&plane) == S_OK);
        EXPECT(grab(frame) && region_is(frame, 0, 0, frame.extent.width, frame.extent.height, 0, 0, 255));

        // 4. Zero planes: a real black frame.
        EXPECT(present(nullptr, 0) == S_OK);
        EXPECT(grab(frame) && region_is(frame, 0, 0, frame.extent.width, frame.extent.height, 0, 0, 0));
        const auto counts = rt::d3d12::present_bridge_counts(device.p);
        EXPECT(counts.presented == 5 && counts.blank == 1 && counts.failed == 0);
        EXPECT(presentation->presents() == counts.presented);
        std::printf("xbox_present_bridge: %llu presents read back (1 blank), %llu refusals\n",
                    static_cast<unsigned long long>(counts.presented), static_cast<unsigned long long>(counts.refused));

        // A minimized window must neither report S_OK nor advance the actual frame count.
        const auto before_minimized = rt::d3d12::present_bridge_counts(device.p).presented;
        ShowWindow(window, SW_MINIMIZE);
        for (unsigned i = 0; i < 100 && !IsIconic(window); ++i) Sleep(1);
        EXPECT(IsIconic(window));
        EXPECT(present(nullptr, 0) == DXGI_STATUS_OCCLUDED);
        EXPECT(rt::d3d12::present_bridge_counts(device.p).presented == before_minimized);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        for (unsigned i = 0; i < 100 && IsIconic(window); ++i) Sleep(1);
        EXPECT(!IsIconic(window));

        // 5. Lifetime: the queue keeps the device proxy, and so the bridge and its window, after the caller's device
        // reference is gone; the last queue reference takes them down.
        buffer.reset();
        source.reset();
        rtv_heap.reset();
        list.reset();
        allocator.reset();
        first_queue.reset();
        void* proxy = device.p;
        device.reset();
        EXPECT(IsWindow(window));
        EXPECT(rt::d3d12::present_bridge_window(proxy) == window);  // still alive: the queue holds the proxy
        EXPECT(present(nullptr, 0) == S_OK);
        SendMessageW(window, WM_CLOSE, 0, 0);
        EXPECT(IsWindow(window) && !IsWindowVisible(window));
        EXPECT(present(nullptr, 0) == HRESULT_FROM_WIN32(ERROR_CANCELLED));
        queue.reset();  // last reference: the proxy destroys the bridge, then releases the real device
    }
    EXPECT(!IsWindow(window));

    if (d.validation_errors() > 0) {
        if (d.validation_errors() == d.known_validation_errors()) {
            std::printf("only the known sRGB swapchain validation message: skipped\n");
            return 77;
        }
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("xbox_present_bridge: ok (synthetic frames only)\n");
    return 0;
}
