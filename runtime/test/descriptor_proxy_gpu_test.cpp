// Title-backed CPU descriptor handles (d3d12_xs_device.h, "Title-backed descriptor heaps") on the Vulkan-backed device,
// through the device proxy, with a real GPU clear and readback:
//   - an explicit DescriptorHeapAllocator hands device slot 14 two CPU byte arrays of this test as the backings of an
//     RTV heap and a DSV heap. No registry and no hook registration: loader_test and device_proxy_test cover those;
//   - heap slot 9, called with the console's raw ABI (the handle comes back in RAX), returns those arrays' addresses;
//   - device slots 20/21 write views at the title handles; the real heaps receive them and the arrays stay unwritten;
//   - an RTV heap created directly on the Vulkan device (never seen by slot 14) is used through the same proxy slots
//     next to the title ranges, so the PC path for native heaps is shown unchanged;
//   - a console list from device slot 73 (the measured console IID) records OMSetRenderTargets (slot 50, single range
//     and array), ClearDepthStencilView (slot 51, typed: FLOAT Depth 0.25 in XMM3), ClearRenderTargetView (slot 52),
//     ResourceBarrier (26) and CopyTextureRegion (16) into a readback buffer, then Close (9); the queue and fence
//     proxies run it and every pixel of three 4x4 RGBA8 targets and every value of a 4x4 D32 target is compared.
// The backing arrays have static storage and outlive every heap; the explicit allocator is cleared only after both
// title-backed heaps are released and their backings were returned through the paired release callback.
// PALMWAVE_VK_VALIDATION=1 runs under VK_LAYER_KHRONOS_validation with synchronization validation; any validation error,
// counted until after the VkDevice is destroyed, fails the test. Exits 77 only when no Vulkan device can be created.
// Synthetic: no shaders, no game data. It checks the handle translation on a real GPU; it does not render a retail
// title frame and does not establish the console's descriptor ABI.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_command_list_iid.h"
#include "../shims/d3d12_xs_device.h"

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "descriptor_proxy_gpu_test line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

namespace {

namespace xs = rt::d3d12_xs;

template <class T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
};

// ---- The synthetic title's descriptor memory: two CPU byte arrays, handed out in order (RTV heap, DSV heap) ----

constexpr std::size_t kBackingBytes = 4096;
struct TitleMemory {
    alignas(64) std::uint8_t bytes[2][kBackingBytes];
    unsigned allocations = 0;
    unsigned frees = 0;
    unsigned bad_calls = 0;  // wrong attributes, unknown token or address, double free
    std::size_t sizes[2] = {};
    bool freed[2] = {};
};
TitleMemory g_title;  // static storage: valid until the process ends, after every heap and resource is gone

xs::DescriptorBackingStatus title_allocate(std::size_t size, std::uint64_t attributes, xs::DescriptorHeapBacking* out) {
    const unsigned index = g_title.allocations++;
    if (attributes != xs::kXmemDescriptorHeapAttributes || !out) ++g_title.bad_calls;
    if (!out || index >= 2 || size == 0 || size > kBackingBytes) return xs::DescriptorBackingStatus::out_of_memory;
    g_title.sizes[index] = size;
    out->address = g_title.bytes[index];
    out->token = index + 1;
    out->release_hook = nullptr;
    return xs::DescriptorBackingStatus::allocated;
}

void title_release(const xs::DescriptorHeapBacking& backing, std::uint64_t attributes) {
    ++g_title.frees;
    const std::uint64_t index = backing.token - 1;  // token 0 wraps and is rejected below
    if (attributes != xs::kXmemDescriptorHeapAttributes || index >= 2 || backing.address != g_title.bytes[index] ||
        g_title.freed[index]) {
        ++g_title.bad_calls;
        return;
    }
    g_title.freed[index] = true;
}

// The views must land in the real heaps: the title's bytes are never written by the proxy.
bool title_bytes_untouched(unsigned index) {
    for (const std::uint8_t byte : g_title.bytes[index])
        if (byte) return false;
    return true;
}

// Installs the explicit allocator for slot 14 and clears it on every exit path. Declared before the heaps, so it is
// destroyed after them.
struct AllocatorScope {
    explicit AllocatorScope(const xs::DescriptorHeapAllocator& allocator) { xs::set_descriptor_heap_allocator(&allocator); }
    ~AllocatorScope() { xs::set_descriptor_heap_allocator(nullptr); }
    AllocatorScope(const AllocatorScope&) = delete;
    AllocatorScope& operator=(const AllocatorScope&) = delete;
};

// Raw console calls. Recording methods are void; their return register is never read.
using CreateHeapFn = HRESULT(WINAPI*)(void*, const D3D12_DESCRIPTOR_HEAP_DESC*, const IID*, void**);
using HeapCpuStartFn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(void*);  // heap slot 9: handle in RAX
using CreateRtvFn = void(WINAPI*)(void*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                  D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateDsvFn = void(WINAPI*)(void*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                  D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateListXFn = HRESULT(WINAPI*)(void*, const UINT*, ID3D12CommandAllocator*, ID3D12PipelineState*, const IID*,
                                       void**);
using CloseFn = HRESULT(WINAPI*)(void*);
using CopyTextureFn = void(WINAPI*)(void*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                    const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using BarrierFn = void(WINAPI*)(void*, UINT, const D3D12_RESOURCE_BARRIER*);
using OmSetFn = void(WINAPI*)(void*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ClearDsvFn = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT,
                                 const D3D12_RECT*);
using ClearRtvFn = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);

D3D12_CPU_DESCRIPTOR_HANDLE heap_slot9(ID3D12DescriptorHeap* heap) {
    return reinterpret_cast<HeapCpuStartFn>((*reinterpret_cast<void***>(heap))[9])(heap);
}

int wait_for(ID3D12Fence* fence, UINT64 value) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(event != nullptr);
    const bool set = SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
                     WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    CloseHandle(event);
    CHECK(set && fence->GetCompletedValue() >= value);
    return 0;
}

HRESULT make_texture(ID3D12Device* device, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                     D3D12_RESOURCE_STATES state, Ref<ID3D12Resource>& out) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 4;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out.p));
}

// `device` is the proxy, `real` the Vulkan-layer device behind it.
int check_title_descriptors(vktest::Device& vk, ID3D12Device* device, ID3D12Device* real) {
    using T = xs::DescriptorHandleTranslation;
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    const std::size_t ranges_before = xs::title_descriptor_range_count();
    const std::size_t tracked_before = xs::tracked_descriptor_heap_count();
    void** dv = *reinterpret_cast<void***>(device);
    const UINT rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT dsv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    CHECK(rtv_inc && dsv_inc && 2u * rtv_inc <= kBackingBytes && 2u * dsv_inc <= kBackingBytes);

    unsigned color_bytes = 0, depth_values = 0;
    {
        const xs::DescriptorHeapAllocator allocator{&title_allocate, &title_release};
        const AllocatorScope scope(allocator);
        // Declaration order is the reverse of the release order an early return takes: list objects first, then
        // resources, then the title-backed heaps, then the native heap, then the allocator.
        Ref<ID3D12DescriptorHeap> native;
        Ref<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
        Ref<ID3D12Resource> targets[3], depth, readback;
        Ref<ID3D12Fence> fence;
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> list_allocator;
        Ref<IUnknown> list;
        Ref<ID3D12CommandList> submission;
        // On an early failure after submission, finish GPU work before any resource or heap destructor runs.
        struct IdleBeforeRelease {
            vktest::Device& vk;
            ~IdleBeforeRelease() { vk.fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(vk.device); }
        } idle_before_release{vk};

        // Device slot 14: two title-backed heaps, each asked for NumDescriptors * increment with 0x00800004.
        const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
        const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        CHECK(SUCCEEDED(reinterpret_cast<CreateHeapFn>(dv[14])(device, &rtv_desc, &heap_iid,
                                                               reinterpret_cast<void**>(&rtv_heap.p))));
        CHECK(SUCCEEDED(reinterpret_cast<CreateHeapFn>(dv[14])(device, &dsv_desc, &heap_iid,
                                                               reinterpret_cast<void**>(&dsv_heap.p))));
        CHECK(xs::is_heap_proxy(rtv_heap.p) && xs::is_heap_proxy(dsv_heap.p));
        CHECK(g_title.allocations == 2 && g_title.bad_calls == 0);
        CHECK(g_title.sizes[0] == 2u * rtv_inc && g_title.sizes[1] == 2u * dsv_inc);
        CHECK(xs::title_descriptor_range_count() == ranges_before + 2);
        CHECK(xs::tracked_descriptor_heap_count() == tracked_before + 2);

        // A native heap made on the Vulkan device itself, alongside the title ranges: slot 14 never sees it.
        const D3D12_DESCRIPTOR_HEAP_DESC native_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                                                     0};
        CHECK(SUCCEEDED(real->CreateDescriptorHeap(&native_desc, IID_PPV_ARGS(&native.p))));
        CHECK(!xs::is_heap_proxy(native.p) && g_title.allocations == 2);
        CHECK(xs::tracked_descriptor_heap_count() == tracked_before + 2);

        // Heap slot 9, console ABI: the title's addresses. The real heaps keep their own CPU starts.
        const SIZE_T rtv_base = heap_slot9(rtv_heap.p).ptr;
        const SIZE_T dsv_base = heap_slot9(dsv_heap.p).ptr;
        CHECK(rtv_base == reinterpret_cast<SIZE_T>(g_title.bytes[0]));
        CHECK(dsv_base == reinterpret_cast<SIZE_T>(g_title.bytes[1]));
        ID3D12DescriptorHeap* real_rtv_heap = xs::heap_proxy_real_heap(rtv_heap.p);
        ID3D12DescriptorHeap* real_dsv_heap = xs::heap_proxy_real_heap(dsv_heap.p);
        CHECK(real_rtv_heap && real_dsv_heap);
        const SIZE_T real_rtv = real_rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
        const SIZE_T real_dsv = real_dsv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
        const SIZE_T native_rtv = native->GetCPUDescriptorHandleForHeapStart().ptr;
        CHECK(real_rtv && real_dsv && native_rtv && real_rtv != rtv_base && real_dsv != dsv_base);
        D3D12_CPU_DESCRIPTOR_HANDLE out{};
        CHECK(xs::translate_cpu_descriptor_range(device, {rtv_base + rtv_inc}, 1, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                                 &out) == T::translated && out.ptr == real_rtv + rtv_inc);
        CHECK(xs::translate_cpu_descriptor_range(device, {dsv_base + dsv_inc}, 1, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                                 &out) == T::translated && out.ptr == real_dsv + dsv_inc);
        CHECK(xs::translate_cpu_descriptor_range(device, {rtv_base}, 2, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, &out) ==
                  T::translated && out.ptr == real_rtv);
        CHECK(xs::translate_cpu_descriptor_range(device, {rtv_base + rtv_inc}, 2, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                                 &out) == T::invalid);  // the span runs past the heap's end
        CHECK(xs::translate_cpu_descriptor_range(device, {rtv_base}, 1, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, &out) ==
              T::invalid);  // a title RTV range used as a DSV handle
        CHECK(xs::translate_cpu_descriptor_range(device, {native_rtv}, 1, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, &out) ==
                  T::unchanged && out.ptr == native_rtv);

        // Real Vulkan resources: three 4x4 RGBA8 targets and a 4x4 D32 depth target.
        for (auto& target : targets)
            CHECK(SUCCEEDED(make_texture(device, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, target)));
        CHECK(SUCCEEDED(make_texture(device, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                     D3D12_RESOURCE_STATE_DEPTH_WRITE, depth)));
        ID3D12Resource* const images[4] = {targets[0].p, targets[1].p, targets[2].p, depth.p};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[4]{};
        UINT64 readback_size = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const D3D12_RESOURCE_DESC desc = images[i]->GetDesc();
            UINT64 bytes = 0;
            real->GetCopyableFootprints(&desc, 0, 1, readback_size, &footprints[i], nullptr, nullptr, &bytes);
            const D3D12_SUBRESOURCE_FOOTPRINT& f = footprints[i].Footprint;
            CHECK(bytes && f.Width == 4 && f.Height == 4 && f.Depth == 1 && f.RowPitch >= 16);
            readback_size = (footprints[i].Offset + bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                            ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        }
        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC readback_desc{};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = readback_size;
        readback_desc.Height = readback_desc.DepthOrArraySize = readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        CHECK(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&readback.p))));
        void* mapped = nullptr;
        CHECK(SUCCEEDED(readback->Map(0, nullptr, &mapped)) && mapped);
        std::memset(mapped, 0xcd, static_cast<std::size_t>(readback_size));  // a skipped copy cannot match
        readback->Unmap(0, nullptr);

        // Device slots 20/21: two title RTVs, the native RTV through the same proxy slot, and a title DSV at index 1.
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {{rtv_base}, {rtv_base + rtv_inc}, {native_rtv}};
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv{dsv_base + dsv_inc};
        const std::uint64_t refused_before = xs::refused_descriptor_handle_calls();
        for (unsigned i = 0; i < 3; ++i) reinterpret_cast<CreateRtvFn>(dv[20])(device, targets[i].p, nullptr, rtvs[i]);
        reinterpret_cast<CreateDsvFn>(dv[21])(device, depth.p, nullptr, dsv);
        CHECK(xs::refused_descriptor_handle_calls() == refused_before);
        CHECK(title_bytes_untouched(0) && title_bytes_untouched(1));

        // Queue and fence proxies, an allocator proxy, and a console list from slot 73 with the console IID.
        const D3D12_COMMAND_QUEUE_DESC queue_desc{};
        CHECK(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        CHECK(xs::is_queue_proxy(queue.p) && xs::is_fence_proxy(fence.p));
        CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&list_allocator.p))));
        const UINT list_desc[2] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0};
        const IID console_iid = xs::kCommandListIidPairs[0].console;
        CHECK(SUCCEEDED(reinterpret_cast<CreateListXFn>(dv[73])(device, list_desc, list_allocator.p, nullptr,
                                                                &console_iid, reinterpret_cast<void**>(&list.p))));
        CHECK(xs::is_list_proxy(list.p));
        CHECK(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&submission.p))));
        CHECK(static_cast<void*>(submission.p) == static_cast<void*>(list.p));

        // Read the counters before the calls they count (d3d12_xs_device.h counter rule).
        const std::uint64_t om_hits = xs::list_slot_hits(50), dsv_hits = xs::list_slot_hits(51);
        const std::uint64_t rtv_hits = xs::list_slot_hits(52);
        void** lv = *reinterpret_cast<void***>(list.p);
        // Slot 50, single range: rtvs[0] names both title descriptors. Then the array form, title and native mixed.
        reinterpret_cast<OmSetFn>(lv[50])(list.p, 2, &rtvs[0], TRUE, &dsv);
        reinterpret_cast<OmSetFn>(lv[50])(list.p, 3, rtvs, FALSE, &dsv);
        // Slot 51, typed: FLOAT Depth is the fourth argument, so it travels in XMM3.
        reinterpret_cast<ClearDsvFn>(lv[51])(list.p, dsv, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
        // Slot 52: a distinct colour per descriptor, so an offset or heap mix-up cannot pass.
        const FLOAT colors[3][4] = {{0.0f, 1.0f, 64.0f / 255.0f, 1.0f},
                                    {1.0f, 0.0f, 128.0f / 255.0f, 1.0f},
                                    {32.0f / 255.0f, 96.0f / 255.0f, 192.0f / 255.0f, 1.0f}};
        const std::uint8_t expected[3][4] = {{0, 255, 64, 255}, {255, 0, 128, 255}, {32, 96, 192, 255}};
        for (unsigned i = 0; i < 3; ++i) reinterpret_cast<ClearRtvFn>(lv[52])(list.p, rtvs[i], colors[i], 0, nullptr);
        CHECK(xs::list_proxy_unsupported_slot(list.p) == 0);

        D3D12_RESOURCE_BARRIER barriers[4]{};
        for (unsigned i = 0; i < 4; ++i) {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.pResource = images[i];
            barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[i].Transition.StateBefore =
                i < 3 ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        reinterpret_cast<BarrierFn>(lv[26])(list.p, 4, barriers);
        for (unsigned i = 0; i < 4; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprints[i];
            src.pResource = images[i];
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            reinterpret_cast<CopyTextureFn>(lv[16])(list.p, &dst, 0, 0, 0, &src, nullptr);
        }
        CHECK(xs::list_proxy_unsupported_slot(list.p) == 0);
        CHECK(xs::list_slot_hits(50) == om_hits + 2 && xs::list_slot_hits(51) == dsv_hits + 1 &&
              xs::list_slot_hits(52) == rtv_hits + 3);
        CHECK(xs::refused_descriptor_handle_calls() == refused_before);
        CHECK(reinterpret_cast<CloseFn>(lv[9])(list.p) == S_OK);

        ID3D12CommandList* lists[] = {submission.p};
        queue->ExecuteCommandLists(1, lists);
        CHECK(SUCCEEDED(queue->Signal(fence.p, 1)));
        if (wait_for(fence.p, 1)) return 1;

        // Every pixel and every depth value, not a sample.
        const D3D12_RANGE read{0, static_cast<SIZE_T>(readback_size)};
        CHECK(SUCCEEDED(readback->Map(0, &read, &mapped)) && mapped);
        const auto* bytes = static_cast<const std::uint8_t*>(mapped);
        unsigned wrong = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f = footprints[i];
            for (unsigned y = 0; y < 4; ++y) {
                for (unsigned x = 0; x < 4; ++x) {
                    const std::uint8_t* texel = bytes + f.Offset + UINT64(y) * f.Footprint.RowPitch + x * 4u;
                    if (i < 3) {
                        for (unsigned c = 0; c < 4; ++c) {
                            if (texel[c] == expected[i][c]) {
                                ++color_bytes;
                            } else if (wrong++ < 8) {
                                std::fprintf(stderr, "target %u pixel (%u,%u) channel %u: %u, expected %u\n", i, x, y,
                                             c, texel[c], expected[i][c]);
                            }
                        }
                    } else {
                        float value = -1.0f;
                        std::memcpy(&value, texel, sizeof(value));
                        if (value == 0.25f) {
                            ++depth_values;
                        } else if (wrong++ < 8) {
                            std::fprintf(stderr, "depth (%u,%u): %.9g, expected 0.25\n", x, y,
                                         static_cast<double>(value));
                        }
                    }
                }
            }
        }
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        CHECK(wrong == 0 && color_bytes == 3u * 16u * 4u && depth_values == 16u);

        // Release order: the executed list and its objects, the resources, then the title-backed heaps (whose
        // backings go back through title_release), the native heap, and last the allocator (scope).
        submission.reset();
        list.reset();
        list_allocator.reset();
        queue.reset();
        fence.reset();
        readback.reset();
        depth.reset();
        for (auto& target : targets) target.reset();
        CHECK(g_title.frees == 0);
        rtv_heap.reset();
        dsv_heap.reset();
        CHECK(g_title.frees == 2 && g_title.freed[0] && g_title.freed[1] && g_title.bad_calls == 0);
        CHECK(xs::title_descriptor_range_count() == ranges_before);
        CHECK(xs::tracked_descriptor_heap_count() == tracked_before);
        native.reset();
    }
    CHECK(xs::device_proxy_ref_count(device) == device_refs);
    std::printf("title-backed RTV/DSV heaps (slot 14, explicit allocator) and a native backend RTV heap: views via "
                "slots 20/21, console list 73 slots 50 (single range and array)/51/52; %u/192 colour bytes and "
                "%u/16 depth values match (RGBA 255,0,128,255 on the title RTV, Depth 0.25); both backings freed once\n",
                color_bytes, depth_values);
    return 0;
}

int run(vktest::Device& d) {
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    ID3D12Device* inner = nullptr;
    CHECK(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))) && inner);
    inner->AddRef();  // the test's own reference; the proxy takes the other
    ID3D12Device* device = nullptr;
    if (FAILED(xs::create_device_proxy(inner, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        if (device) device->Release();
        inner->Release();
        inner->Release();
        std::fprintf(stderr, "descriptor_proxy_gpu_test: device proxy creation failed\n");
        return 1;
    }
    rt::d3d12::set_device_outer(inner, device);
    const int result = check_title_descriptors(d, device, inner);
    device->Release();
    const ULONG remaining = inner->Release();
    if (result) return 1;
    CHECK(remaining == 0);  // nothing the proxies or the heaps made kept the layer's device alive
    return 0;
}

void print_device(const vktest::Device& d) {
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    const auto get = d.instance_fn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
    if (get) get(d.physical, &properties);
    std::printf("physical device: %s (vendor 0x%04x, device 0x%04x); driver: %s, %s (driverVersion 0x%08x); "
                "Vulkan validation %s\n",
                properties.properties.deviceName, properties.properties.vendorID, properties.properties.deviceID,
                driver.driverName, driver.driverInfo, properties.properties.driverVersion,
                d.validation ? "on (synchronization validation)" : "off (set PALMWAVE_VK_VALIDATION=1)");
}

}  // namespace

// A failure ends the process with a message on stderr, never in a dialog a test run would wait on.
static void no_dialogs() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

int main() {
    no_dialogs();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool validation = false;
    int result = 1;
    {
        vktest::Device d;
        const vktest::DeviceStatus status = d.initialize();
        if (status == vktest::DeviceStatus::unavailable) {
            std::printf("SKIP descriptor_proxy_gpu_test: no Vulkan device\n");
            return 77;
        }
        if (status != vktest::DeviceStatus::ready) {
            std::fprintf(stderr, "descriptor_proxy_gpu_test: Vulkan device creation failed\n");
            return 1;
        }
        validation = d.validation;
        print_device(d);
        result = run(d);
    }  // the VkDevice, then the debug messenger, are destroyed here: reports up to this point are counted below
    if (result) return 1;
    const unsigned errors = vktest::validation_error_count();
    if (errors) {
        std::fprintf(stderr, "descriptor_proxy_gpu_test: %u Vulkan validation errors (counted through device "
                             "destruction)\n", errors);
        return 1;
    }
    std::printf("descriptor_proxy_gpu_test: passed; validation %s\n",
                validation ? "on, 0 errors through resource and device destruction" : "off");
    return 0;
}
