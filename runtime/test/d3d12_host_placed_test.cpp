// Xbox device slot 133 (CreatePlacedResourceX) on the Vulkan layer: a buffer whose memory is the caller's own pages,
// imported with VK_EXT_external_memory_host (rt::d3d12::create_buffer_at_host_address, docs/d3d12_vulkan.md 4.2).
// Synthetic memory only; no shaders, no game data. Exits 77 only without a Vulkan device or without the extension;
// every other failure, and every validation-layer message, fails the test.
//
// Every positive range comes from a served title shim, as the title's would: kernel32!VirtualAlloc (the registered
// shim), XMemVirtualAlloc + mapped physical pages, and an XMemAlloc block.
//   - round trips: bytes the CPU wrote before creation and bytes it wrote after creation are what a GPU copy reads;
//     bytes a GPU copy writes into the buffer are what the CPU then reads at the same address, with no Map or upload
//     step; on a title VirtualAlloc range, 256 bytes into a page, and inside an XMemAlloc block;
//   - xmem physical pages: while the resource lives, unmap, remap, free and the title's VirtualFree, VirtualProtect and
//     UnmapViewOfFile are refused (ERROR_BUSY) and the pages keep their contents; after Release the pin is gone, the
//     pages remap in reverse order and a new resource on them reads the reordered bytes; a second address that maps a
//     page of a live import is refused;
//   - shared imports: identical and contained ranges share one import and outlive each other's Release; a partial
//     overlap is refused;
//   - refusals with a null output and nothing imported or pinned: memory no shim owns (a raw VirtualAlloc, a section
//     view, a range past an XMemAlloc block), a texture, every malformed buffer field, unimplemented flags, an initial
//     state other than COMMON (the retail 0xFFFFFFFF included), no address, overflowing widths, reserved, read-only,
//     guarded and partly committed pages, uncached and write-combined pages;
//   - through the Xbox-ABI proxy's slot 133: the implemented call shape makes a buffer; console flag bits, a clear value
//     and the retail state word are refused without a resource (and, by default, the experimental tuple too);
//   - raw device slot 70 over that import into a title-backed heap: the real slot gets PC slot 18's raw V# for the live
//     buffer; refusals leave the slot untouched. A shader read through it is not run (see check_slot70).
//
// --experimental (CTest sets PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT=1 for the process; the test never sets it): the same
// checks, which the experiment must not change, plus one DIAGNOSTIC case (experimental_host_import.h). xmem physical
// pages in a window that asks for write-combine are mapped as cached views; the exact experimental tuple through raw slot
// 133 imports them; CPU bytes written before and after the import are what a GPU copy reads, and a GPU copy into the
// buffer, after its fence, is what the CPU reads at the same VA. This proves buffer visibility through copies only: not a
// shader's V# addressing, not shader UAV writes, not the console's meaning of 0x20000 or of the state word.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_trace.h"
#include "../shims/d3d12_xs_device.h"
#include "../shims/experimental_host_import.h"
#include "../shims/host_pins.h"
#include "../shims/shims.h"
#include "../src/gfx10_descriptors.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "%s line %d: %s\n", who, __LINE__, #condition); return 1; } \
} while (0)
#define EXPECT_HR(expr, want) do { \
    const HRESULT got_ = (expr); \
    if (got_ != (want)) { \
        std::fprintf(stderr, "%s line %d: %s = 0x%08lX, expected 0x%08lX\n", who, __LINE__, #expr, \
                     static_cast<unsigned long>(got_), static_cast<unsigned long>(want)); \
        return 1; \
    } \
} while (0)

namespace {

template<class T>
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

constexpr SIZE_T k64 = 0x10000;
constexpr DWORD kXmem64kPages = 0x20400000;
constexpr std::uint32_t kCommon = 0;               // D3D12_RESOURCE_STATE_COMMON: the one initial state accepted
constexpr std::uint32_t kRetailStateWord = 0xFFFFFFFFu;  // measured on retail; its meaning is not established

using XMemVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
using XMemAllocatePhysicalPagesFn = BOOL(WINAPI*)(uint64_t, uint64_t*, uint64_t*);
using XMemFreePhysicalPagesFn = BOOL(WINAPI*)(uint64_t, const uint64_t*);
using XMemMapPhysicalPagesFn = void*(WINAPI*)(void*, uint64_t, const uint64_t*);
using XMemAllocFn = void*(__stdcall*)(size_t, uint64_t);
using XMemFreeFn = void(__stdcall*)(void*, uint64_t);
using TitleVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, DWORD);
using TitleVirtualFreeFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD);
using TitleVirtualProtectFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD, DWORD*);
using TitleUnmapViewOfFileFn = BOOL(WINAPI*)(const void*);

// The served title memory calls (the registered shims, not the host's).
struct Title {
    TitleVirtualAllocFn alloc;
    TitleVirtualFreeFn free;
    TitleVirtualProtectFn protect;
    TitleUnmapViewOfFileFn unmap;
    XMemAllocFn xmem_alloc;
    XMemFreeFn xmem_free;
};

void fill(void* at, size_t bytes, unsigned seed) {
    auto* p = static_cast<std::uint8_t*>(at);
    for (size_t i = 0; i < bytes; ++i) p[i] = static_cast<std::uint8_t>(i * 131u + seed);
}

bool holds(const void* at, size_t bytes, unsigned seed) {
    const auto* p = static_cast<const std::uint8_t*>(at);
    for (size_t i = 0; i < bytes; ++i)
        if (p[i] != static_cast<std::uint8_t>(i * 131u + seed)) return false;
    return true;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 width) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = width;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

HRESULT place(ID3D12Device* device, void* va, const D3D12_RESOURCE_DESC& desc, ID3D12Resource** out,
              std::uint32_t state = kCommon) {
    return rt::d3d12::create_buffer_at_host_address(device, va, desc, state, __uuidof(ID3D12Resource),
                                                    reinterpret_cast<void**>(out));
}

// One queue, one list, a fence: copies that are waited for before they return.
struct Gpu {
    ID3D12Device* device = nullptr;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    Ref<ID3D12Resource> upload;
    Ref<ID3D12Resource> readback;
    UINT64 value = 0;
    HANDLE event = nullptr;
    static constexpr UINT64 kStaging = 4 * k64;

    ~Gpu() {
        if (event) CloseHandle(event);
    }

    int init(ID3D12Device* d) {
        const char* who = "gpu setup";
        device = d;
        const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(list->Close()));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        EXPECT(event != nullptr);
        const D3D12_RESOURCE_DESC desc = buffer_desc(kStaging);
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&upload.p))));
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        return 0;
    }

    int copy(ID3D12Resource* dst, ID3D12Resource* src, UINT64 bytes) {
        const char* who = "gpu copy";
        EXPECT(SUCCEEDED(allocator->Reset()));
        EXPECT(SUCCEEDED(list->Reset(allocator.p, nullptr)));
        list->CopyBufferRegion(dst, 0, src, 0, bytes);
        EXPECT(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, ++value)));
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(value, event)));
        EXPECT(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
        return 0;
    }

    // The GPU's view of `placed` (its first `bytes`) is pattern `seed`.
    int gpu_reads(ID3D12Resource* placed, UINT64 bytes, unsigned seed) {
        const char* who = "gpu read";
        EXPECT(bytes <= kStaging);
        if (copy(readback.p, placed, bytes)) return 1;
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
        EXPECT(SUCCEEDED(readback->Map(0, &range, &mapped)));
        const bool same = holds(mapped, static_cast<size_t>(bytes), seed);
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        EXPECT(same);
        return 0;
    }

    // A GPU copy writes pattern `seed` into `placed`.
    int gpu_writes(ID3D12Resource* placed, UINT64 bytes, unsigned seed) {
        const char* who = "gpu write";
        EXPECT(bytes <= kStaging);
        void* mapped = nullptr;
        const D3D12_RANGE none{0, 0};
        EXPECT(SUCCEEDED(upload->Map(0, &none, &mapped)));
        fill(mapped, static_cast<size_t>(bytes), seed);
        upload->Unmap(0, nullptr);
        return copy(placed, upload.p, bytes);
    }
};

bool nothing_held(ID3D12Device* device) {
    return rt::d3d12::host_import_count(device) == 0 && rt::host_pins::pin_count() == 0;
}

// A call that must fail: the expected HRESULT, a null output, and nothing imported or pinned afterwards.
bool refused(ID3D12Device* device, void* va, const D3D12_RESOURCE_DESC& desc, HRESULT want,
             std::uint32_t state = kCommon) {
    ID3D12Resource* out = reinterpret_cast<ID3D12Resource*>(1);
    const HRESULT hr = place(device, va, desc, &out, state);
    if (out && out != reinterpret_cast<ID3D12Resource*>(1)) out->Release();
    const bool ok = hr == want && out == nullptr && nothing_held(device);
    if (!ok)
        std::fprintf(stderr, "  refusal: va %p width %llu flags 0x%x state 0x%x -> 0x%08lx (want 0x%08lx)\n", va,
                     static_cast<unsigned long long>(desc.Width), static_cast<unsigned>(desc.Flags), state,
                     static_cast<unsigned long>(hr), static_cast<unsigned long>(want));
    return ok;
}

// CPU before creation -> GPU, CPU after creation -> GPU, GPU -> CPU at the same address.
int check_round_trip(Gpu& gpu, const char* who, void* pages, UINT64 size,
                     D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    fill(pages, static_cast<size_t>(size), 7);
    Ref<ID3D12Resource> placed;
    D3D12_RESOURCE_DESC desc = buffer_desc(size);
    desc.Flags = flags;
    EXPECT_HR(place(gpu.device, pages, desc, &placed.p), S_OK);
    EXPECT(placed.p != nullptr);
    EXPECT(placed->GetGPUVirtualAddress() == reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(pages));
    EXPECT(placed->GetDesc().Flags == flags);
    D3D12_HEAP_PROPERTIES heap{};
    EXPECT(SUCCEEDED(placed->GetHeapProperties(&heap, nullptr)));
    EXPECT(heap.Type == D3D12_HEAP_TYPE_CUSTOM && heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
    void* mapped = nullptr;
    EXPECT(SUCCEEDED(placed->Map(0, nullptr, &mapped)) && mapped == pages);
    EXPECT((rt::d3d12::vulkan_memory_flags(placed.p) &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    if (gpu.gpu_reads(placed.p, size, 7)) return 1;   // written before the resource existed
    fill(pages, static_cast<size_t>(size), 91);
    if (gpu.gpu_reads(placed.p, size, 91)) return 1;  // written after, no upload, no Map
    if (gpu.gpu_writes(placed.p, size, 203)) return 1;
    EXPECT(holds(pages, static_cast<size_t>(size), 203));  // the GPU's bytes, read by the CPU where it put them
    placed.reset();
    EXPECT(nothing_held(gpu.device));
    std::printf("%s, %llu bytes: CPU before/after creation -> GPU and GPU -> CPU match, nothing held after Release\n",
                who, static_cast<unsigned long long>(size));
    return 0;
}

int check_round_trips(Gpu& gpu, const Title& title, VkDeviceSize host_alignment) {
    constexpr SIZE_T size = 4 * k64;
    int failures = 0;
    void* pages = title.alloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!pages) return 1;
    failures += check_round_trip(gpu, "title VirtualAlloc range", pages, size);
    failures += check_round_trip(gpu, "title VirtualAlloc range, UAV | DENY_SHADER_RESOURCE", pages, k64,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    // Inside a page: the import starts at the aligned page and the buffer is bound at the offset, where the driver's
    // binding alignment allows it; where it does not, the refusal must be clean.
    {
        const char* who = "256 bytes into a page";
        void* inner = static_cast<char*>(pages) + 256;
        Ref<ID3D12Resource> probe;
        const HRESULT hr = place(gpu.device, inner, buffer_desc(0x1000 - 256), &probe.p);
        probe.reset();
        if (hr == E_NOTIMPL) {
            if (!nothing_held(gpu.device)) return failures + 1;
            std::printf("%s: the driver's buffer alignment refuses this offset; refused cleanly\n", who);
        } else if (hr != S_OK) {
            std::fprintf(stderr, "%s: 0x%08lx\n", who, static_cast<unsigned long>(hr));
            ++failures;
        } else {
            failures += check_round_trip(gpu, who, inner, 0x1000 - 256);
        }
    }
    if (!title.free(pages, 0, MEM_RELEASE)) ++failures;

    // An XMemAlloc block: an import wholly inside the live block works; one whose rounded range runs past it is refused.
    {
        const char* who = "XMemAlloc block";
        constexpr size_t kBlock = 3 * k64;
        auto* const block = static_cast<std::uint8_t*>(title.xmem_alloc(kBlock, 0));
        EXPECT(block != nullptr);
        if (host_alignment <= k64) {
            const std::uintptr_t a = static_cast<std::uintptr_t>(host_alignment);
            auto* const inner = reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(block) + a - 1) & ~(a - 1));
            failures += check_round_trip(gpu, who, inner, k64);
            EXPECT(refused(gpu.device, block + kBlock - 0x100, buffer_desc(0x1000), E_NOTIMPL));
        } else {
            std::printf("%s: minImportedHostPointerAlignment %llu exceeds the test's 64 KiB slack; not checked\n", who,
                        static_cast<unsigned long long>(host_alignment));
        }
        title.xmem_free(block, 0);
    }
    return failures;
}

// Memory no served shim owns is refused before anything is imported, whatever its page state.
int check_unmanaged(Gpu& gpu) {
    const char* who = "unmanaged memory";
    void* const raw = VirtualAlloc(nullptr, k64, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    EXPECT(raw != nullptr);
    const bool raw_refused = refused(gpu.device, raw, buffer_desc(k64), E_NOTIMPL);
    VirtualFree(raw, 0, MEM_RELEASE);
    EXPECT(raw_refused);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(k64), nullptr);
    void* const view = section ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, k64) : nullptr;
    EXPECT(view != nullptr);
    const bool view_refused = refused(gpu.device, view, buffer_desc(k64), E_NOTIMPL);
    UnmapViewOfFile(view);
    CloseHandle(section);
    EXPECT(view_refused);
    std::printf("%s: a raw host VirtualAlloc and a raw section view are refused (E_NOTIMPL), nothing held\n", who);
    return 0;
}

// Shared imports: identical and contained ranges share one import, a partial overlap is refused, a share outlives
// another's Release, and the title's VirtualFree of the range is refused until the last one is gone.
int check_shared(Gpu& gpu, const Title& title) {
    const char* who = "shared imports";
    auto* const pages = static_cast<std::uint8_t*>(
        title.alloc(nullptr, 6 * k64, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    EXPECT(pages != nullptr);
    Ref<ID3D12Resource> a, b, c, d;
    EXPECT_HR(place(gpu.device, pages, buffer_desc(4 * k64), &a.p), S_OK);
    EXPECT_HR(place(gpu.device, pages, buffer_desc(4 * k64), &b.p), S_OK);
    EXPECT_HR(place(gpu.device, pages + k64, buffer_desc(k64), &c.p), S_OK);
    EXPECT(rt::d3d12::host_import_count(gpu.device) == 1 && rt::host_pins::pin_count() == 1);
    EXPECT_HR(place(gpu.device, pages + 3 * k64, buffer_desc(2 * k64), &d.p), E_NOTIMPL);
    EXPECT(d.p == nullptr && rt::d3d12::host_import_count(gpu.device) == 1);
    SetLastError(ERROR_SUCCESS);
    EXPECT(title.free(pages, 0, MEM_RELEASE) == FALSE && GetLastError() == ERROR_BUSY);
    a.reset();
    fill(pages, 4 * k64, 17);
    if (gpu.gpu_reads(b.p, 4 * k64, 17)) return 1;  // b still reads the pages after a's Release
    b.reset();
    fill(pages + k64, k64, 29);
    if (gpu.gpu_reads(c.p, k64, 29)) return 1;
    EXPECT(rt::d3d12::host_import_count(gpu.device) == 1);
    c.reset();
    EXPECT(nothing_held(gpu.device));
    EXPECT(title.free(pages, 0, MEM_RELEASE) != FALSE);
    std::printf("%s: identical and contained ranges share one import, a partial overlap is refused, the title's "
                "VirtualFree waits for the last Release\n", who);
    return 0;
}

struct XMem {
    XMemVirtualAllocFn virtual_alloc;
    XMemAllocatePhysicalPagesFn allocate;
    XMemFreePhysicalPagesFn free_pages;
    XMemMapPhysicalPagesFn map;
};

// The title's physical pages: refusals while the import lives, release, reverse remap, a new import, an alias.
int check_xmem_lifecycle(Gpu& gpu, const XMem& x, const Title& title) {
    const char* who = "xmem physical pages";
    constexpr uint64_t kPages = 4;
    constexpr UINT64 size = kPages * k64;
    auto* const window = static_cast<std::uint8_t*>(
        x.virtual_alloc(nullptr, size, MEM_RESERVE | kXmem64kPages, 0, PAGE_READWRITE));
    auto* const alias = static_cast<std::uint8_t*>(
        x.virtual_alloc(nullptr, k64, MEM_RESERVE | kXmem64kPages, 0, PAGE_READWRITE));
    EXPECT(window && alias);
    uint64_t pages[kPages]{};
    uint64_t count = kPages;
    EXPECT(x.allocate(0, &count, pages) == TRUE && count == kPages);
    EXPECT(x.map(window, kPages, pages) == window);
    for (uint64_t i = 0; i < kPages; ++i) fill(window + i * k64, k64, 40 + unsigned(i));

    Ref<ID3D12Resource> placed;
    EXPECT_HR(place(gpu.device, window, buffer_desc(size), &placed.p), S_OK);
    {
        std::vector<std::uint8_t> expect(size);
        for (uint64_t i = 0; i < kPages; ++i) fill(expect.data() + i * k64, k64, 40 + unsigned(i));
        if (gpu.copy(gpu.readback.p, placed.p, size)) return 1;
        void* mapped = nullptr;
        const D3D12_RANGE range{0, size};
        EXPECT(SUCCEEDED(gpu.readback->Map(0, &range, &mapped)));
        const bool same = std::memcmp(mapped, expect.data(), size) == 0;
        gpu.readback->Unmap(0, nullptr);
        EXPECT(same);
    }

    // While the import lives: nothing served pulls the pages away or changes their protection.
    const auto busy = [] { return GetLastError() == ERROR_BUSY; };
    SetLastError(ERROR_SUCCESS);
    EXPECT(x.map(window, kPages, nullptr) == nullptr && busy());
    const uint64_t reversed[kPages] = {pages[3], pages[2], pages[1], pages[0]};
    SetLastError(ERROR_SUCCESS);
    EXPECT(x.map(window, kPages, reversed) == nullptr && busy());
    SetLastError(ERROR_SUCCESS);
    EXPECT(x.free_pages(kPages, pages) == FALSE && busy());
    SetLastError(ERROR_SUCCESS);
    EXPECT(title.free(window, 0, MEM_RELEASE) == FALSE && busy());
    DWORD old = 0;
    SetLastError(ERROR_SUCCESS);
    EXPECT(title.protect(window + k64, 0x1000, PAGE_READONLY, &old) == FALSE && busy());
    SetLastError(ERROR_SUCCESS);
    EXPECT(title.unmap(window + 2 * k64) == FALSE && busy());
    // The same physical page at a second address maps, but a second import of it is refused.
    EXPECT(x.map(alias, 1, &pages[0]) == alias);
    {
        Ref<ID3D12Resource> aliased;
        EXPECT_HR(place(gpu.device, alias, buffer_desc(k64), &aliased.p), E_NOTIMPL);
        EXPECT(aliased.p == nullptr && rt::d3d12::host_import_count(gpu.device) == 1 &&
               rt::host_pins::pin_count() == 1);
    }
    EXPECT(x.map(alias, 1, nullptr) == alias);
    fill(window, size, 77);
    if (gpu.gpu_reads(placed.p, size, 77)) return 1;  // written after creation, through the title's own mapping

    // Release, then the title's remap order changes what both the CPU and a new import see.
    placed.reset();
    EXPECT(nothing_held(gpu.device));
    for (uint64_t i = 0; i < kPages; ++i) fill(window + i * k64, k64, 60 + unsigned(i));
    EXPECT(x.map(window, kPages, nullptr) == window);
    EXPECT(x.map(window, kPages, reversed) == window);
    EXPECT_HR(place(gpu.device, window, buffer_desc(size), &placed.p), S_OK);
    {
        std::vector<std::uint8_t> expect(size);
        for (uint64_t i = 0; i < kPages; ++i) fill(expect.data() + i * k64, k64, 60 + unsigned(kPages - 1 - i));
        EXPECT(std::memcmp(window, expect.data(), size) == 0);
        if (gpu.copy(gpu.readback.p, placed.p, size)) return 1;
        void* mapped = nullptr;
        const D3D12_RANGE range{0, size};
        EXPECT(SUCCEEDED(gpu.readback->Map(0, &range, &mapped)));
        const bool same = std::memcmp(mapped, expect.data(), size) == 0;
        gpu.readback->Unmap(0, nullptr);
        EXPECT(same);
    }
    placed.reset();
    EXPECT(nothing_held(gpu.device));
    EXPECT(x.map(window, kPages, nullptr) == window);
    EXPECT(x.free_pages(kPages, pages) == TRUE);
    std::printf("%s: unmap, remap, free, VirtualFree, VirtualProtect and UnmapViewOfFile refused while imported, an "
                "alias import refused, release then reverse remap then a new import reads the reordered pages\n", who);
    return 0;
}

// Every refusal: E_* with a null output, nothing imported, nothing pinned. Page-state cases use served title memory,
// so they reach the page checks rather than the ownership refusal.
int check_refusals(Gpu& gpu, const Title& title) {
    const char* who = "refusals";
    ID3D12Device* const device = gpu.device;
    auto* const good = static_cast<std::uint8_t*>(
        title.alloc(nullptr, 2 * k64, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    EXPECT(good != nullptr);

    // Not a buffer: no committed stand-in texture.
    D3D12_RESOURCE_DESC texture = buffer_desc(64);
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Height = 64;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    EXPECT(refused(device, good, texture, E_NOTIMPL));

    // Every malformed buffer field is E_INVALIDARG.
    {
        D3D12_RESOURCE_DESC d = buffer_desc(k64);
        d.Height = 2;
        EXPECT(refused(device, good, d, E_INVALIDARG));
        d = buffer_desc(k64);
        d.DepthOrArraySize = 2;
        EXPECT(refused(device, good, d, E_INVALIDARG));
        d = buffer_desc(k64);
        d.MipLevels = 0;
        EXPECT(refused(device, good, d, E_INVALIDARG));
        d = buffer_desc(k64);
        d.Format = DXGI_FORMAT_R8_UNORM;
        EXPECT(refused(device, good, d, E_INVALIDARG));
        d = buffer_desc(k64);
        d.SampleDesc.Count = 2;
        EXPECT(refused(device, good, d, E_INVALIDARG));
        d = buffer_desc(k64);
        d.SampleDesc.Quality = 1;
        EXPECT(refused(device, good, d, E_INVALIDARG));
        d = buffer_desc(k64);
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        EXPECT(refused(device, good, d, E_INVALIDARG));
        d = buffer_desc(k64);
        d.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
        EXPECT(refused(device, good, d, E_INVALIDARG));
        d = buffer_desc(k64);
        d.Alignment = 0x1000;
        EXPECT(refused(device, good, d, E_INVALIDARG));
        EXPECT(refused(device, good, buffer_desc(0), E_INVALIDARG));
    }
    // Flags this path does not implement, and initial states other than COMMON (the retail word included).
    for (const UINT flags : {UINT(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), UINT(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                             UINT(D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER), UINT(D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS),
                             0x100u, 0x400u, 0x20000u, 0x20004u}) {
        D3D12_RESOURCE_DESC d = buffer_desc(k64);
        d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
        EXPECT(refused(device, good, d, E_NOTIMPL));
    }
    for (const std::uint32_t state : {kRetailStateWord, std::uint32_t(D3D12_RESOURCE_STATE_GENERIC_READ),
                                      std::uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)})
        EXPECT(refused(device, good, buffer_desc(k64), E_NOTIMPL, state));

    EXPECT(refused(device, nullptr, buffer_desc(k64), E_INVALIDARG));
    EXPECT(refused(device, good, buffer_desc(~UINT64(0)), E_INVALIDARG));
    EXPECT(refused(device, good, buffer_desc(~UINT64(0) - reinterpret_cast<std::uintptr_t>(good)), E_INVALIDARG));
    EXPECT(rt::d3d12::create_buffer_at_host_address(device, good, buffer_desc(k64), kCommon, __uuidof(ID3D12Resource),
                                                    nullptr) == E_POINTER);

    // Page state, on served title memory.
    struct Pages {
        const char* name;
        DWORD reserve_only;  // MEM_RESERVE alone
        DWORD protect;
    };
    const Pages cases[] = {
        {"reserved only", 1, PAGE_READWRITE},
        {"read-only", 0, PAGE_READONLY},
        {"guard page", 0, PAGE_READWRITE | PAGE_GUARD},
    };
    for (const Pages& c : cases) {
        void* p = title.alloc(nullptr, k64, c.reserve_only ? MEM_RESERVE : (MEM_RESERVE | MEM_COMMIT), c.protect);
        EXPECT(p != nullptr);
        const bool ok = refused(device, p, buffer_desc(k64), E_INVALIDARG);
        title.free(p, 0, MEM_RELEASE);
        if (!ok) std::fprintf(stderr, "%s: %s pages were not refused with E_INVALIDARG\n", who, c.name);
        EXPECT(ok);
    }
    void* partial = title.alloc(nullptr, 2 * k64, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    EXPECT(partial && title.alloc(partial, k64, MEM_COMMIT, PAGE_READWRITE) == partial);
    EXPECT(refused(device, partial, buffer_desc(2 * k64), E_INVALIDARG));
    title.free(partial, 0, MEM_RELEASE);
    // Cache-modified pages: refused where the host reports the modifier (a WC import read wrong GPU bytes upstream).
    for (const DWORD modifier : {DWORD(PAGE_NOCACHE), DWORD(PAGE_WRITECOMBINE)}) {
        void* p = title.alloc(nullptr, k64, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE | modifier);
        EXPECT(p != nullptr);
        MEMORY_BASIC_INFORMATION region{};
        EXPECT(VirtualQuery(p, &region, sizeof(region)) == sizeof(region));
        if (region.Protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE)) {
            const bool ok = refused(device, p, buffer_desc(k64), E_NOTIMPL);
            title.free(p, 0, MEM_RELEASE);
            EXPECT(ok);
        } else {
            std::printf("%s: modifier 0x%lx not reported by the host (protection 0x%lx); not checked\n", who,
                        static_cast<unsigned long>(modifier), static_cast<unsigned long>(region.Protect));
            title.free(p, 0, MEM_RELEASE);
        }
    }
    title.free(good, 0, MEM_RELEASE);
    EXPECT(nothing_held(device));
    std::printf("%s: texture, every malformed buffer field, unimplemented flags and initial states, null, zero, "
                "overflowing, partly committed, reserved, read-only, guarded and cache-modified cases fail with a null "
                "output and nothing held\n", who);
    return 0;
}

// The title's call shape through the proxy's slot 133.
int check_proxy(Gpu& gpu, void* proxy, const Title& title) {
    const char* who = "proxy slot 133";
    auto* const pages = title.alloc(nullptr, k64, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    EXPECT(pages != nullptr);
    fill(pages, k64, 5);
    D3D12_RESOURCE_DESC xbox = buffer_desc(k64);
    xbox.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);  // the console's row-major word (= PC ROW_MAJOR)
    xbox.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const GUID iid = __uuidof(ID3D12Resource);
    using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                          std::uint64_t, std::uint64_t);
    void* const* vtable = *static_cast<void* const* const*>(proxy);
    const auto slot = reinterpret_cast<SlotFn>(vtable[133]);
    const auto call = [&](const D3D12_RESOURCE_DESC& desc, std::uint32_t state, const void* clear,
                          ID3D12Resource** out) {
        return static_cast<HRESULT>(static_cast<std::uint32_t>(
            slot(proxy, reinterpret_cast<std::uint64_t>(pages), reinterpret_cast<std::uint64_t>(&desc), state,
                 reinterpret_cast<std::uint64_t>(clear), reinterpret_cast<std::uint64_t>(&iid),
                 reinterpret_cast<std::uint64_t>(out))));
    };
    ID3D12Resource* placed = nullptr;
    EXPECT(call(xbox, kCommon, nullptr, &placed) == S_OK && placed != nullptr);
    EXPECT(placed->GetGPUVirtualAddress() == reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(pages));
    EXPECT(placed->GetDesc().Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const int read = gpu.gpu_reads(placed, k64, 5);
    placed->Release();
    EXPECT(read == 0 && nothing_held(gpu.device));

    // Refused with a null output and nothing held: the retail state word (by the device), console flag bits and a
    // clear value (by the proxy, which does not call the device).
    const auto refused_through_proxy = [&](const D3D12_RESOURCE_DESC& desc, std::uint32_t state, const void* clear,
                                           HRESULT want) {
        ID3D12Resource* out = reinterpret_cast<ID3D12Resource*>(1);
        const HRESULT hr = call(desc, state, clear, &out);
        if (out && out != reinterpret_cast<ID3D12Resource*>(1)) out->Release();
        return hr == want && out == nullptr && nothing_held(gpu.device);
    };
    EXPECT(refused_through_proxy(xbox, kRetailStateWord, nullptr, E_NOTIMPL));
    D3D12_RESOURCE_DESC console_bits = xbox;
    console_bits.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20004);
    EXPECT(refused_through_proxy(console_bits, kCommon, nullptr, E_NOTIMPL));
    const D3D12_CLEAR_VALUE clear{};
    EXPECT(refused_through_proxy(xbox, kCommon, &clear, E_INVALIDARG));
    if (!rt::experimental_host_import::enabled()) {
        // By default the exact experimental tuple is refused like any console bit (check_experimental covers opt-in).
        D3D12_RESOURCE_DESC tuple = xbox;
        tuple.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20004);
        EXPECT(refused_through_proxy(tuple, kRetailStateWord, nullptr, E_NOTIMPL));
    }
    title.free(pages, 0, MEM_RELEASE);
    std::printf("%s: the implemented call shape makes a buffer on the title's pages; the retail state word, console "
                "flag bits and a clear value are refused with nothing held\n", who);
    return 0;
}

// Raw device slot 133 as the title calls it: (va, desc, state word, clear, riid, out).
HRESULT call_slot133(void* proxy, void* va, const D3D12_RESOURCE_DESC* desc, std::uint32_t state, const void* clear,
                     ID3D12Resource** out) {
    using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                          std::uint64_t, std::uint64_t);
    static const GUID iid = __uuidof(ID3D12Resource);
    void* const* vtable = *static_cast<void* const* const*>(proxy);
    return static_cast<HRESULT>(static_cast<std::uint32_t>(reinterpret_cast<SlotFn>(vtable[133])(
        proxy, reinterpret_cast<std::uint64_t>(va), reinterpret_cast<std::uint64_t>(desc), state,
        reinterpret_cast<std::uint64_t>(clear), reinterpret_cast<std::uint64_t>(&iid),
        reinterpret_cast<std::uint64_t>(out))));
}

// PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT=1 only: the exact experimental tuple through raw slot 133 on xmem physical pages
// whose window asks for write-combine. A diagnostic experiment; see the file comment for what it does not prove.
int check_experimental(Gpu& gpu, void* proxy, const XMem& x, const Title& title) {
    const char* who = "EXPERIMENTAL slot 133 host import";
    constexpr uint64_t kPages = 2;
    constexpr UINT64 size = kPages * k64;
    constexpr DWORD kWriteCombine = PAGE_READWRITE | PAGE_WRITECOMBINE;
    auto* const window = static_cast<std::uint8_t*>(
        x.virtual_alloc(nullptr, size, MEM_RESERVE | kXmem64kPages, 0, kWriteCombine));
    auto* const alias = static_cast<std::uint8_t*>(
        x.virtual_alloc(nullptr, k64, MEM_RESERVE | kXmem64kPages, 0, kWriteCombine));
    EXPECT(window && alias);
    uint64_t pages[kPages]{};
    uint64_t count = kPages;
    EXPECT(x.allocate(0, &count, pages) == TRUE && count == kPages);
    EXPECT(x.map(window, kPages, pages) == window);
    DWORD logical = 0;
    EXPECT(rt::experimental_host_import::xmem_window_protection(window, &logical) && logical == kWriteCombine);
    for (uint64_t i = 0; i < kPages; ++i) {
        MEMORY_BASIC_INFORMATION region{};
        EXPECT(VirtualQuery(window + i * k64, &region, sizeof(region)) == sizeof(region));
        EXPECT(region.State == MEM_COMMIT && region.Protect == PAGE_READWRITE);  // a normal cached host view
    }
    fill(window, size, 11);  // CPU bytes written before the import

    // The exact tuple, with garbage in the padding DWORD at +4 (written through bytes; it is not a field).
    D3D12_RESOURCE_DESC tuple = buffer_desc(size);
    tuple.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);
    tuple.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x00020004);
    const std::uint32_t padding = 0x5EC0DE11u;
    std::memcpy(reinterpret_cast<unsigned char*>(&tuple) + 4, &padding, sizeof(padding));
    unsigned char original[sizeof(tuple)];
    std::memcpy(original, &tuple, sizeof(tuple));

    ID3D12Resource* placed = nullptr;
    EXPECT_HR(call_slot133(proxy, window, &tuple, kRetailStateWord, nullptr, &placed), S_OK);
    EXPECT(placed != nullptr);
    EXPECT(std::memcmp(original, &tuple, sizeof(tuple)) == 0);  // the title's description, byte for byte
    EXPECT(placed->GetGPUVirtualAddress() == reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(window));
    EXPECT(placed->GetDesc().Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS && placed->GetDesc().Width == size);
    EXPECT((rt::d3d12::vulkan_memory_flags(placed) &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    EXPECT(rt::d3d12::host_import_count(gpu.device) == 1 && rt::host_pins::pin_count() == 1);
    int failures = 0;
    const auto release = [&] {
        if (placed) placed->Release();
        placed = nullptr;
    };

    // CPU before -> GPU copy, CPU after (no Map, no upload) -> GPU copy, GPU copy -> CPU at the same VA after the fence.
    if (gpu.gpu_reads(placed, size, 11)) {
        release();
        return 1;
    }
    fill(window, size, 23);
    if (gpu.gpu_reads(placed, size, 23) || gpu.gpu_writes(placed, size, 37)) {
        release();
        return 1;
    }
    if (!holds(window, size, 37)) {
        std::fprintf(stderr, "%s: the CPU does not see the GPU copy's bytes at the imported VA\n", who);
        ++failures;
    }

    // An alias of page 0 in another write-combine window: the same cached host view, the same bytes; a second import
    // of it is refused (host_pins: aliases), with the live import untouched.
    {
        const bool mapped = x.map(alias, 1, &pages[0]) == alias;
        MEMORY_BASIC_INFORMATION region{};
        const bool cached = mapped && VirtualQuery(alias, &region, sizeof(region)) == sizeof(region) &&
                            region.Protect == PAGE_READWRITE;
        ID3D12Resource* second = reinterpret_cast<ID3D12Resource*>(1);
        D3D12_RESOURCE_DESC alias_tuple = tuple;
        alias_tuple.Width = k64;
        const HRESULT hr = mapped ? call_slot133(proxy, alias, &alias_tuple, kRetailStateWord, nullptr, &second) : S_OK;
        if (second && second != reinterpret_cast<ID3D12Resource*>(1)) second->Release();
        if (!mapped || !cached || !holds(alias, k64, 37) || hr != E_NOTIMPL || second != nullptr ||
            rt::d3d12::host_import_count(gpu.device) != 1 || rt::host_pins::pin_count() != 1) {
            std::fprintf(stderr, "%s: alias: mapped %d cached %d hr 0x%08lx\n", who, mapped, cached,
                         static_cast<unsigned long>(hr));
            ++failures;
        }
        if (mapped && x.map(alias, 1, nullptr) != alias) ++failures;
    }

    // While the import lives: unmap, remap, free and VirtualProtect of its pages are refused.
    {
        const auto busy = [] { return GetLastError() == ERROR_BUSY; };
        SetLastError(ERROR_SUCCESS);
        const bool unmap_refused = x.map(window, kPages, nullptr) == nullptr && busy();
        const uint64_t reversed[kPages] = {pages[1], pages[0]};
        SetLastError(ERROR_SUCCESS);
        const bool remap_refused = x.map(window, kPages, reversed) == nullptr && busy();
        SetLastError(ERROR_SUCCESS);
        const bool free_refused = x.free_pages(kPages, pages) == FALSE && busy();
        DWORD old = 0;
        SetLastError(ERROR_SUCCESS);
        const bool protect_refused = title.protect(window, 0x1000, PAGE_READONLY, &old) == FALSE && busy();
        if (!unmap_refused || !remap_refused || !free_refused || !protect_refused) {
            std::fprintf(stderr, "%s: lifetime refusals: unmap %d remap %d free %d protect %d\n", who, unmap_refused,
                         remap_refused, free_refused, protect_refused);
            ++failures;
        }
    }

    // Near misses stay refused under the opt-in, before the device: nothing new imported.
    {
        const auto near_miss = [&](const D3D12_RESOURCE_DESC& desc, std::uint32_t state, const void* clear) {
            ID3D12Resource* out = reinterpret_cast<ID3D12Resource*>(1);
            const HRESULT hr = call_slot133(proxy, window, &desc, state, clear, &out);
            if (out && out != reinterpret_cast<ID3D12Resource*>(1)) out->Release();
            return hr == E_NOTIMPL && out == nullptr && rt::d3d12::host_import_count(gpu.device) == 1 &&
                   rt::host_pins::pin_count() == 1;
        };
        D3D12_RESOURCE_DESC d = tuple;
        d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20000);
        bool ok = near_miss(d, kRetailStateWord, nullptr);
        d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x2000C);
        ok = ok && near_miss(d, kRetailStateWord, nullptr);
        ok = ok && near_miss(tuple, kCommon, nullptr);
        const D3D12_CLEAR_VALUE clear{};
        ok = ok && near_miss(tuple, kRetailStateWord, &clear);
        d = tuple;
        d.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        ok = ok && near_miss(d, kRetailStateWord, nullptr);
        if (!ok) {
            std::fprintf(stderr, "%s: a near miss of the experimental tuple was not refused cleanly\n", who);
            ++failures;
        }
    }

    // Release: the GPU retires, the import and the pin go, the pages unmap and free.
    release();
    EXPECT(nothing_held(gpu.device));
    EXPECT(x.map(window, kPages, nullptr) == window);
    EXPECT(x.free_pages(kPages, pages) == TRUE);

    // The device's own refusals still apply after the tuple matched: memory no shim owns is refused (E_NOTIMPL) with a
    // null output and nothing held, and that real failure is what slot 133 returns.
    {
        void* const raw = VirtualAlloc(nullptr, k64, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
        EXPECT(raw != nullptr);
        D3D12_RESOURCE_DESC d = tuple;
        d.Width = k64;
        ID3D12Resource* out = reinterpret_cast<ID3D12Resource*>(1);
        const HRESULT hr = call_slot133(proxy, raw, &d, kRetailStateWord, nullptr, &out);
        if (out && out != reinterpret_cast<ID3D12Resource*>(1)) out->Release();
        VirtualFree(raw, 0, MEM_RELEASE);
        EXPECT(hr == E_NOTIMPL && out == nullptr && nothing_held(gpu.device));
    }
    if (failures) return failures;
    std::printf("%s: write-combine window mapped as cached views (window keeps 0x%lx); the exact tuple (flags 0x20004, "
                "state word 0xFFFFFFFF) imported as UAV / COMMON; CPU before/after -> GPU copy and GPU copy -> CPU at "
                "the same VA match; alias import, unmap, remap, free and VirtualProtect refused while live; near misses "
                "refused; nothing held after Release. Hypothesis only: copies, not V# or shader UAV writes\n",
                who, static_cast<unsigned long>(logical));
    return 0;
}

// ---- Device slot 70 over a real host import ----
//
// A title-backed descriptor heap (slot 14 with a test allocator whose backing is real memory), a real import of title
// pages through raw slot 133, then raw slot 70 with the measured raw-buffer shape at a smaller width. The descriptor the
// layer writes into the real heap must be the one PC slot 18 writes for the same live buffer and byte range: a raw V#
// whose base is the title VA and whose record count is the width. Refusals leave a canary in the slot.
//
// NOT covered here: a shader reading through this V#. The translated shader contract adds heap_base to a V#'s base
// (docs/d3d12_vulkan.md 3.3), and a title-page buffer's base is the title's host VA, so such a read misses the import;
// dispatching one would read an unrelated device address. d3d12_compute_test reads a slot-70 view of an arena buffer,
// which that contract does reach.

namespace xs = rt::d3d12_xs;

xs::DescriptorBackingStatus host_backing_allocate(std::size_t size, std::uint64_t, xs::DescriptorHeapBacking* out) {
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return xs::DescriptorBackingStatus::out_of_memory;
    out->address = p;
    out->token = 0;
    out->release_hook = nullptr;
    return xs::DescriptorBackingStatus::allocated;
}
void host_backing_release(const xs::DescriptorHeapBacking& backing, std::uint64_t) {
    VirtualFree(backing.address, 0, MEM_RELEASE);
}

// The measured slot-70 shape (docs/d3d12_vulkan.md, "Device slot 70") at any width: the resource desc and the 64-byte
// view desc as the title lays them out, with garbage in the padding and in the view's unread bytes 0x30..0x3F.
struct Slot70Args {
    alignas(8) std::uint8_t resource[sizeof(D3D12_RESOURCE_DESC)];
    alignas(8) std::uint8_t view[64];
};

Slot70Args slot70_args(std::uint64_t va, std::uint64_t width) {
    Slot70Args a{};
    D3D12_RESOURCE_DESC r = buffer_desc(width);
    std::memcpy(a.resource, &r, sizeof(r));
    const std::uint32_t padding = 0xDEADBEEFu;
    std::memcpy(a.resource + 4, &padding, 4);
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_R32_TYPELESS;
    v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Buffer.NumElements = static_cast<UINT>(width / 4);
    v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    std::memcpy(a.view, &v, sizeof(v));
    std::memcpy(a.view + 0x24, &padding, 4);
    std::memcpy(a.view + 0x28, &va, 8);
    std::memset(a.view + 0x30, 0xA5, 0x10);
    return a;
}

void call_slot70(void* proxy, const Slot70Args& args, std::uint64_t destination) {
    using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
    void* const* vtable = *static_cast<void* const* const*>(proxy);
    reinterpret_cast<SlotFn>(vtable[70])(proxy, reinterpret_cast<std::uint64_t>(args.resource),
                                         reinterpret_cast<std::uint64_t>(args.view), destination);
}

int check_slot70(Gpu& gpu, void* proxy, const Title& title) {
    const char* who = "slot 70 raw SRV over a host import";
    constexpr UINT64 kWidth = k64;
    auto* const pages = static_cast<std::uint32_t*>(
        title.alloc(nullptr, kWidth, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    auto* const elsewhere = title.alloc(nullptr, k64, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    EXPECT(pages && elsewhere);
    for (UINT64 i = 0; i < kWidth / 4; ++i) pages[i] = 0x70000000u + static_cast<std::uint32_t>(i) * 3u;
    const auto va = reinterpret_cast<std::uint64_t>(pages);

    // The import, as the title makes it: raw slot 133, a row-major UAV buffer on its pages.
    D3D12_RESOURCE_DESC xbox = buffer_desc(kWidth);
    xbox.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* placed = nullptr;
    EXPECT_HR(call_slot133(proxy, pages, &xbox, kCommon, nullptr, &placed), S_OK);
    EXPECT(placed && placed->GetGPUVirtualAddress() == va);

    // A title-backed shader-visible heap: slot 9 answers the title's address, the real heap is behind it.
    const xs::DescriptorHeapAllocator allocator{&host_backing_allocate, &host_backing_release};
    xs::set_descriptor_heap_allocator(&allocator);
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT heap_hr = gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p));
    xs::set_descriptor_heap_allocator(nullptr);
    EXPECT_HR(heap_hr, S_OK);
    using HeapHandleFn = std::uint64_t(WINAPI*)(void*);  // heap slot 9, the Xbox form: the handle in RAX
    const std::uint64_t title_cpu = reinterpret_cast<HeapHandleFn>((*reinterpret_cast<void***>(heap.p))[9])(heap.p);
    ID3D12DescriptorHeap* const real_heap = xs::heap_proxy_real_heap(heap.p);
    EXPECT(real_heap && title_cpu && xs::title_descriptor_range_count() >= 1);
    const std::uint64_t real_cpu = real_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    const UINT inc = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    EXPECT(real_cpu && real_cpu != title_cpu && inc >= 32);
    const auto real_slot = [&](UINT i) { return reinterpret_cast<std::uint8_t*>(real_cpu + i * inc); };
    std::vector<std::uint8_t> title_before(4 * inc, 0xC5);
    std::memcpy(reinterpret_cast<void*>(title_cpu), title_before.data(), title_before.size());
    for (UINT i = 0; i < 4; ++i) std::memset(real_slot(i), 0x5A, inc);

    // The measured shape, scaled to this buffer: the whole import by its base VA, into title slot 1.
    xs::reset_srv_at_va_stats();
    const Slot70Args args = slot70_args(va, kWidth);
    Slot70Args call = args;
    call_slot70(proxy, call, title_cpu + 1 * inc);
    EXPECT(std::memcmp(&call, &args, sizeof(args)) == 0);
    EXPECT(xs::last_srv_at_va_outcome() == xs::SrvAtVaOutcome::created && xs::srv_at_va_created_calls() == 1);
    // The real slot holds a raw V# of exactly the title VA and width; the title's backing bytes are not written.
    palmwave::gfx10::BufferWords words{};
    std::memcpy(words.data(), real_slot(1), sizeof(words));
    palmwave::gfx10::Buffer decoded{};
    EXPECT(palmwave::gfx10::decode(words, decoded));
    EXPECT(decoded.address == va && decoded.num_records == kWidth && decoded.stride == 0 &&
           decoded.oob_select == palmwave::gfx10::OobSelect::raw &&
           decoded.format == palmwave::gfx10::format_from_vulkan(VK_FORMAT_R32_UINT));
    EXPECT(std::memcmp(reinterpret_cast<const void*>(title_cpu), title_before.data(), title_before.size()) == 0);
    // The oracle: PC slot 18 (CreateShaderResourceView) for the same live buffer and range writes the same bytes.
    D3D12_SHADER_RESOURCE_VIEW_DESC pc{};
    pc.Format = DXGI_FORMAT_R32_TYPELESS;
    pc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    pc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    pc.Buffer = {0, static_cast<UINT>(kWidth / 4), 0, D3D12_BUFFER_SRV_FLAG_RAW};
    gpu.device->CreateShaderResourceView(placed, &pc, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(title_cpu + 2 * inc)});
    EXPECT(std::memcmp(real_slot(1), real_slot(2), inc) == 0);
    // The resolver's reference was released: only ours is left.
    placed->AddRef();
    EXPECT(placed->Release() == 1);

    // Refusals leave the real slot's canary: a range one element past the import (never cut to fit), title memory
    // with no import, a misaligned VA. Nothing is held for them.
    std::vector<std::uint8_t> canary(inc, 0x5A);
    const auto intact = [&](const Slot70Args& a, xs::SrvAtVaOutcome want) {
        call_slot70(proxy, a, title_cpu + 3 * inc);
        return xs::last_srv_at_va_outcome() == want && std::memcmp(real_slot(3), canary.data(), inc) == 0;
    };
    EXPECT(intact(slot70_args(va, kWidth + 16), xs::SrvAtVaOutcome::unknown_va));
    EXPECT(intact(slot70_args(reinterpret_cast<std::uint64_t>(elsewhere), 0x100), xs::SrvAtVaOutcome::unknown_va));
    EXPECT(intact(slot70_args(va + 4, 0x100), xs::SrvAtVaOutcome::invalid_range));

    // CPU writes to the pages do not change the descriptor (it still names the title VA). This rereads the descriptor
    // only: it is not evidence that a shader sees those bytes (see NOT covered above).
    for (UINT64 i = 0; i < kWidth / 4; ++i) pages[i] = ~(0x70000000u + static_cast<std::uint32_t>(i) * 3u);
    std::memcpy(words.data(), real_slot(1), sizeof(words));
    EXPECT(palmwave::gfx10::decode(words, decoded) && decoded.address == va);

    // After the last Release the VA names nothing: refused, canary intact, nothing imported or pinned.
    placed->Release();
    placed = nullptr;
    EXPECT(nothing_held(gpu.device));
    EXPECT(intact(args, xs::SrvAtVaOutcome::unknown_va));
    heap.reset();
    title.free(elsewhere, 0, MEM_RELEASE);
    title.free(pages, 0, MEM_RELEASE);
    std::printf("%s: the title-backed slot holds PC slot 18's raw V# for the live import (VA 0x%llx, %llu bytes), the "
                "title backing is not written, the reference is balanced; past-the-end, unimported, misaligned and "
                "released VAs leave the slot untouched. NOT RUN: a shader read through this V# (heap_base + title VA, "
                "docs/d3d12_vulkan.md 3.3)\n",
                who, static_cast<unsigned long long>(va), static_cast<unsigned long long>(kWidth));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const bool experimental = argc == 2 && std::strcmp(argv[1], "--experimental") == 0;
    if (argc > 2 || (argc == 2 && !experimental)) return 2;
    // The mode names the environment CTest gave this process; the test only reads it.
    if (rt::experimental_host_import::enabled() != experimental) {
        std::fprintf(stderr, "PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT does not match the mode (%s)\n",
                     experimental ? "--experimental needs exactly 1" : "the default run needs it unset or not 1");
        return 1;
    }
    // The experimental run records its D3D12 trace (as palmwave --trace-d3d12 does), so the "experimental" outcome of
    // every slot-133 attempt is in the output.
    if (experimental) rt::d3d12_trace::set_enabled(true);
    vktest::Device vk;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;
    if (!vk.host_pointer_alignment) {
        std::printf("SKIP: the Vulkan device has no VK_EXT_external_memory_host\n");
        return 77;
    }
    std::printf("VK_EXT_external_memory_host: minImportedHostPointerAlignment %llu\n",
                static_cast<unsigned long long>(vk.host_pointer_alignment));
    const Title title{
        reinterpret_cast<TitleVirtualAllocFn>(rt::shims::find("kernel32.dll", "VirtualAlloc")),
        reinterpret_cast<TitleVirtualFreeFn>(rt::shims::find("kernel32.dll", "VirtualFree")),
        reinterpret_cast<TitleVirtualProtectFn>(rt::shims::find("kernel32.dll", "VirtualProtect")),
        reinterpret_cast<TitleUnmapViewOfFileFn>(rt::shims::find("kernel32.dll", "UnmapViewOfFile")),
        reinterpret_cast<XMemAllocFn>(rt::shims::find("xmem.dll", "XMemAlloc")),
        reinterpret_cast<XMemFreeFn>(rt::shims::find("xmem.dll", "XMemFree")),
    };
    const XMem xmem{
        reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc")),
        reinterpret_cast<XMemAllocatePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages")),
        reinterpret_cast<XMemFreePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages")),
        reinterpret_cast<XMemMapPhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages")),
    };
    if (!title.alloc || !title.free || !title.protect || !title.unmap || !title.xmem_alloc || !title.xmem_free ||
        !xmem.virtual_alloc || !xmem.allocate || !xmem.free_pages || !xmem.map) {
        std::fprintf(stderr, "the xmem and title-memory shims are not registered in this executable\n");
        return 1;  // a build defect, not a missing capability
    }
    if (!rt::host_pins::registry().provider.load()) {
        std::fprintf(stderr, "no host-page lifetime provider is linked\n");
        return 1;
    }

    int failures = 0;
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = vk.instance_proc;
        info.instance = vk.instance;
        info.physical = vk.physical;
        info.device = vk.device;
        info.queue = vk.queue;
        info.queue_family = vk.family;
        info.buffer_device_address = vk.buffer_device_address;
        info.host_pointer_alignment = vk.host_pointer_alignment;
        ID3D12Device* inner = nullptr;
        if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner)))) return 1;
        Ref<ID3D12Device> device;
        if (FAILED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                     reinterpret_cast<void**>(&device.p)))) {
            inner->Release();
            return 1;
        }
        rt::d3d12::set_device_outer(inner, device.p);
        {
            Gpu gpu;
            if (gpu.init(device.p)) return 1;
            failures += check_round_trips(gpu, title, vk.host_pointer_alignment);
            failures += check_unmanaged(gpu);
            failures += check_shared(gpu, title);
            failures += check_xmem_lifecycle(gpu, xmem, title);
            failures += check_refusals(gpu, title);
            failures += check_proxy(gpu, device.p, title);
            failures += check_slot70(gpu, device.p, title);
            if (experimental) failures += check_experimental(gpu, device.p, xmem, title);
        }
    }
    if (vk.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", vk.validation_errors());
        return 1;
    }
    std::printf("d3d12_host_placed_test: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
