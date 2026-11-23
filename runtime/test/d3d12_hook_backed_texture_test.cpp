// Device slot 127's double opt-in hook-backed texture EXPERIMENT (PALMWAVE_EXPERIMENTAL_XBOX_COMMITTED_TEXTURE=1 and
// PALMWAVE_EXPERIMENTAL_XBOX_TEXTURE_HOOK_BACKING=1) on the real Vulkan D3D12 backend, through the Xbox-ABI device proxy.
// A diagnostic under test, not a console ABI. It checks that the ONE linear VkImage the route creates is bound to the
// very pages the page provider returned, that it behaves as the PC RT|UAV R8G8B8A8_UNORM 3840x2160 texture for
// same-image RTV clear, copy, UAV write and SRV read, and the lifetime contract (d3d12_xs_device.h TexturePageProvider,
// src/hook_backed_image.h). It cannot establish the console's layout, DCC, tiling, the attribute word's meaning or
// anything about a frame; it says nothing about the 174 MiB arena or buffer addressing.
//
// The provider is a SYNTHETIC model of the aggregated scope facts recorded for this experiment (no title code, bytes or
// offsets): an allocation scope that takes ONE 64 MiB pool from the previous handler (the served xmem!XMemAlloc) on its
// first call and returns base + cursor, cursor += align_up(size, 64 KiB); and a free scope, installed before the
// textures are released, that ignores pointers other than the pool base and forwards only the base to the previous
// handler (xmem!XMemFree). xmem keeps that physical free pending while any pin overlaps the pool.
//
//   0. Strict first: hook variable alone, a near miss, and no provider are never routed to the importer.
//   1. Round 1: A (the pool base) and B (an interior slice) created. Their pages are where the driver's subresource layout
//      says: zero after the fenced first transition (a GPU clear seen in the ORIGINAL pages). The CPU then writes a
//      pattern into B's pages; one list does the committed-texture oracle on A (UNORDERED_ACCESS -> RENDER_TARGET, RTV
//      clear, rows copied out, UAV pattern in five patches, SRV read of six patches) and reads B's CPU-written texels
//      through B's own SRV. After the fence: the copies/SRV reads are exact, A's ORIGINAL pages hold the clear and the UAV
//      pattern at the driver's row pitch, and B's SRV read returned exactly what the CPU wrote.
//      A second list reads B again and is NOT waited for: B then A are released (reverse order) inside the free scope:
//      two logical releases now (B interior ignored, A's base forwarded once), both ranges still pinned, the pool's free
//      pending. The scope is removed before the host waits for retirement. After the fence, the submitted reads were exact; after the
//      deferred group is drained the pins are gone, and no release ran after the scope ended.
//   2. Round 2 (a new pool): A2 is released FIRST, while B2 lives: the base free is forwarded and pending under B2's pin
//      (a probe pin in the pool's free tail is refused as the block is no longer live); the CPU writes B2's pages and
//      B2's SRV reads them exactly; then B2 is released and retired.
//   3. Rollback, each with a null output, the title's description unchanged, pins back to where they were: provider out
//      of memory (no release), a misaligned address under owned_allocation (released once, after unpin), memory no shim
//      owns (released once), write-combined title pages (released once), and a misaligned address under
//      at_final_release_only (NOT released: counted unreturned; the test frees its own block).
// Synthetic data only; no game data. Validation (with synchronization validation, vk_test_device.h) must be active and
// report 0 errors: CTest sets PALMWAVE_VK_VALIDATION=1 and this test fails without it.
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../shims/host_pins.h"
#include "../shims/shims.h"
#include "../src/shader_library.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#if !defined(PALMWAVE_TEST_CS_COMMITTED_TEXTURE_WRITE_SPV) || !defined(PALMWAVE_TEST_CS_COMMITTED_TEXTURE_READ_SPV)
#error "the committed-texture GLSL fixtures must be compiled and named"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
namespace xs = rt::d3d12_xs;

namespace {

constexpr char kTextureVariable[] = "PALMWAVE_EXPERIMENTAL_XBOX_COMMITTED_TEXTURE";
constexpr char kHookVariable[] = "PALMWAVE_EXPERIMENTAL_XBOX_TEXTURE_HOOK_BACKING";
constexpr UINT kWidth = 3840, kHeight = 2160;
constexpr UINT kRowBytes = kWidth * 4;
constexpr UINT kRows[3] = {0, 1080, kHeight - 1};
constexpr UINT kPatch = 64, kPatchTexels = kPatch * kPatch, kPatches = 6, kWrittenPatches = 5;
constexpr UINT kPatchOrigin[kPatches][2] = {{0, 0}, {kWidth - kPatch, 0}, {0, kHeight - kPatch},
                                            {kWidth - kPatch, kHeight - kPatch}, {1888, 1048}, {1024, 512}};
constexpr UINT64 kRowsBytes = UINT64(kRowBytes) * 3;
constexpr UINT64 kOutputBytes = UINT64(kPatches) * kPatchTexels * 4;
constexpr UINT64 kOutA = 49152, kOutB = kOutA + kOutputBytes, kOutC = kOutB + kOutputBytes;
constexpr UINT64 kReadbackBytes = kOutC + kOutputBytes;
constexpr std::uint8_t kClear[4] = {51, 102, 204, 255};
constexpr std::uint64_t kBudget = 64ull << 20, kStep = 0x10000;

using Texel = std::array<std::uint8_t, 4>;
Texel pattern(UINT x, UINT y) {  // what the UAV fixture writes
    return {std::uint8_t(x & 0xFF), std::uint8_t(y & 0xFF), std::uint8_t((x >> 8) | ((y >> 8) << 4)), 0x5A};
}
Texel cpu_pattern(UINT x, UINT y, unsigned seed) {  // what the CPU writes into a texture's pages
    return {std::uint8_t(x * 7 + y + seed), std::uint8_t((y * 3 + (x >> 4)) ^ seed), 0xC3, std::uint8_t(0x90 + seed)};
}

using Slot127Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                         std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                         std::uint64_t);
struct Request {
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_RESOURCE_DESC1 desc{};
    D3D12_CLEAR_VALUE clear{};
};
Request measured_request() {
    Request r;
    r.heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    r.heap.CreationNodeMask = 1;
    r.heap.VisibleNodeMask = 1;
    r.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    r.desc.Width = kWidth;
    r.desc.Height = kHeight;
    r.desc.DepthOrArraySize = 1;
    r.desc.MipLevels = 1;
    r.desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    r.desc.SampleDesc = {1, 0};
    r.desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    r.desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x00300005);
    r.clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    return r;
}
HRESULT call127(void* proxy, Request& r, ID3D12Resource** out) {
    static const IID resource_iid = __uuidof(ID3D12Resource);
    const auto fn = reinterpret_cast<Slot127Fn>(slot(proxy, 127));
    return static_cast<HRESULT>(static_cast<std::uint32_t>(
        fn(proxy, reinterpret_cast<std::uint64_t>(&r.heap), std::uint64_t(D3D12_HEAP_FLAG_ALLOW_DISPLAY),
           reinterpret_cast<std::uint64_t>(&r.desc), 3, reinterpret_cast<std::uint64_t>(&r.clear), 0, 0, 0,
           reinterpret_cast<std::uint64_t>(&resource_iid), reinterpret_cast<std::uint64_t>(out))));
}

class EnvRestore {
public:
    explicit EnvRestore(const char* name) : name_(name) {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetEnvironmentVariableA(name_, value_, sizeof(value_));
        present_ = n != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    }
    ~EnvRestore() { SetEnvironmentVariableA(name_, present_ ? value_ : nullptr); }
    EnvRestore(const EnvRestore&) = delete;
    EnvRestore& operator=(const EnvRestore&) = delete;

private:
    const char* name_;
    char value_[64] = {};
    bool present_ = false;
};

// ---- The served previous handler and the synthetic scope ----
using XMemAllocFn = void*(__stdcall*)(size_t, uint64_t);
using XMemFreeFn = void(__stdcall*)(void*, uint64_t);
using TitleVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, DWORD);
using TitleVirtualFreeFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD);
XMemAllocFn g_xmem_alloc = nullptr;
XMemFreeFn g_xmem_free = nullptr;
TitleVirtualAllocFn g_title_alloc = nullptr;
TitleVirtualFreeFn g_title_free = nullptr;

struct Scope {
    bool alloc_active = false, free_active = false;
    std::uint8_t* base = nullptr;
    std::uint64_t cursor = 0;
    unsigned pools = 0, allocations = 0, foreign = 0, frees = 0, interior_ignored = 0, base_forwarded = 0,
             after_scope = 0;
    void open() {
        alloc_active = true;
        base = nullptr;
        cursor = 0;
    }
    void close() {  // removed, and its pool base reset before any recreation
        alloc_active = free_active = false;
        base = nullptr;
        cursor = 0;
    }
};
Scope g_scope;

xs::DescriptorBackingStatus scope_allocate(std::size_t size, std::uint64_t attributes, xs::DescriptorHeapBacking* out) {
    if (!g_scope.alloc_active || (attributes & 0xc000) != 0x8000) {
        ++g_scope.foreign;
        return xs::DescriptorBackingStatus::not_hooked;
    }
    if (!g_scope.base) {
        g_scope.base = static_cast<std::uint8_t*>(g_xmem_alloc(kBudget, attributes));  // the previous handler
        if (!g_scope.base) return xs::DescriptorBackingStatus::out_of_memory;
        ++g_scope.pools;
    }
    const std::uint64_t step = (std::uint64_t(size) + kStep - 1) & ~(kStep - 1);
    if (step > kBudget - g_scope.cursor) return xs::DescriptorBackingStatus::out_of_memory;
    out->address = g_scope.base + g_scope.cursor;
    out->token = 0;
    out->release_hook = nullptr;
    g_scope.cursor += step;
    ++g_scope.allocations;
    return xs::DescriptorBackingStatus::allocated;
}
void scope_free(const xs::DescriptorHeapBacking& backing, std::uint64_t attributes) {
    ++g_scope.frees;
    if (!g_scope.free_active) {
        ++g_scope.after_scope;  // would have reached a default free with this pointer: a defect
        return;
    }
    if (backing.address != g_scope.base) {
        ++g_scope.interior_ignored;
        return;
    }
    ++g_scope.base_forwarded;
    g_xmem_free(g_scope.base, attributes);  // pending in xmem while a pin overlaps the pool
}
constexpr xs::TexturePageProvider kScopeProvider{&scope_allocate, &scope_free,
                                                 xs::TexturePageReleaseContract::at_final_release_only};

// Failure providers: one allocation per call, freed by their own release.
enum class Mode { out_of_memory, misaligned, unmanaged, write_combined };
struct Failing {
    Mode mode = Mode::out_of_memory;
    unsigned allocations = 0, releases = 0;
    void* block = nullptr;
};
Failing g_failing;
xs::DescriptorBackingStatus failing_allocate(std::size_t size, std::uint64_t, xs::DescriptorHeapBacking* out) {
    ++g_failing.allocations;
    switch (g_failing.mode) {
    case Mode::out_of_memory: return xs::DescriptorBackingStatus::out_of_memory;
    case Mode::misaligned:
        g_failing.block = g_xmem_alloc(size + kStep, 0);
        out->address = g_failing.block ? static_cast<std::uint8_t*>(g_failing.block) + 64 : nullptr;
        break;
    case Mode::unmanaged:
        g_failing.block = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);  // the host's, no shim
        out->address = g_failing.block;
        break;
    case Mode::write_combined:
        g_failing.block = g_title_alloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_WRITECOMBINE);
        out->address = g_failing.block;
        break;
    }
    return out->address ? xs::DescriptorBackingStatus::allocated : xs::DescriptorBackingStatus::out_of_memory;
}
void free_failing_block() {
    if (!g_failing.block) return;
    switch (g_failing.mode) {
    case Mode::misaligned: g_xmem_free(g_failing.block, 0); break;
    case Mode::unmanaged: VirtualFree(g_failing.block, 0, MEM_RELEASE); break;
    case Mode::write_combined: g_title_free(g_failing.block, 0, MEM_RELEASE); break;
    default: break;
    }
    g_failing.block = nullptr;
}
void failing_release(const xs::DescriptorHeapBacking&, std::uint64_t) {
    ++g_failing.releases;
    free_failing_block();
}
constexpr xs::TexturePageProvider kOwnedFailing{&failing_allocate, &failing_release,
                                                xs::TexturePageReleaseContract::owned_allocation};
constexpr xs::TexturePageProvider kFinalOnlyFailing{&failing_allocate, &failing_release,
                                                    xs::TexturePageReleaseContract::at_final_release_only};

// ---- D3D12 helpers ----
D3D12_TEXTURE_BARRIER texture_barrier(ID3D12Resource* resource, D3D12_BARRIER_SYNC sync_before,
                                      D3D12_BARRIER_SYNC sync_after, D3D12_BARRIER_ACCESS access_before,
                                      D3D12_BARRIER_ACCESS access_after, D3D12_BARRIER_LAYOUT layout_before,
                                      D3D12_BARRIER_LAYOUT layout_after) {
    D3D12_TEXTURE_BARRIER b{};
    b.SyncBefore = sync_before;
    b.SyncAfter = sync_after;
    b.AccessBefore = access_before;
    b.AccessAfter = access_after;
    b.LayoutBefore = layout_before;
    b.LayoutAfter = layout_after;
    b.pResource = resource;
    b.Subresources = {0xffffffff, 0, 0, 0, 0, 0};
    return b;
}
void barrier(ID3D12GraphicsCommandList7* list, const D3D12_TEXTURE_BARRIER& b) {
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &b;
    list->Barrier(1, &group);
}
void buffer_to_copy(ID3D12GraphicsCommandList7* list, ID3D12Resource* buffer) {
    D3D12_BUFFER_BARRIER b{};
    b.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    b.SyncAfter = D3D12_BARRIER_SYNC_COPY;
    b.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    b.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    b.pResource = buffer;
    b.Size = UINT64_MAX;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = 1;
    group.pBufferBarriers = &b;
    list->Barrier(1, &group);
}
void sr_barrier(ID3D12GraphicsCommandList7* list, ID3D12Resource* texture) {  // initial UNORDERED_ACCESS -> SR
    barrier(list, texture_barrier(texture, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                  D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                                  D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE));
}

std::uint8_t* texel_at(const rt::d3d12::HookBackedTextureInfo& info, UINT x, UINT y) {
    return static_cast<std::uint8_t*>(info.address) + info.offset + std::uint64_t(y) * info.row_pitch +
           std::uint64_t(x) * 4;
}
void write_patches(const rt::d3d12::HookBackedTextureInfo& info, unsigned seed) {
    for (UINT p = 0; p < kPatches; ++p)
        for (UINT i = 0; i < kPatchTexels; ++i) {
            const UINT x = kPatchOrigin[p][0] + i % kPatch, y = kPatchOrigin[p][1] + i / kPatch;
            const Texel t = cpu_pattern(x, y, seed);
            std::memcpy(texel_at(info, x, y), t.data(), 4);
        }
}
unsigned check_cpu_patches(const char* what, const std::uint8_t* output, unsigned seed) {
    unsigned bad = 0;
    for (UINT p = 0; p < kPatches; ++p)
        for (UINT i = 0; i < kPatchTexels; ++i) {
            const UINT x = kPatchOrigin[p][0] + i % kPatch, y = kPatchOrigin[p][1] + i / kPatch;
            const Texel want = cpu_pattern(x, y, seed);
            const std::uint8_t* got = output + (std::size_t(p) * kPatchTexels + i) * 4;
            if (std::memcmp(got, want.data(), 4) && bad++ < 6)
                std::fprintf(stderr, "%s (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", what, x, y, got[0], got[1],
                             got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    return bad;
}
bool range_pinned(const rt::d3d12::HookBackedTextureInfo& info) {
    const auto begin = reinterpret_cast<std::uintptr_t>(info.address);
    return rt::host_pins::is_pinned(begin, begin + info.bytes);
}

}  // namespace

int main() {
    if (!GetEnvironmentVariableA("PALMWAVE_VK_VALIDATION", nullptr, 0)) {
        std::fprintf(stderr, "d3d12_hook_backed_texture_test needs PALMWAVE_VK_VALIDATION=1 (synchronization validation)\n");
        return 1;
    }
    vktest::Device d;
    d.request_buffer_device_address = true;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;
    if (device_status != vktest::DeviceStatus::ready) return 1;
    if (!d.bindless) {
        std::printf("SKIP: the device lacks the descriptor indexing features of the bindless set 0\n");
        return 77;
    }
    if (!d.host_pointer_alignment) {
        std::printf("SKIP: the Vulkan device has no VK_EXT_external_memory_host\n");
        return 77;
    }
    {
        VkPhysicalDeviceProperties props{};
        d.instance_fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(d.physical, &props);
        std::printf("d3d12_hook_backed_texture_test: GPU %s, driver 0x%08x, minImportedHostPointerAlignment %llu\n",
                    props.deviceName, props.driverVersion, static_cast<unsigned long long>(d.host_pointer_alignment));
    }
    g_xmem_alloc = reinterpret_cast<XMemAllocFn>(rt::shims::find("xmem.dll", "XMemAlloc"));
    g_xmem_free = reinterpret_cast<XMemFreeFn>(rt::shims::find("xmem.dll", "XMemFree"));
    g_title_alloc = reinterpret_cast<TitleVirtualAllocFn>(rt::shims::find("kernel32.dll", "VirtualAlloc"));
    g_title_free = reinterpret_cast<TitleVirtualFreeFn>(rt::shims::find("kernel32.dll", "VirtualFree"));
    EXPECT(g_xmem_alloc && g_xmem_free && g_title_alloc && g_title_free);
    EXPECT(rt::host_pins::registry().provider.load() != nullptr);
    const EnvRestore restore_texture(kTextureVariable), restore_hook(kHookVariable);

    const auto write_program = fssm_program({0x43540001, 0xbf810000});
    const auto read_program = fssm_program({0x43540002, 0xbf810000});
    const auto write_container = shader_container(write_program);
    const auto read_container = shader_container(read_program);
    const testfiles::ScratchDir scratch("palmwave_hook_backed_texture_test");
    const auto& library = scratch.path();
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(PALMWAVE_TEST_CS_COMMITTED_TEXTURE_WRITE_SPV),
                                library / "cs_committed_texture_write.spv"));
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(PALMWAVE_TEST_CS_COMMITTED_TEXTURE_READ_SPV),
                                library / "cs_committed_texture_read.spv"));
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"cs_committed_texture_write\": {\"stage\": \"cs\", \"fssm_sha256\": \""
              << palmwave::sha256_hex(write_program)
              << "\", \"bindings\": [2, 8], \"push_constant_bytes\": 144, \"local_size\": [8, 8, 1], "
                 "\"uses_code_base\": false, \"wave64\": false}, "
                 "\"cs_committed_texture_read\": {\"stage\": \"cs\", \"fssm_sha256\": \""
              << palmwave::sha256_hex(read_program)
              << "\", \"bindings\": [0, 2], \"push_constant_bytes\": 144, \"local_size\": [8, 8, 1], "
                 "\"uses_code_base\": false, \"wave64\": false}}";
    }

    std::vector<std::uint8_t> rows(kRowsBytes), out_a(kOutputBytes), out_b(kOutputBytes), out_c(kOutputBytes);
    rt::d3d12::HookBackedTextureInfo info_a{}, info_b{};
    unsigned mismatches = 0;
    const std::size_t pins_at_start = rt::host_pins::pin_count();
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.bindless = d.bindless;
        info.bindless_partially_bound = d.partially_bound;
        info.image_view_min_lod = d.image_view_min_lod;
        info.host_pointer_alignment = d.host_pointer_alignment;
        info.shader_dir = library.string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> proxy;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&proxy.p))));
        rt::d3d12::set_device_outer(inner, proxy.p);
        if (rt::d3d12::device_push_constants_need_fallback(inner)) {
            std::printf("SKIP: the PSOs need 144 push-constant bytes and this GPU has %u\n",
                        rt::d3d12::device_max_push_constants_size(inner));
            return 77;
        }
        xs::set_texture_page_provider(nullptr);

        // 0. Strict first. Hook variable without the texture variable; a near miss; no provider.
        {
            const auto routed = xs::hook_backed_texture_routed_calls();
            EXPECT(SetEnvironmentVariableA(kTextureVariable, nullptr) != FALSE || GetLastError() == ERROR_ENVVAR_NOT_FOUND);
            EXPECT(SetEnvironmentVariableA(kHookVariable, "1") != FALSE);
            xs::set_texture_page_provider(&kScopeProvider);
            g_scope.open();
            ID3D12Resource* probe = reinterpret_cast<ID3D12Resource*>(1);
            Request strict = measured_request();
            EXPECT(call127(proxy.p, strict, &probe) == E_INVALIDARG && probe == nullptr);
            EXPECT(SetEnvironmentVariableA(kTextureVariable, "1") != FALSE);
            Request near_miss = measured_request();
            near_miss.desc.Height = kHeight - 1;
            probe = reinterpret_cast<ID3D12Resource*>(1);
            EXPECT(call127(proxy.p, near_miss, &probe) == E_INVALIDARG && probe == nullptr);
            EXPECT(xs::hook_backed_texture_routed_calls() == routed && g_scope.allocations == 0);
            xs::set_texture_page_provider(nullptr);  // the test binary links no default bridge
            const auto refused = xs::hook_backed_texture_refused_calls();
            Request no_provider = measured_request();
            probe = reinterpret_cast<ID3D12Resource*>(1);
            EXPECT(call127(proxy.p, no_provider, &probe) == E_NOTIMPL && probe == nullptr);
            EXPECT(xs::hook_backed_texture_refused_calls() == refused + 1 && g_scope.allocations == 0);
            g_scope.close();
        }

        // Root signature, PSOs, views, buffers (as d3d12_committed_texture_test), with three SRV slots.
        const D3D12_DESCRIPTOR_RANGE view_ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0},
                                                       {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 1}};
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {1, 0};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {2, view_ranges};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(inner->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));
        Ref<ID3D12PipelineState> write_pso, read_pso;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.CS = {write_container.data(), write_container.size()};
        EXPECT(SUCCEEDED(inner->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&write_pso.p))));
        pso_desc.CS = {read_container.data(), read_container.size()};
        EXPECT(SUCCEEDED(inner->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&read_pso.p))));
        Ref<ID3D12DescriptorHeap> rtv_heap, view_heap;
        const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC view_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3,
                                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        EXPECT(SUCCEEDED(inner->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        EXPECT(SUCCEEDED(inner->CreateDescriptorHeap(&view_heap_desc, IID_PPV_ARGS(&view_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const UINT step = inner->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE view_cpu = view_heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_DESCRIPTOR_HANDLE view_gpu = view_heap->GetGPUDescriptorHandleForHeapStart();
        const auto make_srv = [&](ID3D12Resource* texture, UINT index) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D = {0, 1, 0, 0.0f};
            inner->CreateShaderResourceView(texture, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{view_cpu.ptr + index * step});
            std::uint32_t words[8] = {};
            std::memcpy(words, reinterpret_cast<const void*>(view_cpu.ptr + index * step), sizeof(words));
            return rt::d3d12::bindless_hash_lookup(inner, words) != 0;
        };
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> buf_a, buf_b, buf_c, readback;
        const auto out_desc = buffer_desc(kOutputBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        for (Ref<ID3D12Resource>* b : {&buf_a, &buf_b, &buf_c})
            EXPECT(SUCCEEDED(inner->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &out_desc,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&b->p))));
        const auto readback_desc = buffer_desc(kReadbackBytes);
        EXPECT(SUCCEEDED(inner->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&readback.p))));
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12GraphicsCommandList7> list7;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(inner->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(inner->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(inner->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                  IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list7.p))));
        EXPECT(SUCCEEDED(inner->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        UINT64 fence_value = 0;
        const auto submit = [&]() -> bool {
            if (list->Close() != S_OK) return false;
            ID3D12CommandList* lists[] = {list.p};
            queue->ExecuteCommandLists(1, lists);
            return rt::d3d12::last_execute_result(queue.p) == palmwave::ReplayResult::success &&
                   SUCCEEDED(queue->Signal(fence.p, ++fence_value));
        };
        const auto wait = [&]() { return SUCCEEDED(fence->SetEventOnCompletion(fence_value, nullptr)); };
        const auto reopen = [&]() {
            return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, nullptr));
        };
        // Retires deferred groups: an empty submission, its fence, then another (whose acquire drains what retired).
        const auto drain = [&]() {
            for (int i = 0; i < 3; ++i)
                if (!reopen() || !submit() || !wait()) return false;
            return true;
        };
        const auto read_patches = [&](ID3D12Resource* out, UINT index) {
            list->SetDescriptorHeaps(1, &view_heap.p);
            list->SetComputeRootSignature(root.p);
            list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
            list->SetComputeRootDescriptorTable(2, view_gpu);
            list->SetPipelineState(read_pso.p);
            for (UINT p = 0; p < kPatches; ++p) {
                const std::uint32_t constants[2] = {index * step / 4, p};
                list->SetComputeRoot32BitConstants(0, 2, constants, 0);
                list->Dispatch(kPatch / 8, kPatch / 8, 1);
            }
        };
        const auto copy_out = [&](ID3D12Resource* out, UINT64 offset) {
            buffer_to_copy(list7.p, out);
            list->CopyBufferRegion(readback.p, offset, out, 0, kOutputBytes);
        };
        const auto map_readback = [&](UINT64 offset, std::vector<std::uint8_t>& into) {
            void* mapped = nullptr;
            if (FAILED(readback->Map(0, nullptr, &mapped)) || !mapped) return false;
            std::memcpy(into.data(), static_cast<const std::uint8_t*>(mapped) + offset, into.size());
            readback->Unmap(0, nullptr);
            return true;
        };

        // 1. Round 1: two slices of one pool.
        EXPECT(SetEnvironmentVariableA(kTextureVariable, "1") != FALSE && SetEnvironmentVariableA(kHookVariable, "1") != FALSE);
        xs::set_texture_page_provider(&kScopeProvider);
        g_scope.open();
        Request request = measured_request();
        Ref<ID3D12Resource> tex_a, tex_b;
        EXPECT(call127(proxy.p, request, &tex_a.p) == S_OK && tex_a.p);
        EXPECT(call127(proxy.p, request, &tex_b.p) == S_OK && tex_b.p);
        EXPECT(static_cast<std::uint32_t>(request.desc.Flags) == 0x00300005u);  // the title's bytes are not written
        EXPECT(g_scope.pools == 1 && g_scope.allocations == 2);                  // one allocation per texture, no more
        EXPECT(rt::d3d12::hook_backed_texture_info(tex_a.p, &info_a) && rt::d3d12::hook_backed_texture_info(tex_b.p, &info_b));
        {
            const D3D12_RESOURCE_DESC got = tex_a->GetDesc();
            EXPECT(got.Width == kWidth && got.Height == kHeight && got.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
                   static_cast<UINT>(got.Flags) == 0x5u);
            const std::uint64_t rounded = (info_a.requirement_size + d.host_pointer_alignment - 1) &
                                          ~std::uint64_t(d.host_pointer_alignment - 1);
            EXPECT(info_a.address == g_scope.base && info_a.bytes == rounded);  // the real size, rounded to the import
            EXPECT(info_b.address == g_scope.base + ((info_a.bytes + kStep - 1) & ~(kStep - 1)));
            constexpr VkMemoryPropertyFlags coherent =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            EXPECT((info_a.memory_flags & coherent) == coherent && (info_a.image_memory_types & (1u << info_a.memory_type)));
            EXPECT(info_a.row_pitch >= kRowBytes && info_a.offset + info_a.layout_size <= info_a.requirement_size);
            EXPECT(range_pinned(info_a) && range_pinned(info_b));
            EXPECT(rt::d3d12::vulkan_image(tex_a.p) != VK_NULL_HANDLE && rt::d3d12::vulkan_image(tex_a.p) != rt::d3d12::vulkan_image(tex_b.p));
            std::printf("round 1: A %p (pool base) B %p, %llu bytes each (requirement %llu, alignment %llu), memory "
                        "type %u (image types 0x%x), offset %llu, row pitch %llu\n",
                        info_a.address, info_b.address, static_cast<unsigned long long>(info_a.bytes),
                        static_cast<unsigned long long>(info_a.requirement_size),
                        static_cast<unsigned long long>(info_a.requirement_alignment), info_a.memory_type,
                        info_a.image_memory_types, static_cast<unsigned long long>(info_a.offset),
                        static_cast<unsigned long long>(info_a.row_pitch));
        }
        // The fenced first transition zero-filled the texture: visible in A's ORIGINAL pages (rows, both ends).
        for (UINT r = 0; r < 3; ++r)
            for (UINT x = 0; x < kWidth; x += 7) {
                const std::uint8_t* t = texel_at(info_a, x, kRows[r]);
                if ((t[0] | t[1] | t[2] | t[3]) && mismatches++ < 6)
                    std::fprintf(stderr, "A pages after creation (%u, %u) not zero\n", x, kRows[r]);
            }
        EXPECT(!mismatches);
        write_patches(info_b, 1);  // after the first transition's fence: nothing discards these
        EXPECT(make_srv(tex_b.p, 2));
        inner->CreateRenderTargetView(tex_a.p, nullptr, rtv);
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            inner->CreateUnorderedAccessView(tex_a.p, nullptr, &uav, view_cpu);
            std::uint32_t words[8] = {};
            std::memcpy(words, reinterpret_cast<const void*>(view_cpu.ptr), sizeof(words));
            EXPECT(rt::d3d12::bindless_hash_lookup(inner, words) != 0);
        }
        EXPECT(make_srv(tex_a.p, 1));

        // The committed-texture oracle on A, and B's CPU-written texels through B's SRV.
        barrier(list7.p, texture_barrier(tex_a.p, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_RENDER_TARGET,
                                         D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                                         D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_RENDER_TARGET));
        const float clear_color[4] = {kClear[0] / 255.0f, kClear[1] / 255.0f, kClear[2] / 255.0f, 1.0f};
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        barrier(list7.p, texture_barrier(tex_a.p, D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_COPY,
                                         D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                         D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COPY_SOURCE));
        for (UINT r = 0; r < 3; ++r) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = UINT64(r) * kRowBytes;
            dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, 1, 1, kRowBytes};
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = tex_a.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{0, kRows[r], 0, kWidth, kRows[r] + 1, 1};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        barrier(list7.p, texture_barrier(tex_a.p, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                         D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                         D3D12_BARRIER_LAYOUT_COPY_SOURCE, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS));
        list->SetDescriptorHeaps(1, &view_heap.p);
        list->SetComputeRootSignature(root.p);
        list->SetComputeRootUnorderedAccessView(1, buf_a->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(2, view_gpu);
        list->SetPipelineState(write_pso.p);
        for (UINT p = 0; p < kWrittenPatches; ++p) {
            const std::uint32_t constants[2] = {0, p};
            list->SetComputeRoot32BitConstants(0, 2, constants, 0);
            list->Dispatch(kPatch / 8, kPatch / 8, 1);
        }
        barrier(list7.p, texture_barrier(tex_a.p, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                         D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                                         D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE));
        read_patches(buf_a.p, 1);
        sr_barrier(list7.p, tex_b.p);
        read_patches(buf_b.p, 2);
        copy_out(buf_a.p, kOutA);
        copy_out(buf_b.p, kOutB);
        EXPECT(submit() && wait());
        EXPECT(map_readback(0, rows) && map_readback(kOutA, out_a) && map_readback(kOutB, out_b));
        for (UINT r = 0; r < 3; ++r)
            for (UINT x = 0; x < kWidth; ++x) {
                const std::uint8_t* got = rows.data() + std::size_t(r) * kRowBytes + std::size_t(x) * 4;
                const std::uint8_t* page = texel_at(info_a, x, kRows[r]);
                bool in_patch = false;
                for (UINT p = 0; p < kWrittenPatches; ++p)
                    in_patch = in_patch || (x >= kPatchOrigin[p][0] && x < kPatchOrigin[p][0] + kPatch &&
                                            kRows[r] >= kPatchOrigin[p][1] && kRows[r] < kPatchOrigin[p][1] + kPatch);
                if (std::memcmp(got, kClear, 4) && mismatches++ < 6)
                    std::fprintf(stderr, "A copied row %u x %u not the clear\n", kRows[r], x);
                if (!in_patch && std::memcmp(page, kClear, 4) && mismatches++ < 6)
                    std::fprintf(stderr, "A ORIGINAL page row %u x %u not the clear\n", kRows[r], x);
            }
        for (UINT p = 0; p < kPatches; ++p)
            for (UINT i = 0; i < kPatchTexels; ++i) {
                const UINT x = kPatchOrigin[p][0] + i % kPatch, y = kPatchOrigin[p][1] + i / kPatch;
                Texel want{};
                if (p < kWrittenPatches) want = pattern(x, y);
                else std::memcpy(want.data(), kClear, 4);
                const std::uint8_t* got = out_a.data() + (std::size_t(p) * kPatchTexels + i) * 4;
                if (std::memcmp(got, want.data(), 4) && mismatches++ < 6)
                    std::fprintf(stderr, "A SRV patch %u (%u, %u) wrong\n", p, x, y);
                if (std::memcmp(texel_at(info_a, x, y), want.data(), 4) && mismatches++ < 6)
                    std::fprintf(stderr, "A ORIGINAL page patch %u (%u, %u) wrong\n", p, x, y);
            }
        mismatches += check_cpu_patches("B SRV read of CPU-written pages", out_b.data(), 1);
        EXPECT(!mismatches);
        std::printf("round 1: GPU clear/UAV writes are in A's original pages (%u row texels, %u patch texels) and B's SRV "
                    "returned the CPU's writes into its pages (%u texels)\n", 3 * kWidth, kPatches * kPatchTexels,
                    kPatches * kPatchTexels);

        // Release before the host wait: B read again, not waited for; B then A released inside the free scope.
        EXPECT(reopen());
        for (int repeat = 0; repeat < 32; ++repeat) read_patches(buf_c.p, 2);
        copy_out(buf_c.p, kOutC);
        EXPECT(submit());  // not waited for
        const auto counters_before = rt::d3d12::hook_backed_texture_counters(inner);
        g_scope.free_active = true;
        tex_b.reset();
        tex_a.reset();
        EXPECT(g_scope.frees == 2 && g_scope.interior_ignored == 1 && g_scope.base_forwarded == 1);
        const auto counters_after = rt::d3d12::hook_backed_texture_counters(inner);
        EXPECT(counters_after.logical_releases == counters_before.logical_releases + 2 && counters_after.live == 0);
        EXPECT(range_pinned(info_a) && range_pinned(info_b));  // physical lifetime still held
        g_scope.close();                                        // the scope goes before the host waits for retirement
        EXPECT(wait());
        EXPECT(map_readback(kOutC, out_c));
        mismatches += check_cpu_patches("B SRV read submitted before the logical releases", out_c.data(), 1);
        EXPECT(!mismatches);
        EXPECT(drain());
        EXPECT(!range_pinned(info_a) && !range_pinned(info_b) && rt::host_pins::pin_count() == pins_at_start);
        EXPECT(g_scope.after_scope == 0 && g_scope.frees == 2 && g_scope.base_forwarded == 1);
        std::printf("round 1: reverse release in the free scope before the host wait: interior ignored, base forwarded "
                    "once and kept pending under both pins; scope removed before retirement; pins released after the "
                    "deferred group; no release after the scope\n");

        // 2. Round 2: A2 (base) released first while B2 lives and is used.
        g_scope.open();
        Ref<ID3D12Resource> tex_a2, tex_b2;
        rt::d3d12::HookBackedTextureInfo info_a2{}, info_b2{};
        EXPECT(call127(proxy.p, request, &tex_a2.p) == S_OK && call127(proxy.p, request, &tex_b2.p) == S_OK);
        EXPECT(rt::d3d12::hook_backed_texture_info(tex_a2.p, &info_a2) &&
               rt::d3d12::hook_backed_texture_info(tex_b2.p, &info_b2));
        EXPECT(info_a2.address == g_scope.base && g_scope.pools == 2);
        const auto tail = reinterpret_cast<std::uintptr_t>(g_scope.base) + g_scope.cursor;  // the pool's unused tail
        EXPECT(g_scope.cursor + 0x1000 <= kBudget);
        EXPECT(rt::host_pins::pin(tail, tail + 0x1000) == rt::host_pins::PinResult::pinned);  // a live block
        EXPECT(rt::host_pins::unpin(tail, tail + 0x1000));
        g_scope.free_active = true;
        const unsigned forwarded = g_scope.base_forwarded;
        tex_a2.reset();
        EXPECT(g_scope.base_forwarded == forwarded + 1);
        EXPECT(rt::host_pins::pin(tail, tail + 0x1000) == rt::host_pins::PinResult::unmanaged);  // free pending
        EXPECT(range_pinned(info_b2));
        write_patches(info_b2, 2);
        EXPECT(make_srv(tex_b2.p, 2));
        EXPECT(reopen());
        sr_barrier(list7.p, tex_b2.p);
        read_patches(buf_b.p, 2);
        copy_out(buf_b.p, kOutB);
        EXPECT(submit() && wait() && map_readback(kOutB, out_b));
        mismatches += check_cpu_patches("B2 SRV read after A2 and the pool base were released", out_b.data(), 2);
        EXPECT(!mismatches);
        const unsigned interior = g_scope.interior_ignored;
        tex_b2.reset();
        EXPECT(g_scope.interior_ignored == interior + 1);
        g_scope.close();
        EXPECT(drain());
        EXPECT(!range_pinned(info_b2) && rt::host_pins::pin_count() == pins_at_start && g_scope.after_scope == 0);
        std::printf("round 2: A2 released first; the pool's free stayed pending under B2's pin; B2 kept the CPU's "
                    "writes and read them exactly; B2 released and retired\n");

        // 3. Rollback.
        const auto counters0 = rt::d3d12::hook_backed_texture_counters(inner);
        const auto rollback = [&](const xs::TexturePageProvider& provider, Mode mode, HRESULT want, unsigned releases,
                                  bool returned) -> bool {
            g_failing = {};
            g_failing.mode = mode;
            xs::set_texture_page_provider(&provider);
            Request r = measured_request();
            ID3D12Resource* out = reinterpret_cast<ID3D12Resource*>(1);
            const auto before = rt::d3d12::hook_backed_texture_counters(inner);
            const HRESULT hr = call127(proxy.p, r, &out);
            const auto after = rt::d3d12::hook_backed_texture_counters(inner);
            const bool ok = hr == want && out == nullptr && g_failing.allocations == 1 &&
                            g_failing.releases == releases && static_cast<std::uint32_t>(r.desc.Flags) == 0x00300005u &&
                            rt::host_pins::pin_count() == pins_at_start && after.live == 0 &&
                            after.unreturned_allocations == before.unreturned_allocations + (returned ? 0u : 1u);
            if (!ok)
                std::fprintf(stderr, "rollback mode %d: hr 0x%08lx (want 0x%08lx) out %p releases %u (want %u)\n",
                             int(mode), static_cast<unsigned long>(hr), static_cast<unsigned long>(want),
                             static_cast<void*>(out), g_failing.releases, releases);
            free_failing_block();  // the final-only case: the test's own block, never released by the layer
            return ok;
        };
        EXPECT(rollback(kOwnedFailing, Mode::out_of_memory, E_OUTOFMEMORY, 0, true));
        EXPECT(rollback(kOwnedFailing, Mode::misaligned, E_NOTIMPL, 1, true));
        EXPECT(rollback(kOwnedFailing, Mode::unmanaged, E_NOTIMPL, 1, true));
        EXPECT(rollback(kOwnedFailing, Mode::write_combined, E_NOTIMPL, 1, true));
        EXPECT(rollback(kFinalOnlyFailing, Mode::misaligned, E_NOTIMPL, 0, false));
        EXPECT(rt::d3d12::hook_backed_texture_counters(inner).logical_releases == counters0.logical_releases + 3);
        xs::set_texture_page_provider(nullptr);
        std::printf("rollback: out of memory, misaligned, unmanaged and write-combined pages refused with a null "
                    "output and no pin left; owned allocations released once, the final-release-only one kept and "
                    "counted\n");

        EXPECT(rt::d3d12::hook_backed_texture_counters(inner).live == 0);
    }
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("d3d12_hook_backed_texture_test: PASS (a diagnostic of the opt-in experiment: one linear image on the "
                "provider's pages; not a console layout, DCC, attribute or frame claim)\n");
    return 0;
}
